#include "history.hpp"
#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>

namespace edgecourt {
namespace {
void bounded_count(const json &metadata, const char *key, long long limit) {
    if (!metadata.contains(key) || !metadata[key].is_number_integer() || metadata[key].get<long long>() < 0 ||
        metadata[key].get<long long>() > limit)
        throw std::invalid_argument(std::string("Invalid history manifest count: ") + key);
}
json merge_logs(const json &historical, const json &current) {
    std::map<std::string, json, std::greater<std::string>> dates;
    for (const auto &log : historical) {
        auto tagged = log;
        tagged["historySource"] = "basketball_reference";
        dates[log.value("date", "")] = std::move(tagged);
    }
    // Official/current imported observations win on overlap; a reload replaces old archive observations.
    for (const auto &log : current)
        if (!log.contains("historySource"))
            dates[log.value("date", "")] = log;
    json result = json::array();
    for (const auto &entry : dates) {
        if (result.size() >= 150)
            break;
        result.push_back(entry.second);
    }
    return result;
}
} // namespace
HistoryBundle load_history_bundle(const std::filesystem::path &directory) {
    HistoryBundle bundle;
    const auto path = directory / "features.jsonl";
    if (!std::filesystem::exists(path))
        return bundle;
    if (!std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) > 128 * 1024 * 1024)
        throw std::invalid_argument("Historical feature bundle must be a regular file under 128 MB");
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("Cannot read historical feature bundle");
    // Read incrementally with a line cap rather than allocating a whole all-season dataset.
    std::string line;
    size_t line_number = 0;
    auto accept = [&]() {
        if (line.empty())
            throw std::invalid_argument("Empty record in historical feature bundle");
        ++line_number;
        auto record = json::parse(line);
        if (!record.is_object())
            throw std::invalid_argument("Historical records must be JSON objects");
        const auto kind = record.value("recordType", "");
        if (line_number == 1) {
            if (kind != "manifest" || record.value("schemaVersion", 0) != 1 ||
                record.value("source", "") != "basketball_reference" || record.value("synthetic", false))
                throw std::invalid_argument("Unsupported or synthetic history manifest");
            const auto metadata = record.at("metadata");
            bounded_count(metadata, "games", 200000);
            bounded_count(metadata, "events", 200000000);
            bounded_count(metadata, "missingGames", 200000);
            for (auto key : {"firstDate", "lastDate", "asOf"}) {
                const auto date = metadata.value(key, "");
                if ((!date.empty() && (date.size() != 10 || !parse_time(date))) ||
                    (std::string(key) == "asOf" && date.empty()))
                    throw std::invalid_argument("Invalid history manifest date");
            }
            if (metadata.value("asOf", "") > now_iso().substr(0, 10))
                throw std::invalid_argument("Historical cutoff cannot be in the future");
            const auto exported_at = metadata.value("exportedAt", "");
            if (!parse_time(exported_at) || parse_time(exported_at) > parse_time(now_iso()) + 30)
                throw std::invalid_argument("Invalid history export timestamp");
            if (metadata.value("firstDate", "") > metadata.value("lastDate", ""))
                throw std::invalid_argument("History date range is reversed");
            bundle.status = metadata;
            bundle.status["state"] = number(metadata["games"]) > 0 ? "local_history" : "empty";
            bundle.status["message"] =
                "Local, user-supplied archive features; source accuracy and completeness "
                "are not independently verified. No historical betting prices included.";
            return;
        }
        if (line_number > 1231 || (kind != "player" && kind != "team"))
            throw std::invalid_argument("Too many or unsupported historical feature records");
        const auto &feature = record.at("feature");
        auto validated = validate_import({{kind == "player" ? "players" : "teams", json::array({feature})}});
        auto key = kind == "player" ? normalize_name(feature.value("name", "")) : feature.value("abbr", "");
        const auto category = kind == "player" ? "players" : "teams";
        if (bundle.features[category].contains(key))
            throw std::invalid_argument("Duplicate historical feature identity");
        auto value = validated[category].at(key);
        value["historySource"] = "basketball_reference";
        if (kind == "player") {
            value["starterConfirmed"] = false;
            value["injury"] = "Unknown";
            for (auto key : {"starterGameId", "live", "projectedMinutes", "rosterAsOf"})
                value.erase(key);
        }
        // Export receipt is not a new observation of an old statistical environment.
        if (!feature.contains("dataAsOf"))
            throw std::invalid_argument("Historical features must preserve their original dataAsOf");
        for (const auto &log : value["logs"])
            if (log.value("date", "") >= bundle.status.value("asOf", ""))
                throw std::invalid_argument("Historical game crosses the exclusive as-of cutoff");
        if (value.contains("firstBasketHistory"))
            for (const auto &observation : value["firstBasketHistory"])
                if (observation.value("date", "") >= bundle.status.value("asOf", ""))
                    throw std::invalid_argument("First-FG opportunity crosses the exclusive as-of cutoff");
        bundle.features[category][key] = std::move(value);
    };
    char c;
    while (file.get(c)) {
        if (c == '\n') {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            accept();
            line.clear();
        } else {
            if (line.size() >= 1024 * 1024)
                throw std::invalid_argument("Historical feature record exceeds 1 MB");
            line += c;
        }
    }
    if (!file.eof())
        throw std::runtime_error("Cannot finish reading historical feature bundle");
    if (!line.empty())
        accept();
    if (!line_number)
        throw std::invalid_argument("Historical feature bundle is empty");
    if (bundle.features["players"].size() > 1200 || bundle.features["teams"].size() > 30)
        throw std::invalid_argument("Historical workspace exceeds 1200 players or 30 teams");
    bundle.status["players"] = bundle.features["players"].size();
    bundle.status["teams"] = bundle.features["teams"].size();
    return bundle;
}
void apply_history(json &snapshot, const HistoryBundle &history) {
    snapshot["historicalData"] = history.status;
    // Synthetic mode must never be represented as historical/provider observations.
    if (snapshot.value("mode", "demo") != "live")
        return;
    for (const auto &entry : history.features["players"].items()) {
        auto current = snapshot["players"].value(entry.key(), json::object());
        auto combined = entry.value();
        if (!current.empty()) {
            combined = current;
            // Preserve roster team, injuries, current-game starters and actual live stats.
            combined["logs"] = merge_logs(entry.value()["logs"], current.value("logs", json::array()));
            if (entry.value().contains("firstBasketHistory")) {
                combined["firstBasketHistory"] = entry.value()["firstBasketHistory"];
                combined.erase("firstBasketGames");
                combined.erase("firstBasketWins");
            }
            if (entry.value().contains("historicalPace") && !current.contains("historicalPace"))
                combined["historicalPace"] = entry.value()["historicalPace"];
        } else {
            combined["logs"] = merge_logs(entry.value()["logs"], json::array());
            combined["starterConfirmed"] = false;
            combined.erase("starterGameId");
        }
        combined["historySource"] = "basketball_reference";
        snapshot["players"][entry.key()] = std::move(combined);
    }
    for (const auto &entry : history.features["teams"].items()) {
        auto current = snapshot["teams"].value(entry.key(), json::object());
        auto combined = entry.value();
        if (!current.empty()) {
            for (const auto &field : current.items())
                combined[field.key()] = field.value();
            combined["logs"] = merge_logs(entry.value()["logs"], current.value("logs", json::array()));
            // Partial public team identities must not stamp archive-derived ratings fresh.
            if (!current.contains("pace") || !current.contains("offRating") || !current.contains("defRating"))
                combined["dataAsOf"] = entry.value()["dataAsOf"];
        } else {
            combined["logs"] = merge_logs(entry.value()["logs"], json::array());
        }
        combined["historySource"] = "basketball_reference";
        snapshot["teams"][entry.key()] = std::move(combined);
    }
}
} // namespace edgecourt
