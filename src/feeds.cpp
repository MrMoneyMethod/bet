#include "feeds.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#else
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace edgecourt {
namespace {
constexpr size_t MAX_RESPONSE = 16 * 1024 * 1024;
long long now_seconds() {
    return parse_time(now_iso());
}
std::string canonical_team(std::string value) {
    const std::map<std::string, std::string> aliases = {{"GS", "GSW"},  {"NY", "NYK"},   {"NO", "NOP"},
                                                        {"SA", "SAS"},  {"UTAH", "UTA"}, {"WSH", "WAS"},
                                                        {"BRK", "BKN"}, {"PHO", "PHX"},  {"CHO", "CHA"}};
    auto found = aliases.find(value);
    return found == aliases.end() ? value : found->second;
}
std::string clean_timestamp(std::string value) {
    // ESPN frequently returns minute precision, NBA sometimes millisecond precision.
    if (value.size() == 17 && value.back() == 'Z')
        value.insert(16, ":00");
    if (value.size() > 20 && value[19] == '.')
        value = value.substr(0, 19) + "Z";
    return value;
}
std::string clean_date(std::string value) {
    if (value.size() >= 10 && value[4] == '-')
        return value.substr(0, 10);
    std::tm tm{};
    std::istringstream stream(value);
    stream >> std::get_time(&tm, "%b %d, %Y");
    if (stream.fail())
        return "";
    char out[16];
    std::strftime(out, sizeof out, "%Y-%m-%d", &tm);
    return out;
}
double clock_minutes(std::string clock) {
    std::smatch match;
    if (std::regex_search(clock, match, std::regex("PT([0-9]+)M([0-9.]+)S")))
        return std::stod(match[1]) + std::stod(match[2]) / 60;
    if (std::regex_match(clock, match, std::regex("([0-9]+):([0-9.]+)")))
        return std::stod(match[1]) + std::stod(match[2]) / 60;
    return std::numeric_limits<double>::quiet_NaN();
}
double elapsed_minutes(int period, const std::string &clock) {
    if (period <= 0)
        return 0;
    double duration = period <= 4 ? 12 : 5;
    double before = period <= 4 ? (period - 1) * 12 : 48 + (period - 5) * 5;
    const double left = clock_minutes(clock);
    if (!std::isfinite(left))
        return std::numeric_limits<double>::quiet_NaN();
    return before + std::max(0.0, duration - left);
}
json &ensure_team(json &snapshot, const std::string &abbr) {
    if (!snapshot["teams"].contains(abbr))
        snapshot["teams"][abbr] = json::object();
    return snapshot["teams"][abbr];
}
void upsert_game(json &snapshot, const json &game) {
    for (auto &old : snapshot["games"])
        if (old.value("id", "") == game.value("id", "")) {
            old.update(game);
            return;
        }
    snapshot["games"].push_back(game);
}
void add_odds(const json &odds, json &snapshot, const json &game, const std::string &fetched) {
    if (!odds.is_array())
        return;
    for (const auto &entry : odds) {
        std::string book = "ESPN public odds";
        if (entry.contains("provider") && entry["provider"].is_object())
            book = entry["provider"].value("name", book);
        for (const std::string side : {"home", "away"}) {
            json raw = nullptr;
            const auto old_key = side + "TeamOdds";
            if (entry.contains(old_key) && entry[old_key].is_object() && entry[old_key].contains("moneyLine"))
                raw = entry[old_key]["moneyLine"];
            if (entry.contains("moneyline") && entry["moneyline"].is_object() &&
                entry["moneyline"].contains(side)) {
                const auto &current = entry["moneyline"][side];
                if (current.contains("close") && current["close"].is_object() &&
                    current["close"].contains("odds"))
                    raw = current["close"]["odds"];
                else if (current.contains("odds"))
                    raw = current["odds"];
            }
            const double value = number(raw, 0);
            if ((value > -100 && value < 100) || value > 100000 || value < -100000 ||
                std::floor(value) != value)
                continue;
            const auto team = game.value(side, "");
            json q = {{"gameId", game["id"]},
                      {"market", "moneyline"},
                      {"player", ""},
                      {"team", team},
                      {"side", side},
                      {"line", nullptr},
                      {"americanOdds", static_cast<int>(value)},
                      {"book", book},
                      {"observedAt", fetched},
                      {"timestampKind", "retrieved"},
                      {"source", "espn"},
                      {"status", "open"},
                      {"settlement", "full_game_including_ot"},
                      {"demo", false},
                      {"sourceUrl", "https://www.espn.com/nba/game/_/gameId/" + game.value("id", "")}};
            q["id"] = quote_id(q);
            bool found = false;
            for (auto &old : snapshot["quotes"])
                if (old.value("id", "") == q.value("id", "")) {
                    old = q;
                    found = true;
                    break;
                }
            if (!found)
                snapshot["quotes"].push_back(q);
        }
    }
}
json source(const std::string &id, const std::string &name, const FeedResponse &response, int count,
            const std::string &detail) {
    return {{"id", id},
            {"label", name},
            {"state", response.data.is_null() ? "unavailable"
                      : response.stale        ? "stale"
                      : response.cached       ? "cached"
                                              : "ok"},
            {"updatedAt", response.fetched_at},
            {"count", count},
            {"message",
             response.data.is_null()
                 ? response.error
                 : detail + (response.stale ? " Cached data is older than the normal refresh TTL." : "")}};
}
std::string strip_tags(const std::string &html) {
    std::string value = std::regex_replace(html, std::regex("<[^>]*>"), "");
    for (const auto &pair : std::vector<std::pair<std::string, std::string>>{{"&amp;", "&"},
                                                                             {"&nbsp;", " "},
                                                                             {"&#39;", "'"},
                                                                             {"&quot;", "\""},
                                                                             {"&lt;", "<"},
                                                                             {"&gt;", ">"}}) {
        size_t p = 0;
        while ((p = value.find(pair.first, p)) != std::string::npos) {
            value.replace(p, pair.first.size(), pair.second);
            p += pair.second.size();
        }
    }
    auto first = value.find_first_not_of(" \n\r\t"), last = value.find_last_not_of(" \n\r\t");
    return first == std::string::npos ? "" : value.substr(first, last - first + 1);
}
json balanced_json(const std::string &html, size_t start) {
    bool quoted = false, escape = false;
    int depth = 0;
    for (size_t i = start; i < html.size(); ++i) {
        char c = html[i];
        if (quoted) {
            if (escape)
                escape = false;
            else if (c == '\\')
                escape = true;
            else if (c == '"')
                quoted = false;
        } else if (c == '"')
            quoted = true;
        else if (c == '{' || c == '[')
            ++depth;
        else if (c == '}' || c == ']') {
            if (--depth == 0)
                return json::parse(html.substr(start, i - start + 1));
        }
    }
    throw std::invalid_argument("Incomplete embedded JSON");
}
void walk_espn(const json &node, json &snapshot, int depth = 0) {
    if (depth > 24)
        return;
    if (node.is_object()) {
        if (node.contains("events") && node["events"].is_array()) {
            parse_espn_scoreboard(node, snapshot, now_iso());
            return;
        }
        if (node.contains("competitions") && node.contains("id")) {
            parse_espn_scoreboard({{"events", json::array({node})}}, snapshot, now_iso());
            return;
        }
        for (const auto &item : node.items())
            if (item.value().is_structured())
                walk_espn(item.value(), snapshot, depth + 1);
    } else if (node.is_array())
        for (const auto &value : node)
            if (value.is_structured())
                walk_espn(value, snapshot, depth + 1);
}
} // namespace

bool allowed_data_url(const std::string &url) {
    static const std::regex expression(
        "^https://(site\\.api\\.espn\\.com|cdn\\.nba\\.com|stats\\.nba\\.com|www\\.espn\\.com)/[^\\r\\n\\t "
        "]*$");
    return url.size() < 4096 && std::regex_match(url, expression);
}
HttpResponse public_get(const std::string &url) {
    HttpResponse response;
    if (!allowed_data_url(url)) {
        response.error = "Data URL is outside the fixed public-source allowlist";
        return response;
    }
#ifdef _WIN32
    auto wide = [](const std::string &s) {
        int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring w(n, 0);
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
        return w;
    };
    auto input = wide(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(input.c_str(), 0, 0, &parts)) {
        response.error = "Cannot parse source URL";
        return response;
    }
    std::wstring host(parts.lpszHostName, parts.dwHostNameLength),
        path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength)
        path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    HINTERNET session =
        WinHttpOpen(L"Edgecourt/0.1 (personal NBA research)", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connection = nullptr, request = nullptr;
    if (session) {
        WinHttpSetTimeouts(session, 4000, 4000, 8000, 8000);
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
    if (request &&
        WinHttpSendRequest(request,
                           L"Accept: application/json, text/html;q=0.9\r\nReferer: https://www.nba.com/\r\n",
                           static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        DWORD code = 0, size = sizeof(code);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX);
        response.status = static_cast<int>(code);
        DWORD available = 0;
        while (WinHttpQueryDataAvailable(request, &available) && available) {
            if (response.body.size() + available > MAX_RESPONSE) {
                response.error = "Source exceeded the 16 MB response limit";
                break;
            }
            std::string part(available, 0);
            DWORD read = 0;
            if (!WinHttpReadData(request, part.data(), available, &read)) {
                response.error = "Source read failed";
                break;
            }
            response.body.append(part.data(), read);
        }
    } else
        response.error = "HTTPS source unavailable (WinHTTP error " + std::to_string(GetLastError()) + ")";
    if (request)
        WinHttpCloseHandle(request);
    if (connection)
        WinHttpCloseHandle(connection);
    if (session)
        WinHttpCloseHandle(session);
#else
    int pipes[2];
    if (pipe(pipes) != 0) {
        response.error = "Cannot start HTTPS transport";
        return response;
    }
    pid_t pid = fork();
    if (pid == 0) {
        dup2(pipes[1], STDOUT_FILENO);
        int nullfd = open("/dev/null", O_WRONLY);
        if (nullfd >= 0)
            dup2(nullfd, STDERR_FILENO);
        close(pipes[0]);
        close(pipes[1]);
        execlp("curl", "curl", "--silent", "--show-error", "--connect-timeout", "4", "--max-time", "8",
               "--max-filesize", "16777216", "--proto", "=https", "--user-agent",
               "Edgecourt/0.1 personal NBA research", "-H", "Accept: application/json, text/html;q=0.9", "-H",
               "Referer: https://www.nba.com/", "--write-out", "\nEDGECOURT_STATUS:%{http_code}", url.c_str(),
               static_cast<char *>(nullptr));
        _exit(127);
    }
    close(pipes[1]);
    if (pid < 0) {
        close(pipes[0]);
        response.error = "Cannot fork HTTPS transport";
        return response;
    }
    char buffer[8192];
    ssize_t n = 0;
    bool oversized = false;
    while ((n = read(pipes[0], buffer, sizeof buffer)) > 0) {
        if (response.body.size() + static_cast<size_t>(n) > MAX_RESPONSE) {
            oversized = true;
            continue;
        }
        response.body.append(buffer, static_cast<size_t>(n));
    }
    close(pipes[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    auto footer = response.body.rfind("\nEDGECOURT_STATUS:");
    if (footer != std::string::npos) {
        try {
            response.status = std::stoi(response.body.substr(footer + 18));
        } catch (...) {
        }
        response.body.erase(footer);
    }
    if (oversized)
        response.error = "Source exceeded the 16 MB response limit";
    else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        response.error = "HTTPS source unavailable (curl exit " +
                         std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : -1) + ")";
#endif
    if (response.status != 200 && response.error.empty())
        response.error = "Public source returned HTTP " + std::to_string(response.status) +
                         "; no challenge, login or rate-limit bypass attempted";
    return response;
}

json nba_rows(const json &doc) {
    json out = json::array(), sets = doc.value("resultSets", json::array());
    if (sets.is_object())
        sets = json::array({sets});
    if (sets.empty() && doc.contains("resultSet"))
        sets = json::array({doc["resultSet"]});
    for (const auto &set : sets) {
        if (!set.contains("headers") || !set.contains("rowSet") || !set["headers"].is_array() ||
            !set["rowSet"].is_array())
            continue;
        for (const auto &raw : set["rowSet"]) {
            if (!raw.is_array() || raw.size() != set["headers"].size())
                continue;
            json row = json::object();
            for (size_t i = 0; i < raw.size(); ++i)
                row[text(set["headers"][i])] = raw[i];
            out.push_back(std::move(row));
        }
    }
    return out;
}
void parse_espn_scoreboard(const json &doc, json &s, const std::string &fetched) {
    if (!doc.contains("events") || !doc["events"].is_array())
        return;
    for (const auto &event : doc["events"]) {
        if (!event.contains("competitions") || !event["competitions"].is_array() ||
            event["competitions"].empty())
            continue;
        const auto &competition = event["competitions"][0];
        if (!competition.contains("competitors") || competition["competitors"].size() != 2)
            continue;
        json home, away;
        for (const auto &c : competition["competitors"]) {
            if (c.value("homeAway", "") == "home")
                home = c;
            else if (c.value("homeAway", "") == "away")
                away = c;
        }
        if (home.is_null() || away.is_null() || !home.contains("team") || !away.contains("team"))
            continue;
        auto h = canonical_team(home["team"].value("abbreviation", "")),
             a = canonical_team(away["team"].value("abbreviation", ""));
        auto state = event.value("status", competition.value("status", json::object()));
        auto type = state.value("type", json::object());
        auto raw_state = type.value("state", "pre");
        auto name = type.value("name", "");
        std::string status = raw_state == "in" ? "live" : raw_state == "post" ? "final" : "upcoming";
        if (name.find("POSTPON") != std::string::npos)
            status = "postponed";
        if (name.find("CANCEL") != std::string::npos)
            status = "cancelled";
        auto date = clean_timestamp(event.value("date", competition.value("date", "")));
        if (!parse_time(date) || h.empty() || a.empty())
            continue;
        auto id = text(event.value("id", json("")));
        if (id.empty())
            continue;
        int period = static_cast<int>(number(state.value("period", json(0))));
        auto clock = state.value("displayClock", "");
        json g = {{"id", id},
                  {"espnId", id},
                  {"homeEspnId", text(home["team"].value("id", json("")))},
                  {"awayEspnId", text(away["team"].value("id", json("")))},
                  {"home", h},
                  {"away", a},
                  {"homeName", home["team"].value("displayName", h)},
                  {"awayName", away["team"].value("displayName", a)},
                  {"status", status},
                  {"startsAt", date},
                  {"homeScore", number(home.value("score", json(0)))},
                  {"awayScore", number(away.value("score", json(0)))},
                  {"period", period},
                  {"clock", clock},
                  {"elapsedMinutes", elapsed_minutes(period, clock)},
                  {"source", "espn"},
                  {"dataAsOf", fetched},
                  {"seasonType", number(event.value("season", json::object()).value("type", json(2))) == 1
                                     ? "preseason"
                                     : "regular"}};
        g["liveScoresComplete"] =
            std::isfinite(number(home.value("score", json()), std::numeric_limits<double>::quiet_NaN())) &&
            std::isfinite(number(away.value("score", json()), std::numeric_limits<double>::quiet_NaN()));
        g["liveClockComplete"] = period >= 1 && std::isfinite(elapsed_minutes(period, clock));
        if (competition.contains("venue"))
            g["venue"] = competition["venue"].value("fullName", "");
        for (const auto *c : {&home, &away}) {
            auto abbr = canonical_team((*c)["team"].value("abbreviation", ""));
            auto &t = ensure_team(s, abbr);
            t["name"] = (*c)["team"].value("displayName", abbr);
            t["espnId"] = text((*c)["team"].value("id", json("")));
            if (c->contains("records") && (*c)["records"].is_array() && !(*c)["records"].empty())
                t["record"] = (*c)["records"][0].value("summary", "");
        }
        upsert_game(s, g);
        add_odds(competition.value("odds", json::array()), s, g, fetched);
    }
}
void parse_nba_scoreboard(const json &doc, json &s, const std::string &fetched) {
    if (!doc.contains("scoreboard") || !doc["scoreboard"].contains("games"))
        return;
    for (const auto &raw : doc["scoreboard"]["games"]) {
        if (!raw.contains("homeTeam") || !raw.contains("awayTeam"))
            continue;
        auto h = canonical_team(raw["homeTeam"].value("teamTricode", "")),
             a = canonical_team(raw["awayTeam"].value("teamTricode", ""));
        auto id = text(raw.value("gameId", json("")));
        int status = static_cast<int>(number(raw.value("gameStatus", json(1))));
        int period = static_cast<int>(number(raw.value("period", json(0))));
        auto clock = raw.value("gameClock", "");
        auto starts = clean_timestamp(raw.value("gameTimeUTC", ""));
        if (!parse_time(starts) || id.empty() || h.empty() || a.empty())
            continue;
        json *existing = nullptr;
        for (auto &g : s["games"])
            if (g.value("home", "") == h && g.value("away", "") == a &&
                g.value("startsAt", "").substr(0, 10) == starts.substr(0, 10)) {
                existing = &g;
                break;
            }
        json g = {
            {"id", existing ? existing->value("id", id) : "nba-" + id},
            {"nbaId", id},
            {"home", h},
            {"away", a},
            {"homeName", raw["homeTeam"].value("teamCity", "") + " " + raw["homeTeam"].value("teamName", h)},
            {"awayName", raw["awayTeam"].value("teamCity", "") + " " + raw["awayTeam"].value("teamName", a)},
            {"startsAt", starts},
            {"status", status == 2   ? "live"
                       : status == 3 ? "final"
                                     : "upcoming"},
            {"homeScore", number(raw["homeTeam"].value("score", json(0)))},
            {"awayScore", number(raw["awayTeam"].value("score", json(0)))},
            {"period", period},
            {"clock", clock},
            {"elapsedMinutes", elapsed_minutes(period, clock)},
            {"dataAsOf", fetched},
            {"source", "nba_cdn"},
            {"seasonType", id.rfind("001", 0) == 0 ? "preseason" : "regular"}};
        g["liveScoresComplete"] = std::isfinite(number(raw["homeTeam"].value("score", json()),
                                                       std::numeric_limits<double>::quiet_NaN())) &&
                                  std::isfinite(number(raw["awayTeam"].value("score", json()),
                                                       std::numeric_limits<double>::quiet_NaN()));
        g["liveClockComplete"] = period >= 1 && std::isfinite(elapsed_minutes(period, clock));
        const double left = clock_minutes(clock);
        if (std::isfinite(left)) {
            int minutes = static_cast<int>(left),
                seconds = static_cast<int>(std::round((left - minutes) * 60));
            std::ostringstream display;
            display << minutes << ":" << std::setw(2) << std::setfill('0') << seconds;
            g["clock"] = display.str();
        }
        upsert_game(s, g);
    }
}
void parse_espn_roster(const json &doc, json &s, const std::string &team, const std::string &fetched) {
    json athletes = doc.value("athletes", json::array());
    if (!athletes.is_array())
        return;
    for (const auto &raw : athletes) {
        if (!raw.contains("displayName"))
            continue;
        auto name = raw.value("displayName", "");
        auto key = normalize_name(name);
        auto p = s["players"].value(key, json::object());
        p["name"] = name;
        p["team"] = team;
        p["espnId"] = text(raw.value("id", json("")));
        p["injury"] = "Unknown";
        if (raw.contains("injuries") && raw["injuries"].is_array() && !raw["injuries"].empty()) {
            const auto &injury = raw["injuries"][0];
            p["injury"] = injury.value("status", "Unknown");
            if (injury.contains("details"))
                p["injuryDetail"] = injury["details"].value("detail", "");
        } else
            p["injury"] = "No injury listed";
        p["rosterAsOf"] = fetched;
        p["source"] = p.value("source", "espn");
        s["players"][key] = p;
    }
}
void parse_espn_summary(const json &doc, json &s, const json &game, const std::string &fetched) {
    add_odds(doc.value("pickcenter", doc.value("odds", json::array())), s, game, fetched);
    if (!doc.contains("boxscore") || !doc["boxscore"].contains("players"))
        return;
    const std::map<std::string, std::string> keys = {
        {"MIN", "minutes"}, {"PTS", "points"}, {"REB", "rebounds"}, {"AST", "assists"},
        {"STL", "steals"},  {"BLK", "blocks"}, {"3PT", "threes"},   {"3PM", "threes"}};
    for (const auto &group : doc["boxscore"]["players"]) {
        auto team = canonical_team(group.value("team", json::object()).value("abbreviation", ""));
        if (!group.contains("statistics"))
            continue;
        for (const auto &set : group["statistics"]) {
            if (!set.contains("labels") || !set.contains("athletes"))
                continue;
            for (const auto &athlete : set["athletes"]) {
                if (!athlete.contains("athlete") || !athlete.contains("stats") ||
                    athlete["stats"].size() != set["labels"].size())
                    continue;
                auto name = athlete["athlete"].value("displayName", "");
                auto key = normalize_name(name);
                json live = {{"gameId", game["id"]}, {"dataAsOf", fetched}};
                for (size_t i = 0; i < set["labels"].size(); ++i) {
                    auto label = text(set["labels"][i]);
                    if (!keys.count(label))
                        continue;
                    auto raw = text(athlete["stats"][i]);
                    if (raw.empty() || raw == "--")
                        continue;
                    double value = 0;
                    if (label == "MIN" && raw.find(':') != std::string::npos)
                        value = clock_minutes(raw);
                    else if (label == "3PT" && raw.find('-') != std::string::npos)
                        value =
                            number(raw.substr(0, raw.find('-')), std::numeric_limits<double>::quiet_NaN());
                    else
                        value = number(athlete["stats"][i], std::numeric_limits<double>::quiet_NaN());
                    if (std::isfinite(value) && value >= 0)
                        live[keys.at(label)] = value;
                }
                auto p = s["players"].value(key, json::object());
                p["name"] = name;
                p["team"] = team;
                p["live"] = live;
                p["source"] = p.value("source", "espn");
                p["starterConfirmed"] = athlete.value("starter", false);
                p["starterGameId"] = game.value("id", "");
                s["players"][key] = p;
            }
        }
    }
}
void parse_player_logs(const json &doc, json &s, const std::string &fetched) {
    auto rows = nba_rows(doc);
    std::map<std::string, json> groups;
    for (const auto &row : rows) {
        if (!row.contains("PLAYER_NAME") || !row.contains("MIN") || !row.contains("GAME_DATE"))
            continue;
        auto name = text(row["PLAYER_NAME"]), key = normalize_name(name);
        auto date = clean_date(text(row["GAME_DATE"]));
        if (!parse_time(date) || number(row["MIN"]) < 0.1)
            continue;
        json log = {{"date", date},
                    {"minutes", number(row["MIN"])},
                    {"home", text(row.value("MATCHUP", json(""))).find('@') == std::string::npos}};
        for (const auto &pair : std::map<std::string, std::string>{{"PTS", "points"},
                                                                   {"REB", "rebounds"},
                                                                   {"AST", "assists"},
                                                                   {"FG3M", "threes"},
                                                                   {"STL", "steals"},
                                                                   {"BLK", "blocks"},
                                                                   {"FGM", "fgm"},
                                                                   {"FGA", "fga"},
                                                                   {"FTA", "fta"},
                                                                   {"FTM", "ftm"},
                                                                   {"FG3A", "threeAttempts"},
                                                                   {"OREB", "oreb"},
                                                                   {"TOV", "turnovers"},
                                                                   {"PF", "fouls"}})
            if (row.contains(pair.first) && !row[pair.first].is_null()) {
                double value = number(row[pair.first], std::numeric_limits<double>::quiet_NaN());
                if (std::isfinite(value) && value >= 0)
                    log[pair.second] = value;
            }
        if (!log.contains("points") || !log.contains("rebounds") || !log.contains("assists") ||
            !log.contains("threes"))
            continue;
        if (!groups.count(key))
            groups[key] = {{"name", name},
                           {"nbaId", text(row.value("PLAYER_ID", json("")))},
                           {"team", canonical_team(text(row.value("TEAM_ABBREVIATION", json(""))))},
                           {"logs", json::array()},
                           {"latestTeamDate", date},
                           {"source", "nba_stats"},
                           {"dataAsOf", fetched},
                           {"injury", "Unknown"}};
        auto &group = groups[key];
        if (date > group.value("latestTeamDate", "")) {
            group["team"] = canonical_team(text(row.value("TEAM_ABBREVIATION", json(""))));
            group["latestTeamDate"] = date;
        }
        group["logs"].push_back(log);
    }
    for (auto &item : groups) {
        auto &p = item.second;
        std::sort(p["logs"].begin(), p["logs"].end(),
                  [](const json &x, const json &y) { return x["date"] > y["date"]; });
        if (p["logs"].size() > 60)
            p["logs"].erase(p["logs"].begin() + 60, p["logs"].end());
        p.erase("latestTeamDate");
        auto existing = s["players"].value(item.first, json::object());
        for (auto key :
             {"live", "injury", "injuryDetail", "rosterAsOf", "espnId", "starterConfirmed", "starterGameId"})
            if (existing.contains(key))
                p[key] = existing[key];
        if (existing.contains("rosterAsOf") &&
            now_seconds() - parse_time(existing.value("rosterAsOf", "")) < 86400)
            p["team"] = existing["team"];
        s["players"][item.first] = p;
    }
}
void parse_team_ratings(const json &doc, json &s, const std::string &fetched) {
    for (const auto &row : nba_rows(doc)) {
        if (!row.contains("TEAM_NAME") || !row.contains("PACE") || !row.contains("OFF_RATING") ||
            !row.contains("DEF_RATING"))
            continue;
        std::string abbr = text(row.value("TEAM_ABBREVIATION", json("")));
        if (abbr.empty()) {
            const std::map<std::string, std::string> names = {{"Atlanta Hawks", "ATL"},
                                                              {"Boston Celtics", "BOS"},
                                                              {"Brooklyn Nets", "BKN"},
                                                              {"Charlotte Hornets", "CHA"},
                                                              {"Chicago Bulls", "CHI"},
                                                              {"Cleveland Cavaliers", "CLE"},
                                                              {"Dallas Mavericks", "DAL"},
                                                              {"Denver Nuggets", "DEN"},
                                                              {"Detroit Pistons", "DET"},
                                                              {"Golden State Warriors", "GSW"},
                                                              {"Houston Rockets", "HOU"},
                                                              {"Indiana Pacers", "IND"},
                                                              {"LA Clippers", "LAC"},
                                                              {"Los Angeles Clippers", "LAC"},
                                                              {"Los Angeles Lakers", "LAL"},
                                                              {"Memphis Grizzlies", "MEM"},
                                                              {"Miami Heat", "MIA"},
                                                              {"Milwaukee Bucks", "MIL"},
                                                              {"Minnesota Timberwolves", "MIN"},
                                                              {"New Orleans Pelicans", "NOP"},
                                                              {"New York Knicks", "NYK"},
                                                              {"Oklahoma City Thunder", "OKC"},
                                                              {"Orlando Magic", "ORL"},
                                                              {"Philadelphia 76ers", "PHI"},
                                                              {"Phoenix Suns", "PHX"},
                                                              {"Portland Trail Blazers", "POR"},
                                                              {"Sacramento Kings", "SAC"},
                                                              {"San Antonio Spurs", "SAS"},
                                                              {"Toronto Raptors", "TOR"},
                                                              {"Utah Jazz", "UTA"},
                                                              {"Washington Wizards", "WAS"}};
            auto found = names.find(text(row["TEAM_NAME"]));
            if (found != names.end())
                abbr = found->second;
        }
        if (abbr.empty() || number(row["PACE"]) < 70 || number(row["PACE"]) > 130 ||
            number(row["OFF_RATING"]) < 70 || number(row["OFF_RATING"]) > 150 ||
            number(row["DEF_RATING"]) < 70 || number(row["DEF_RATING"]) > 150)
            continue;
        auto &t = ensure_team(s, abbr);
        t["name"] = row["TEAM_NAME"];
        t["nbaId"] = text(row.value("TEAM_ID", json("")));
        t["pace"] = number(row["PACE"]);
        t["offRating"] = number(row["OFF_RATING"]);
        t["defRating"] = number(row["DEF_RATING"]);
        t["dataAsOf"] = fetched;
        t["source"] = "nba_stats";
    }
}
void parse_team_logs(const json &doc, json &s, const std::string &fetched) {
    auto rows = nba_rows(doc);
    std::map<std::string, std::vector<json>> games;
    std::set<std::string> initialized;
    for (const auto &row : rows)
        if (row.contains("TEAM_ABBREVIATION") && row.contains("GAME_ID"))
            games[text(row["GAME_ID"])].push_back(row);
    std::vector<std::pair<std::string, std::vector<json>>> chronological(games.begin(), games.end());
    std::sort(chronological.begin(), chronological.end(), [](const auto &a, const auto &b) {
        return text(a.second[0].value("GAME_DATE", json(""))) <
               text(b.second[0].value("GAME_DATE", json("")));
    });
    std::map<std::string, double> elos;
    for (const auto &item : chronological) {
        const auto &pair = item.second;
        if (pair.size() != 2)
            continue;
        auto t1 = canonical_team(text(pair[0]["TEAM_ABBREVIATION"])),
             t2 = canonical_team(text(pair[1]["TEAM_ABBREVIATION"]));
        double p1 = number(pair[0].value("PTS", json(0))), p2 = number(pair[1].value("PTS", json(0)));
        if (p1 < 30 || p2 < 30 || p1 == p2)
            continue;
        for (auto t : {t1, t2})
            if (!elos.count(t))
                elos[t] = 1500;
        bool first_home = text(pair[0].value("MATCHUP", json(""))).find('@') == std::string::npos;
        double expected = 1 / (1 + std::pow(10, ((elos[t2] - elos[t1]) + (first_home ? -65 : 65)) / 400));
        double delta = 20 * ((p1 > p2 ? 1.0 : 0.0) - expected);
        elos[t1] += delta;
        elos[t2] -= delta;
        for (int i = 0; i < 2; ++i) {
            auto abbr = i == 0 ? t1 : t2;
            auto &team = ensure_team(s, abbr);
            if (!initialized.count(abbr)) {
                team["logs"] = json::array();
                initialized.insert(abbr);
            }
            auto date = clean_date(text(pair[i].value("GAME_DATE", json(""))));
            if (!parse_time(date))
                continue;
            team["logs"].push_back(
                {{"date", date},
                 {"points", i == 0 ? p1 : p2},
                 {"margin", i == 0 ? p1 - p2 : p2 - p1},
                 {"home", text(pair[i].value("MATCHUP", json(""))).find('@') == std::string::npos}});
            team["elo"] = elos[abbr];
            team["dataAsOf"] = fetched;
            team["source"] = "nba_stats";
        }
    }
    for (const auto &abbr : initialized) {
        auto &t = s["teams"][abbr];
        std::sort(t["logs"].begin(), t["logs"].end(),
                  [](const json &a, const json &b) { return a["date"] > b["date"]; });
        if (t["logs"].size() > 60)
            t["logs"].erase(t["logs"].begin() + 60, t["logs"].end());
        if (!t["logs"].empty())
            t["restDays"] = std::min(
                30LL,
                std::max(0LL, (now_seconds() - parse_time(t["logs"][0].value("date", ""))) / 86400 - 1));
    }
}

json scrape_saved_html(const std::string &html, const std::string &player, const std::string &team,
                       const std::string &observed_at) {
    if (html.size() > 4 * 1024 * 1024)
        throw std::invalid_argument("Saved HTML must be under 4 MB");
    // Read data only. Never evaluate JS, visit links, or fetch user-specified URLs.
    std::regex scripts("<script\\b([^>]*)>([\\s\\S]*?)</script>", std::regex::icase);
    json scraped = empty_snapshot("live");
    for (std::sregex_iterator it(html.begin(), html.end(), scripts), end; it != end; ++it) {
        auto attributes = (*it)[1].str(), body = (*it)[2].str();
        if (attributes.find("application/json") != std::string::npos) {
            try {
                auto doc = json::parse(body);
                if (doc.is_object() && (doc.contains("quotes") || doc.contains("players")))
                    return validate_import(doc);
                walk_espn(doc, scraped);
            } catch (const json::exception &) {
            }
        } else {
            auto marker = body.find("__espnfitt__");
            if (marker != std::string::npos) {
                auto start = body.find('{', marker);
                if (start != std::string::npos) {
                    try {
                        walk_espn(balanced_json(body, start), scraped);
                    } catch (const json::exception &) {
                    }
                }
            }
        }
    }
    if (!scraped["games"].empty()) {
        if (!scraped["quotes"].empty() && observed_at.empty())
            throw std::invalid_argument("Saved ESPN HTML prices need their original observedAt UTC time. "
                                        "Import receipt time is not a fresh price timestamp.");
        for (auto &quote : scraped["quotes"])
            quote["observedAt"] = observed_at;
        return validate_import({{"games", scraped["games"]}, {"quotes", scraped["quotes"]}});
    }
    if (player.empty() || team.empty())
        throw std::invalid_argument("No supported embedded ESPN/Edgecourt data found. For Basketball "
                                    "Reference game-log HTML, provide a player name and current team.");
    std::regex row_pattern("<tr\\b[^>]*>([\\s\\S]*?)</tr>", std::regex::icase),
        cell_pattern("<t[dh]\\b[^>]*data-stat=[\"']([^\"']+)[\"'][^>]*>([\\s\\S]*?)</t[dh]>",
                     std::regex::icase);
    json logs = json::array();
    std::set<std::string> dates;
    for (std::sregex_iterator it(html.begin(), html.end(), row_pattern), end; it != end; ++it) {
        auto body = (*it)[1].str();
        std::map<std::string, std::string> cells;
        for (std::sregex_iterator cell(body.begin(), body.end(), cell_pattern); cell != end; ++cell)
            cells[(*cell)[1].str()] = strip_tags((*cell)[2].str());
        auto date = cells["date_game"];
        auto minutes = cells["mp"];
        if (!parse_time(date) || minutes.empty() || !dates.insert(date).second)
            continue;
        double played = minutes.find(':') == std::string::npos ? number(minutes) : clock_minutes(minutes);
        if (played <= 0)
            continue;
        json log = {{"date", date}, {"minutes", played}, {"home", cells["game_location"] != "@"}};
        for (const auto &pair : std::map<std::string, std::string>{{"pts", "points"},
                                                                   {"trb", "rebounds"},
                                                                   {"ast", "assists"},
                                                                   {"fg3", "threes"},
                                                                   {"stl", "steals"},
                                                                   {"blk", "blocks"},
                                                                   {"fga", "fga"},
                                                                   {"fg", "fgm"},
                                                                   {"fta", "fta"},
                                                                   {"ft", "ftm"},
                                                                   {"fg3a", "threeAttempts"},
                                                                   {"orb", "oreb"},
                                                                   {"tov", "turnovers"},
                                                                   {"pf", "fouls"}})
            if (cells.count(pair.first) && !cells[pair.first].empty())
                log[pair.second] = number(cells[pair.first]);
        if (log.contains("points") && log.contains("rebounds") && log.contains("assists") &&
            log.contains("threes"))
            logs.push_back(log);
    }
    if (logs.empty())
        throw std::invalid_argument("No valid historical game rows found in the saved HTML");
    if (logs.size() > 150)
        logs.erase(logs.begin() + 150, logs.end());
    return validate_import(
        {{"players",
          json::array(
              {{{"name", player}, {"team", canonical_team(team)}, {"logs", logs}, {"injury", "Unknown"}}})}});
}

PublicFeeds::PublicFeeds(std::filesystem::path path) : cache_dir_(std::move(path)) {
    std::filesystem::create_directories(cache_dir_);
}
FeedResponse PublicFeeds::get(const std::string &url, int ttl, int max_stale) {
    FeedResponse response;
    if (cancelled_ && cancelled_->load()) {
        response.error = "Refresh cancelled";
        return response;
    }
    auto path = cache_dir_ / (stable_id(url) + ".json");
    json cache;
    try {
        if (std::filesystem::exists(path) && std::filesystem::file_size(path) < MAX_RESPONSE + 1024) {
            std::ifstream stream(path);
            stream >> cache;
        }
    } catch (...) {
        cache = nullptr;
    }
    const auto age = cache.is_object() ? now_seconds() - parse_time(cache.value("fetchedAt", "")) : 1LL << 60;
    if (cache.is_object() && cache.value("url", "") == url && age >= 0 && age < ttl) {
        response.data = cache["body"];
        response.fetched_at = cache.value("fetchedAt", "");
        response.cached = true;
        return response;
    }
    if (requests_ >= 24) {
        response.error = "Per-refresh public-source request budget reached";
    } else {
        if (requests_ > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        ++requests_;
        auto http = public_get(url);
        response.status = http.status;
        response.error = http.error;
        if (http.status == 200 && http.error.empty()) {
            try {
                response.data = json::parse(http.body);
                response.fetched_at = now_iso();
                json entry = {{"url", url}, {"fetchedAt", response.fetched_at}, {"body", response.data}};
                std::ofstream stream(path, std::ios::trunc);
                stream << entry.dump();
                return response;
            } catch (...) {
                response.error = "Public source returned an unsupported/non-JSON response";
            }
        }
    }
    if (max_stale > 0 && cache.is_object() && cache.value("url", "") == url && age >= 0 && age < max_stale) {
        response.data = cache["body"];
        response.fetched_at = cache.value("fetchedAt", "");
        response.cached = true;
        response.stale = true;
    }
    return response;
}
json PublicFeeds::refresh(const json &previous, const std::atomic<bool> *cancelled) {
    cancelled_ = cancelled;
    requests_ = 0;
    json s = empty_snapshot("live");
    s["players"] = previous.value("players", json::object());
    s["teams"] = previous.value("teams", json::object());
    // The date window uses UTC dates so no local timezone assumption enters scheduled starts.
    auto begin = iso_at(now_seconds() - 86400).substr(0, 10),
         end = iso_at(now_seconds() + 7 * 86400).substr(0, 10);
    begin.erase(std::remove(begin.begin(), begin.end(), '-'), begin.end());
    end.erase(std::remove(end.begin(), end.end(), '-'), end.end());
    auto espn = get("https://site.api.espn.com/apis/site/v2/sports/basketball/nba/scoreboard?dates=" + begin +
                        "-" + end + "&limit=100",
                    45, 0);
    if (!espn.data.is_null())
        parse_espn_scoreboard(espn.data, s, espn.fetched_at);
    s["sources"].push_back(source("espn", "ESPN schedule + public moneylines", espn,
                                  static_cast<int>(s["games"].size()),
                                  "Public, undocumented endpoint. Moneylines only when the response "
                                  "explicitly includes them; no inferred player prices."));
    // A fixed public-page scraper, not a challenge bypass: only use it when the JSON
    // endpoint changes its contract (404 / successful unsupported body), never after 403/429.
    if ((espn.status == 404 || espn.status == 200) &&
        (espn.data.is_null() || !espn.data.contains("events")) && requests_ < 24 &&
        !(cancelled_ && cancelled_->load())) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        ++requests_;
        auto page = public_get("https://www.espn.com/nba/scoreboard");
        FeedResponse html;
        html.status = page.status;
        html.error = page.error;
        html.fetched_at = now_iso();
        if (page.status == 200 && page.error.empty())
            try {
                auto imported = scrape_saved_html(page.body, "", "", html.fetched_at);
                merge_import(s, imported);
                html.data = imported;
                for (auto &game : s["games"])
                    if (game.value("source", "") == "user_import") {
                        game["source"] = "espn_html";
                        game["dataAsOf"] = html.fetched_at;
                        for (const auto &side : {std::string("home"), std::string("away")}) {
                            auto &t = ensure_team(s, game.value(side, ""));
                            t["name"] = game.value(side + "Name", game.value(side, ""));
                            if (game.contains(side + "EspnId"))
                                t["espnId"] = game[side + "EspnId"];
                        }
                    }
                for (auto &q : s["quotes"])
                    if (q.value("source", "") == "user_import") {
                        q["source"] = "espn_html";
                        q["timestampKind"] = "retrieved";
                        q["observedAt"] = html.fetched_at;
                    }
            } catch (const std::exception &e) {
                html.error = std::string("Public-page embedded data unavailable: ") + e.what();
            }
        s["sources"].push_back(source("espn-html", "ESPN public scoreboard HTML", html,
                                      static_cast<int>(s["games"].size()),
                                      "Extracted embedded JSON from a fixed public page. Today's schedule "
                                      "only; no JavaScript execution or arbitrary crawling."));
    }
    auto nba = get("https://cdn.nba.com/static/json/liveData/scoreboard/todaysScoreboard_00.json", 30, 0);
    if (!nba.data.is_null())
        parse_nba_scoreboard(nba.data, s, nba.fetched_at);
    s["sources"].push_back(
        source("nba-live", "NBA live scoreboard", nba, static_cast<int>(s["games"].size()),
               "Scores, period and game clock. NBA CDN is keyless; availability is not guaranteed."));
    auto season = nba_season(now_seconds());
    auto date_from = iso_at(now_seconds() - 210 * 86400).substr(0, 10);
    const auto player_url = [&](const std::string &year) {
        return "https://stats.nba.com/stats/playergamelogs?DateFrom=" + date_from +
               "&DateTo=&GameSegment=&LastNGames=0&LeagueID=00&Location=&MeasureType=Base&Month=0&"
               "OpponentTeamID=0&Outcome=&PORound=0&PerMode=Totals&Period=0&PlayerID=&Season=" +
               year +
               "&SeasonSegment=&SeasonType=Regular%20Season&ShotClockRange=&TeamID=0&VsConference=&"
               "VsDivision=";
    };
    auto players = get(player_url(season), 21600, 172800);
    if (!players.data.is_null() && nba_rows(players.data).empty()) {
        season = nba_season(now_seconds(), true);
        players = get(player_url(season), 21600, 172800);
    }
    if (!players.data.is_null())
        parse_player_logs(players.data, s, players.fetched_at);
    s["sources"].push_back(source("nba-players", "NBA historical player logs", players,
                                  static_cast<int>(s["players"].size()),
                                  "Recent minutes, P/R/A, 3PM, STL/BLK and shooting detail. Prior season is "
                                  "used only if the current-season response is empty. Season: " +
                                      season + "."));
    const auto ratings_url =
        "https://stats.nba.com/stats/"
        "leaguedashteamstats?Conference=&DateFrom=&DateTo=&Division=&GameScope=&GameSegment=&LastNGames=0&"
        "LeagueID=00&Location=&MeasureType=Advanced&Month=0&OpponentTeamID=0&Outcome=&PORound=0&PaceAdjust=N&"
        "PerMode=PerGame&Period=0&PlayerExperience=&PlayerPosition=&PlusMinus=N&Rank=N&Season=" +
        season +
        "&SeasonSegment=&SeasonType=Regular%20Season&ShotClockRange=&StarterBench=&TeamID=0&TwoWay=0&"
        "VsConference=&VsDivision=";
    auto ratings = get(ratings_url, 21600, 172800);
    if (!ratings.data.is_null())
        parse_team_ratings(ratings.data, s, ratings.fetched_at);
    auto teamlogs = get("https://stats.nba.com/stats/leaguegamelog?Counter=0&DateFrom=" + date_from +
                            "&DateTo=&Direction=DESC&LeagueID=00&PlayerOrTeam=T&Season=" + season +
                            "&SeasonType=Regular%20Season&Sorter=DATE",
                        21600, 172800);
    if (!teamlogs.data.is_null())
        parse_team_logs(teamlogs.data, s, teamlogs.fetched_at);
    s["sources"].push_back(
        source("nba-teams", "NBA advanced team environment", ratings, static_cast<int>(s["teams"].size()),
               "Pace, offensive/defensive ratings. Historical team results supply Elo, variance and rest."));
    s["sources"].push_back(source(
        "nba-teamlogs", "NBA team results + chronological Elo", teamlogs, static_cast<int>(s["teams"].size()),
        "Elo is updated only after resolved paired games. No trained model weights are claimed."));
    std::set<std::string> active;
    for (const auto &game : s["games"])
        if (game.value("status", "") != "final" &&
            parse_time(game.value("startsAt", "")) < now_seconds() + 3 * 86400) {
            active.insert(game.value("home", ""));
            active.insert(game.value("away", ""));
        }
    int roster_count = 0, roster_failures = 0;
    if (!s["games"].empty())
        for (const auto &team : active) {
            if (!s["teams"].contains(team) || !s["teams"][team].contains("espnId"))
                continue;
            auto roster = get("https://site.api.espn.com/apis/site/v2/sports/basketball/nba/teams/" +
                                  s["teams"][team].value("espnId", "") + "/roster",
                              3600, 7200);
            if (!roster.data.is_null()) {
                parse_espn_roster(roster.data, s, team, roster.fetched_at);
                ++roster_count;
            } else
                ++roster_failures;
        }
    s["sources"].push_back(
        {{"id", "rosters"},
         {"label", "ESPN roster + injury context"},
         {"state", roster_count ? "ok" : "unavailable"},
         {"updatedAt", roster_count ? now_iso() : ""},
         {"count", roster_count},
         {"message", "Fetched " + std::to_string(roster_count) + " active team rosters; " +
                         std::to_string(roster_failures) +
                         " failed. Injury feeds may lag and are not a confirmed starting lineup."}});
    int summaries = 0;
    if (!s["games"].empty())
        for (const auto &game : s["games"]) {
            if (game.value("status", "") != "live" || !game.contains("espnId") || summaries >= 6)
                continue;
            auto summary = get("https://site.api.espn.com/apis/site/v2/sports/basketball/nba/summary?event=" +
                                   game.value("espnId", ""),
                               30, 0);
            if (!summary.data.is_null()) {
                parse_espn_summary(summary.data, s, game, summary.fetched_at);
                ++summaries;
            }
        }
    s["sources"].push_back({{"id", "boxscores"},
                            {"label", "ESPN live player box scores"},
                            {"state", summaries ? "ok" : "idle"},
                            {"updatedAt", summaries ? now_iso() : ""},
                            {"count", summaries},
                            {"message", "Fresh accrued stats are mandatory for in-play props. No live player "
                                        "value is extrapolated from a pregame-only feed."}});
    s["sources"].push_back(
        {{"id", "prop-odds"},
         {"label", "Player-prop & team-total odds"},
         {"state", "import_required"},
         {"updatedAt", ""},
         {"count", 0},
         {"message", "There is no bundled reliable free/keyless all-books prop feed. Import current CSV/JSON "
                     "quotes. First basket also requires explicit history and confirmed starters."}});
    // Keep only players on active teams in the browser workspace. Cache retains full historical responses.
    for (auto it = s["players"].begin(); it != s["players"].end();) {
        auto team = it.value().value("team", "");
        if (!active.count(team))
            it = s["players"].erase(it);
        else
            ++it;
    }
    s["generatedAt"] = now_iso();
    return s;
}
} // namespace edgecourt
