#include "history_download.hpp"
#include <algorithm>
#include <cmath>
#include <deque>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace edgecourt {
namespace {
const std::vector<std::string> kStatFields = {"points",    "threes",   "threeAttempts", "fgm",      "fga",
                                              "ftm",       "fta",      "oreb",          "rebounds", "assists",
                                              "steals",    "blocks",   "turnovers",     "fouls"};
const std::map<std::string, std::vector<std::string>> kStatAliases = {
    {"points", {"points", "pts"}},
    {"threes", {"three_point_field_goals_made", "3pt_field_goals_made", "fg3"}},
    {"threeAttempts", {"three_point_field_goals_attempted", "3pt_field_goals_attempted", "fg3a"}},
    {"fgm", {"field_goals_made", "fg"}},
    {"fga", {"field_goals_attempted", "fga"}},
    {"ftm", {"free_throws_made", "ft"}},
    {"fta", {"free_throws_attempted", "fta"}},
    {"oreb", {"offensive_rebounds", "orb"}},
    {"rebounds", {"total_rebounds", "rebounds", "trb"}},
    {"assists", {"assists", "ast"}},
    {"steals", {"steals", "stl"}},
    {"blocks", {"blocks", "blk"}},
    {"turnovers", {"turnovers", "tov"}},
    {"fouls", {"personal_fouls", "fouls", "pf"}}};

long long now_epoch_seconds() { return parse_time(now_iso()); }
std::string trim_copy(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return "";
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}
std::string upper_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return value;
}
std::string hoop_team(const std::string &raw) {
    static const std::map<std::string, std::string> aliases = {{"GS", "GSW"}, {"NY", "NYK"}, {"SA", "SAS"},
                                                               {"NO", "NOP"}, {"UTAH", "UTA"}, {"WSH", "WAS"}};
    auto value = upper_copy(trim_copy(raw));
    const auto found = aliases.find(value);
    return found == aliases.end() ? value : found->second;
}
bool truthy(const std::string &raw) {
    const auto value = upper_copy(trim_copy(raw));
    return value == "TRUE" || value == "1" || value == "T" || value == "YES";
}
double parse_double(const std::string &raw, double fallback = 0) {
    try {
        return std::stod(trim_copy(raw));
    } catch (...) {
        return fallback;
    }
}
long long parse_stat(const std::string &raw) {
    return static_cast<long long>(std::llround(parse_double(raw, 0)));
}

// Streaming CSV reader: handles quoted fields, embedded commas/newlines and
// CRLF. Throws on unterminated quoted fields rather than guessing.
class CsvReader {
  public:
    explicit CsvReader(const std::filesystem::path &path) : stream_(path, std::ios::binary) {
        if (!stream_)
            throw std::invalid_argument("Cannot open CSV file: " + path.filename().string());
        std::vector<std::string> header;
        if (next(header)) {
            for (size_t i = 0; i < header.size(); ++i)
                columns_[upper_copy(trim_copy(header[i]))] = i;
            width_ = header.size();
        }
    }
    bool next(std::vector<std::string> &row) {
        row.clear();
        std::string field;
        bool in_quotes = false, any = false;
        char c;
        while (stream_.get(c)) {
            any = true;
            if (in_quotes) {
                if (c == '"') {
                    if (stream_.peek() == '"') {
                        stream_.get(c);
                        field += '"';
                    } else
                        in_quotes = false;
                } else
                    field += c;
            } else if (c == '"' && field.empty()) {
                in_quotes = true;
            } else if (c == ',') {
                row.push_back(std::move(field));
                field.clear();
            } else if (c == '\n' || c == '\r') {
                if (c == '\r' && stream_.peek() == '\n')
                    stream_.get(c);
                row.push_back(std::move(field));
                return true;
            } else
                field += c;
        }
        if (in_quotes)
            throw std::invalid_argument("Unterminated quoted field in CSV");
        if (any || !field.empty()) {
            row.push_back(std::move(field));
            return true;
        }
        return false;
    }
    // Case-insensitive alias resolution. Returns -1 when optional and absent.
    int column(const std::vector<std::string> &aliases, const std::string &label, bool required,
               const std::string &file_label) const {
        for (const auto &alias : aliases) {
            const auto found = columns_.find(upper_copy(alias));
            if (found != columns_.end())
                return static_cast<int>(found->second);
        }
        if (required)
            throw std::invalid_argument(file_label + ": CSV missing column for " + label);
        return -1;
    }
    static std::string at(const std::vector<std::string> &row, int index) {
        return index >= 0 && static_cast<size_t>(index) < row.size() ? row[static_cast<size_t>(index)] : "";
    }

  private:
    std::ifstream stream_;
    std::map<std::string, size_t> columns_;
    size_t width_ = 0;
};

struct HistPlayer {
    std::string id, name, team;
    bool starter = false;
    json log;
};
struct HistGame {
    std::string id, date, home, away, seasonType = "regular";
    std::vector<HistPlayer> players;
    bool rejected = false;
    std::string rejectReason;
    // Play-by-play reductions (streamed, never stored in full).
    int events = 0, maxPeriod = 4;
    bool monotone = true;
    double lastHome = 0, lastAway = 0;
    bool firstFgSet = false;
    std::string firstFgPlayer;
    // Finalized.
    double durationMinutes = 48;
    bool firstFgVerified = false;
};

int stat_at(const json &log, const std::string &field) {
    return static_cast<int>(number(log.value(field, json(0))));
}
} // namespace

json build_history_bundle(const std::filesystem::path &history_dir, const std::vector<int> &seasons,
                          const std::string &as_of_date) {
    const auto downloads = history_dir / "downloads";
    std::map<std::string, HistGame> games;
    std::vector<std::string> order;
    long long rejected = 0;
    std::vector<std::string> missing_examples;
    auto reject_game = [&](HistGame &game, const std::string &reason) {
        if (game.rejected)
            return;
        game.rejected = true;
        game.rejectReason = reason;
        ++rejected;
        if (missing_examples.size() < 5)
            missing_examples.push_back(game.id + ": " + reason);
    };
    for (const int season : seasons) {
        const auto label = "player_box_" + std::to_string(season) + ".csv";
        const auto box_path = downloads / label;
        if (!std::filesystem::exists(box_path))
            throw std::invalid_argument("Missing downloaded box-score file: " + label);
        {
            CsvReader csv(box_path);
            const int c_game = csv.column({"game_id"}, "game id", true, label),
                      c_athlete = csv.column({"athlete_id"}, "athlete id", true, label),
                      c_name = csv.column({"athlete_display_name", "display_name", "name"}, "athlete name", true,
                                          label),
                      c_team = csv.column({"team_abbreviation", "team_abbr"}, "team", true, label),
                      c_starter = csv.column({"starter"}, "starter flag", true, label),
                      c_dnp = csv.column({"did_not_play"}, "did-not-play flag", false, label),
                      c_minutes = csv.column({"minutes", "min"}, "minutes", true, label),
                      c_date = csv.column({"game_date", "date"}, "game date", false, label),
                      c_home = csv.column({"home_team_abbreviation", "home_team_abbr"}, "home team", false, label),
                      c_away = csv.column({"away_team_abbreviation", "away_team_abbr"}, "away team", false, label),
                      c_type = csv.column({"season_type"}, "season type", false, label);
            std::map<std::string, int> stat_cols;
            for (const auto &field : kStatFields) {
                const int col = csv.column(kStatAliases.at(field), field, false, label);
                if (col >= 0)
                    stat_cols[field] = col;
            }
            for (const auto &required : {"points", "fgm", "fga", "fta", "oreb", "turnovers"})
                if (!stat_cols.count(required))
                    throw std::invalid_argument(label + ": CSV missing required stat column " + required);
            std::vector<std::string> row;
            while (csv.next(row)) {
                const auto game_id = trim_copy(CsvReader::at(row, c_game));
                if (game_id.empty())
                    continue;
                auto &game = games[game_id];
                if (game.id.empty()) {
                    game.id = game_id;
                    order.push_back(game_id);
                }
                if (c_date >= 0 && game.date.empty())
                    game.date = trim_copy(CsvReader::at(row, c_date)).substr(0, 10);
                if (c_home >= 0 && game.home.empty())
                    game.home = hoop_team(CsvReader::at(row, c_home));
                if (c_away >= 0 && game.away.empty())
                    game.away = hoop_team(CsvReader::at(row, c_away));
                if (c_type >= 0) {
                    const auto type = trim_copy(CsvReader::at(row, c_type));
                    game.seasonType = (type == "3" || type == "playoffs") ? "playoffs" : "regular";
                }
                const bool starter = truthy(CsvReader::at(row, c_starter));
                const double minutes = parse_double(CsvReader::at(row, c_minutes), 0);
                if ((c_dnp >= 0 && truthy(CsvReader::at(row, c_dnp))) || (minutes <= 0 && !starter))
                    continue;
                if (starter && minutes <= 0) {
                    reject_game(game, "starter with no minutes; refusing to guess the lineup");
                    continue;
                }
                json log = {{"gameId", game_id}, {"minutes", minutes}};
                for (const auto &field : kStatFields)
                    log[field] = stat_cols.count(field) ? parse_stat(CsvReader::at(row, stat_cols[field])) : 0;
                game.players.push_back({"espn-" + trim_copy(CsvReader::at(row, c_athlete)),
                                        trim_copy(CsvReader::at(row, c_name)),
                                        hoop_team(CsvReader::at(row, c_team)), starter, log});
            }
        }
        const auto pbp_label = "play_by_play_" + std::to_string(season) + ".csv";
        const auto pbp_path = downloads / pbp_label;
        if (std::filesystem::exists(pbp_path)) {
            CsvReader csv(pbp_path);
            const int c_game = csv.column({"game_id"}, "game id", true, pbp_label),
                      c_period = csv.column({"period_number", "period"}, "period", true, pbp_label),
                      c_scoring = csv.column({"scoring_play"}, "scoring flag", false, pbp_label),
                      c_value = csv.column({"score_value"}, "score value", false, pbp_label),
                      c_away_score = csv.column({"away_score"}, "away score", false, pbp_label),
                      c_home_score = csv.column({"home_score"}, "home score", false, pbp_label),
                      c_athlete = csv.column({"athlete_id_1"}, "primary athlete", false, pbp_label),
                      c_home = csv.column({"home_team_abbreviation", "home_team_abbr"}, "home team", false, pbp_label),
                      c_away = csv.column({"away_team_abbreviation", "away_team_abbr"}, "away team", false, pbp_label);
            csv.column({"text", "play_text", "description"}, "play text", true, pbp_label);
            std::vector<std::string> row;
            while (csv.next(row)) {
                const auto found = games.find(trim_copy(CsvReader::at(row, c_game)));
                if (found == games.end() || found->second.rejected)
                    continue;
                auto &game = found->second;
                if (c_home >= 0 && game.home.empty())
                    game.home = hoop_team(CsvReader::at(row, c_home));
                if (c_away >= 0 && game.away.empty())
                    game.away = hoop_team(CsvReader::at(row, c_away));
                const auto period_raw = trim_copy(CsvReader::at(row, c_period));
                if (period_raw.empty() || parse_double(period_raw, -1) < 0)
                    continue;
                const int period = static_cast<int>(parse_double(period_raw, 1));
                ++game.events;
                game.maxPeriod = std::max(game.maxPeriod, period);
                const double away_score = c_away_score >= 0 ? parse_double(CsvReader::at(row, c_away_score), 0) : 0,
                             home_score = c_home_score >= 0 ? parse_double(CsvReader::at(row, c_home_score), 0) : 0;
                if (away_score < game.lastAway || home_score < game.lastHome)
                    game.monotone = false;
                game.lastAway = away_score;
                game.lastHome = home_score;
                const auto athlete = trim_copy(CsvReader::at(row, c_athlete));
                if (!game.firstFgSet && c_scoring >= 0 && truthy(CsvReader::at(row, c_scoring)) &&
                    parse_double(CsvReader::at(row, c_value), 0) >= 2 && !athlete.empty()) {
                    game.firstFgSet = true;
                    game.firstFgPlayer = "espn-" + athlete;
                }
            }
        }
    }
    // Finalize games (same acceptance rules as the Python hoopR ingest).
    std::vector<HistGame *> completed;
    long long total_events = 0;
    for (const auto &id : order) {
        auto &game = games[id];
        if (game.rejected)
            continue;
        std::set<std::string> teams;
        for (const auto &player : game.players)
            teams.insert(player.team);
        if (teams.size() != 2 || game.date.empty() || game.home.empty() || game.away.empty() ||
            !teams.count(game.home) || !teams.count(game.away) || game.home == game.away) {
            reject_game(game, "missing date or home/away teams");
            continue;
        }
        bool bad_lineup = false;
        for (const auto &team : {game.home, game.away}) {
            int starters = 0;
            for (const auto &player : game.players)
                if (player.team == team && player.starter)
                    ++starters;
            if (starters != 5)
                bad_lineup = true;
        }
        if (bad_lineup) {
            reject_game(game, "historical starting lineup is not exactly five per team");
            continue;
        }
        for (auto &player : game.players) {
            player.log["date"] = game.date;
            player.log["home"] = player.team == game.home;
        }
        game.durationMinutes = 48 + std::max(0, game.maxPeriod - 4) * 5;
        std::set<std::string> scorer_ids;
        for (const auto &player : game.players)
            scorer_ids.insert(player.id);
        game.firstFgVerified =
            game.firstFgSet && scorer_ids.count(game.firstFgPlayer) && game.monotone && game.events > 0;
        total_events += game.events;
        completed.push_back(&game);
    }
    if (completed.empty())
        throw std::invalid_argument("No complete games could be built from the downloaded CSVs");
    std::sort(completed.begin(), completed.end(), [](const HistGame *a, const HistGame *b) {
        return a->date != b->date ? a->date < b->date : a->id < b->id;
    });
    const std::string recent = iso_at(parse_time(as_of_date + "T00:00:00Z") - 210LL * 86400).substr(0, 10);
    struct ActivePlayer {
        std::string name, team, latest;
    };
    std::map<std::string, ActivePlayer> active;
    for (const auto *game : completed)
        if (game->date >= recent && game->date < as_of_date)
            for (const auto &player : game->players)
                active[player.id] = {player.name, player.team, game->date};
    if (active.size() > 1200)
        throw std::invalid_argument("More than 1200 recent players; the raw downloads are unchanged");
    std::map<std::string, std::string> crosswalk;
    std::set<std::string> ambiguous;
    for (const auto &entry : active) {
        const auto key = normalize_name(entry.second.name);
        if (key.empty())
            continue;
        if (crosswalk.count(key) && crosswalk[key] != entry.first)
            ambiguous.insert(crosswalk[key]), ambiguous.insert(entry.first);
        else
            crosswalk[key] = entry.first;
    }
    std::map<std::string, std::deque<json>> logs, first_baskets;
    std::map<std::string, std::deque<double>> paces;
    std::map<std::string, std::deque<json>> team_logs;
    auto push_capped = [](std::deque<json> &deque_, json value, size_t cap) {
        deque_.push_back(std::move(value));
        while (deque_.size() > cap)
            deque_.pop_front();
    };
    for (const auto *game : completed) {
        if (game->date >= as_of_date)
            continue;
        std::map<std::string, long long> points;
        std::map<std::string, double> totals;
        for (const auto &team : {game->home, game->away}) {
            long long fga = 0, oreb = 0, tov = 0, pts = 0;
            double fta = 0;
            for (const auto &player : game->players)
                if (player.team == team) {
                    fga += stat_at(player.log, "fga");
                    oreb += stat_at(player.log, "oreb");
                    tov += stat_at(player.log, "turnovers");
                    fta += stat_at(player.log, "fta");
                    pts += stat_at(player.log, "points");
                }
            points[team] = pts;
            totals[team] = fga - oreb + tov + 0.44 * fta;
        }
        const double possessions = (totals[game->home] + totals[game->away]) / 2;
        const double pace = possessions * 48 / game->durationMinutes;
        for (const auto &player : game->players) {
            if (!active.count(player.id) || ambiguous.count(player.id))
                continue;
            if (game->seasonType == "regular" && number(player.log.value("minutes", json(0))) >= 0.1) {
                push_capped(logs[player.id], player.log, 150);
                auto &pace_deque = paces[player.id];
                pace_deque.push_back(pace);
                while (pace_deque.size() > 150)
                    pace_deque.pop_front();
            }
            if (player.starter && game->firstFgVerified)
                push_capped(first_baskets[player.id],
                            {{"gameId", game->id}, {"date", game->date}, {"win", game->firstFgPlayer == player.id}},
                            2000);
        }
        if (game->seasonType == "regular")
            for (const auto &team : {game->home, game->away}) {
                const auto &opponent = team == game->home ? game->away : game->home;
                push_capped(team_logs[team],
                            {{"date", game->date},
                             {"gameId", game->id},
                             {"points", points[team]},
                             {"margin", points[team] - points[opponent]},
                             {"home", team == game->home},
                             {"opponentPoints", points[opponent]},
                             {"possessions", possessions},
                             {"durationMinutes", game->durationMinutes}},
                            150);
            }
    }
    std::vector<json> records;
    int team_count = 0;
    for (const auto &entry : team_logs) {
        const auto &sample = entry.second;
        if (sample.empty() || sample.back().value("date", "") < recent)
            continue;
        double possessions = 0, pace_sum = 0, scored = 0, allowed = 0;
        for (const auto &game : sample) {
            possessions += number(game.value("possessions", json(0)));
            pace_sum += number(game.value("possessions", json(0))) * 48 /
                        number(game.value("durationMinutes", json(48)));
            scored += number(game.value("points", json(0)));
            allowed += number(game.value("opponentPoints", json(0)));
        }
        if (possessions <= 0)
            continue;
        const double pace = pace_sum / static_cast<double>(sample.size()),
                     offense = scored * 100 / possessions, defense = allowed * 100 / possessions;
        if (!(pace >= 70 && pace <= 130 && offense >= 70 && offense <= 150 && defense >= 70 && defense <= 150))
            continue;
        json reversed = json::array();
        for (auto it = sample.rbegin(); it != sample.rend(); ++it)
            reversed.push_back(*it);
        records.push_back({{"recordType", "team"},
                           {"feature", {{"abbr", entry.first},
                                        {"pace", pace},
                                        {"offRating", offense},
                                        {"defRating", defense},
                                        {"logs", reversed},
                                        {"dataAsOf", sample.back().value("date", "") + "T23:59:59Z"}}}});
        ++team_count;
    }
    int player_count = 0;
    for (const auto &entry : active) {
        if (ambiguous.count(entry.first))
            continue;
        json sample = json::array(), observations = json::array();
        for (auto it = logs[entry.first].rbegin(); it != logs[entry.first].rend(); ++it)
            sample.push_back(*it);
        for (auto it = first_baskets[entry.first].rbegin(); it != first_baskets[entry.first].rend(); ++it)
            observations.push_back(*it);
        if (sample.empty() && observations.empty())
            continue;
        json feature = {{"name", entry.second.name},
                        {"team", entry.second.team},
                        {"basketballReferenceId", entry.first},
                        {"logs", sample},
                        {"firstBasketHistory", observations},
                        {"starterConfirmed", false},
                        {"injury", "Unknown"},
                        {"dataAsOf", entry.second.latest + "T23:59:59Z"}};
        if (!paces[entry.first].empty()) {
            double pace = 0;
            for (const double value : paces[entry.first])
                pace += value;
            pace /= static_cast<double>(paces[entry.first].size());
            if (pace >= 70 && pace <= 130)
                feature["historicalPace"] = pace;
        }
        records.push_back({{"recordType", "player"}, {"feature", feature}});
        ++player_count;
    }
    std::string first_date, last_date;
    for (const auto *game : completed)
        if (game->date < as_of_date) {
            if (first_date.empty())
                first_date = game->date;
            last_date = game->date;
        }
    json metadata = {{"games", completed.size()},
                     {"events", total_events},
                     {"missingGames", rejected},
                     {"missingExamples", missing_examples},
                     {"firstDate", first_date},
                     {"lastDate", last_date},
                     {"asOf", as_of_date},
                     {"exportedAt", now_iso()},
                     {"recentDays", 210},
                     {"players", player_count},
                     {"teams", team_count},
                     {"seasons", seasons}};
    std::ostringstream output;
    const json manifest = {{"recordType", "manifest"}, {"schemaVersion", 1}, {"source", "sportsdataverse"},
                           {"metadata", metadata}};
    auto write_record = [&output](const json &record) {
        const auto line = record.dump();
        if (line.size() > 1024 * 1024)
            throw std::invalid_argument("Historical feature record exceeds 1 MB");
        output << line << "\n";
    };
    write_record(manifest);
    for (const auto &record : records)
        write_record(record);
    std::error_code ec;
    std::filesystem::create_directories(history_dir);
    const auto target = history_dir / "features.jsonl", temp = history_dir / "features.jsonl.tmp";
    {
        std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
        if (!stream)
            throw std::invalid_argument("Cannot write history bundle in " + history_dir.string());
        stream << output.str();
        if (!stream)
            throw std::invalid_argument("Cannot write history bundle in " + history_dir.string());
    }
    std::filesystem::remove(target, ec);
    std::filesystem::rename(temp, target, ec);
    if (ec)
        throw std::invalid_argument("Cannot install history bundle: " + ec.message());
    return metadata;
}

namespace {
bool download_file(const std::string &url, const std::filesystem::path &destination,
                   const std::function<void(long long, long long)> &progress, std::string &error) {
    if (url.rfind("https://github.com/sportsdataverse/sportsdataverse-data/releases/download/", 0) != 0) {
        error = "Download URL is outside the fixed SportsDataverse release prefix";
        return false;
    }
    auto temp = destination;
    temp += ".part";
    std::error_code ec;
    std::filesystem::remove(temp, ec);
#ifdef _WIN32
    auto wide = [](const std::string &s) {
        int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring w(n, 0);
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
        return w;
    };
    std::string current = url;
    for (int redirect = 0; redirect <= 3; ++redirect) {
        auto input = wide(current);
        URL_COMPONENTS parts{};
        parts.dwStructSize = sizeof(parts);
        parts.dwHostNameLength = static_cast<DWORD>(-1);
        parts.dwUrlPathLength = static_cast<DWORD>(-1);
        parts.dwExtraInfoLength = static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(input.c_str(), 0, 0, &parts)) {
            error = "Cannot parse download URL";
            return false;
        }
        std::wstring host(parts.lpszHostName, parts.dwHostNameLength),
            path(parts.lpszUrlPath, parts.dwUrlPathLength);
        if (parts.dwExtraInfoLength)
            path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
        HINTERNET session = WinHttpOpen(L"Edgecourt/0.1 (personal NBA research)",
                                        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                        WINHTTP_NO_PROXY_BYPASS, 0);
        HINTERNET connection = nullptr, request = nullptr;
        if (session) {
            WinHttpSetTimeouts(session, 10000, 10000, 30000, 30000);
            connection = WinHttpConnect(session, host.c_str(), parts.nPort, 0);
        }
        if (connection)
            request = WinHttpOpenRequest(connection, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (request) {
            DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
            WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
            DWORD decompress = WINHTTP_DECOMPRESSION_FLAG_GZIP | WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
            WinHttpSetOption(request, WINHTTP_OPTION_DECOMPRESSION, &decompress, sizeof(decompress));
        }
        bool ok = false, redirected = false;
        if (request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0,
                                          0, 0) &&
            WinHttpReceiveResponse(request, nullptr)) {
            DWORD code = 0, size = sizeof(code);
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX);
            if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
                wchar_t location[2048];
                DWORD location_size = sizeof(location);
                if (WinHttpQueryHeaders(request, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, location,
                                        &location_size, WINHTTP_NO_HEADER_INDEX)) {
                    int n = WideCharToMultiByte(CP_UTF8, 0, location, -1, nullptr, 0, nullptr, nullptr);
                    std::string next(n > 0 ? n - 1 : 0, 0);
                    if (n > 1)
                        WideCharToMultiByte(CP_UTF8, 0, location, -1, next.data(), n, nullptr, nullptr);
                    // GitHub release assets redirect to a signed githubusercontent URL.
                    if (next.rfind("https://", 0) == 0 &&
                        (next.find("githubusercontent.com") != std::string::npos ||
                         next.rfind("https://github.com/", 0) == 0)) {
                        current = next;
                        redirected = true;
                    }
                }
            } else if (code == 200) {
                DWORD length = 0, length_size = sizeof(length);
                const long long total =
                    WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                        WINHTTP_HEADER_NAME_BY_INDEX, &length, &length_size,
                                        WINHTTP_NO_HEADER_INDEX)
                        ? static_cast<long long>(length)
                        : -1;
                std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
                long long downloaded = 0;
                DWORD available = 0;
                bool failed = !stream.is_open();
                while (!failed && WinHttpQueryDataAvailable(request, &available) && available) {
                    std::string chunk(available, 0);
                    DWORD read = 0;
                    if (!WinHttpReadData(request, chunk.data(), available, &read))
                        break;
                    if (read == 0)
                        break;
                    stream.write(chunk.data(), read);
                    downloaded += read;
                    progress(downloaded, total);
                }
                stream.flush();
                ok = stream.good() && downloaded > 0;
                if (!ok)
                    error = "Download interrupted while writing " + destination.filename().string();
            } else
                error = "Download returned HTTP " + std::to_string(code);
        } else
            error = "HTTPS download unavailable (WinHTTP error " + std::to_string(GetLastError()) + ")";
        if (request)
            WinHttpCloseHandle(request);
        if (connection)
            WinHttpCloseHandle(connection);
        if (session)
            WinHttpCloseHandle(session);
        if (redirected)
            continue;
        if (!ok) {
            std::filesystem::remove(temp, ec);
            return false;
        }
        std::filesystem::remove(destination, ec);
        std::filesystem::rename(temp, destination, ec);
        if (ec) {
            error = "Cannot install downloaded file: " + ec.message();
            return false;
        }
        return true;
    }
    error = "Too many redirects while downloading " + destination.filename().string();
    return false;
#else
    int pipes[2];
    if (pipe(pipes) != 0) {
        error = "Cannot start HTTPS transport";
        return false;
    }
    const pid_t pid = fork();
    if (pid == 0) {
        dup2(pipes[1], STDOUT_FILENO);
        int nullfd = open("/dev/null", O_WRONLY);
        if (nullfd >= 0)
            dup2(nullfd, STDERR_FILENO);
        close(pipes[0]);
        close(pipes[1]);
        execlp("curl", "curl", "-sS", "--fail", "-L", "--max-redirs", "3", "--proto", "=https", "--proto-redir",
               "=https", "--connect-timeout", "10", "--max-time", "1800", "-o", temp.c_str(), url.c_str(),
               static_cast<char *>(nullptr));
        _exit(127);
    }
    close(pipes[1]);
    if (pid < 0) {
        close(pipes[0]);
        error = "Cannot fork HTTPS transport";
        return false;
    }
    char drain[4096];
    while (read(pipes[0], drain, sizeof drain) > 0) {
    }
    close(pipes[0]);
    int status = 0;
    bool done = false;
    while (!done) {
        const pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid || waited < 0)
            done = true;
        else {
            std::error_code size_ec;
            const auto size = std::filesystem::file_size(temp, size_ec);
            if (!size_ec)
                progress(static_cast<long long>(size), -1);
            usleep(250000);
        }
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        std::filesystem::remove(temp, ec);
        error = "Download failed (curl exit " + std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1) +
                "); the season file may not be published yet";
        return false;
    }
    const auto size = std::filesystem::file_size(temp, ec);
    if (ec || size == 0) {
        std::filesystem::remove(temp, ec);
        error = "Downloaded file was empty";
        return false;
    }
    progress(static_cast<long long>(size), static_cast<long long>(size));
    std::filesystem::remove(destination, ec);
    std::filesystem::rename(temp, destination, ec);
    if (ec) {
        error = "Cannot install downloaded file: " + ec.message();
        return false;
    }
    return true;
#endif
}
json read_json_file(const std::filesystem::path &path) {
    try {
        std::ifstream stream(path);
        if (!stream)
            return json::object();
        json value;
        stream >> value;
        return value.is_object() ? value : json::object();
    } catch (...) {
        return json::object();
    }
}
void write_json_file(const std::filesystem::path &path, const json &value) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (stream)
        stream << value.dump(2);
}
} // namespace

std::vector<int> HistoryDownloader::target_seasons(long long epoch_seconds) {
    const auto date = iso_at(epoch_seconds);
    const int year = std::stoi(date.substr(0, 4)), month = std::stoi(date.substr(5, 2));
    const int current = month >= 10 ? year : year - 1;
    return {current - 4, current - 3, current - 2, current - 1, current};
}
HistoryDownloader::HistoryDownloader(std::filesystem::path history_dir)
    : history_dir_(std::move(history_dir)) {
    status_ = {{"state", "idle"},
               {"message", "Automatic SportsDataverse history download has not run yet."}};
}
HistoryDownloader::~HistoryDownloader() {
    if (thread_.joinable())
        thread_.join();
}
void HistoryDownloader::set_on_finished(std::function<void()> callback) {
    std::lock_guard<std::mutex> guard(mutex_);
    on_finished_ = std::move(callback);
}
json HistoryDownloader::status() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return status_;
}
void HistoryDownloader::set_status(json status) {
    std::lock_guard<std::mutex> guard(mutex_);
    status_ = std::move(status);
}
bool HistoryDownloader::start(bool force) {
    std::thread previous;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (running_)
            return false;
        if (!force && last_failed_ && now_epoch_seconds() - last_attempt_ < 600)
            return false;
        previous = std::move(thread_);
    }
    if (previous.joinable())
        previous.join();
    {
        std::lock_guard<std::mutex> guard(mutex_);
        if (running_)
            return false;
        running_ = true;
        last_attempt_ = now_epoch_seconds();
        thread_ = std::thread(&HistoryDownloader::run, this);
    }
    return true;
}
void HistoryDownloader::run() {
    bool success = false;
    try {
        const auto downloads = history_dir_ / "downloads";
        std::filesystem::create_directories(downloads);
        const auto seasons = target_seasons(now_epoch_seconds());
        const int current = seasons.back();
        auto manifest = read_json_file(downloads / "download_manifest.json");
        if (!manifest.contains("seasons") || !manifest["seasons"].is_object())
            manifest["seasons"] = json::object();
        std::vector<std::string> notes;
        int done = 0;
        for (const int season : seasons) {
            const std::string key = std::to_string(season);
            if (!manifest["seasons"].contains(key) || !manifest["seasons"][key].is_object())
                manifest["seasons"][key] = json::object();
            for (const std::string kind : {"box", "pbp"}) {
                const std::string file_name = kind == "box"
                                                  ? "player_box_" + key + ".csv"
                                                  : "play_by_play_" + key + ".csv";
                const std::string url =
                    "https://github.com/sportsdataverse/sportsdataverse-data/releases/download/" +
                    std::string(kind == "box" ? "espn_nba_player_boxscores/" : "espn_nba_pbp/") + file_name;
                const auto destination = downloads / file_name;
                auto &entry = manifest["seasons"][key][kind];
                if (!entry.is_object())
                    entry = json::object();
                std::error_code ec;
                const bool have = std::filesystem::exists(destination) && std::filesystem::file_size(destination, ec) > 0;
                const long long fetched_epoch = parse_time(entry.value("at", ""));
                const bool fresh = have && (season < current || now_epoch_seconds() - fetched_epoch < 86400);
                const bool missing_recorded =
                    entry.value("missing", false) && now_epoch_seconds() - fetched_epoch < 86400;
                if (fresh || (kind == "pbp" && missing_recorded && season == current))
                    continue;
                set_status({{"state", "downloading"},
                            {"message", "Downloading SportsDataverse history (" + file_name + ")…"},
                            {"season", season},
                            {"file", file_name},
                            {"bytesDownloaded", 0},
                            {"bytesTotal", -1},
                            {"seasonsDone", done},
                            {"seasonsTotal", seasons.size()},
                            {"updatedAt", now_iso()}});
                std::string error;
                long long last_reported = -1;
                const bool ok = download_file(
                    url, destination,
                    [&](long long downloaded, long long total) {
                        if (downloaded - last_reported < 4 * 1024 * 1024 && downloaded != total)
                            return;
                        last_reported = downloaded;
                        set_status({{"state", "downloading"},
                                    {"message", "Downloading SportsDataverse history (" + file_name + ")…"},
                                    {"season", season},
                                    {"file", file_name},
                                    {"bytesDownloaded", downloaded},
                                    {"bytesTotal", total},
                                    {"seasonsDone", done},
                                    {"seasonsTotal", seasons.size()},
                                    {"updatedAt", now_iso()}});
                    },
                    error);
                if (ok) {
                    entry = {{"bytes", static_cast<long long>(std::filesystem::file_size(destination, ec))},
                             {"at", now_iso()}};
                } else if (kind == "pbp") {
                    entry = {{"missing", true}, {"at", now_iso()}};
                    notes.push_back(file_name + ": " + error);
                } else {
                    notes.push_back(file_name + ": " + error);
                }
                write_json_file(downloads / "download_manifest.json", manifest);
            }
            ++done;
        }
        std::vector<int> available;
        std::string fingerprint;
        for (const int season : seasons) {
            const auto box = downloads / ("player_box_" + std::to_string(season) + ".csv");
            std::error_code ec;
            if (!std::filesystem::exists(box) || std::filesystem::file_size(box, ec) == 0)
                continue;
            available.push_back(season);
            const auto pbp = downloads / ("play_by_play_" + std::to_string(season) + ".csv");
            fingerprint += std::to_string(season) + ":" +
                           std::to_string(static_cast<long long>(std::filesystem::file_size(box, ec))) + ":" +
                           (std::filesystem::exists(pbp)
                                ? std::to_string(static_cast<long long>(std::filesystem::file_size(pbp, ec)))
                                : std::string("0")) +
                           ";";
        }
        if (available.empty())
            throw std::invalid_argument("No SportsDataverse box-score files could be downloaded" +
                                        (notes.empty() ? std::string() : ": " + notes.front()));
        const bool need_build = !std::filesystem::exists(history_dir_ / "features.jsonl") ||
                                manifest.value("bundleBuiltFor", "") != fingerprint;
        if (need_build) {
            set_status({{"state", "building"},
                        {"message", "Building the local history bundle from downloaded seasons…"},
                        {"seasonsDone", static_cast<int>(available.size())},
                        {"seasonsTotal", seasons.size()},
                        {"updatedAt", now_iso()}});
            const auto metadata = build_history_bundle(history_dir_, available, now_iso().substr(0, 10));
            manifest["bundleBuiltFor"] = fingerprint;
            manifest["games"] = metadata.value("games", json(0));
            manifest["updatedAt"] = now_iso();
            write_json_file(downloads / "download_manifest.json", manifest);
            set_status({{"state", "ready"},
                        {"message", "Local history ready: " + std::to_string(static_cast<int>(number(metadata.value("games", json(0))))) +
                                        " games from " + std::to_string(available.size()) +
                                        " downloaded SportsDataverse seasons."},
                        {"games", metadata.value("games", json(0))},
                        {"seasons", available},
                        {"updatedAt", now_iso()}});
        } else {
            set_status({{"state", "ready"},
                        {"message", "Local history is already current for the downloaded seasons."},
                        {"games", manifest.value("games", json(0))},
                        {"seasons", available},
                        {"updatedAt", now_iso()}});
        }
        success = true;
    } catch (const std::exception &error) {
        set_status({{"state", "failed"}, {"message", error.what()}, {"updatedAt", now_iso()}});
    }
    std::function<void()> callback;
    {
        std::lock_guard<std::mutex> guard(mutex_);
        last_failed_ = !success;
        running_ = false;
        callback = on_finished_;
    }
    if (callback)
        callback();
}
} // namespace edgecourt
