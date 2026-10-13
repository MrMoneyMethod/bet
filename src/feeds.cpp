#include "feeds.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <random>
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
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace edgecourt {
namespace {
constexpr size_t MAX_RESPONSE = 16 * 1024 * 1024;
// Hard per-refresh network budget shared by every public source. Schedule and
// moneyline fetches run first so stats/roster enrichment cannot starve them.
// The budget also covers the league-wide official datasets below; they are
// one request each and season datasets cache for 6 hours, so the budget is
// only approached on a cold cache. The small increase accommodates the four
// optional context feeds without removing the overall burst bound.
constexpr int MAX_REQUESTS_PER_REFRESH = 72;
// API-Basketball's free key tier is documented at 100 requests/day. This
// adapter only needs its schedule crosswalk and moneyline feed, which are
// fetched no more than once per 24 hours; reserve a further 20 requests for
// the user's other clients sharing the same key.
constexpr int API_BASKETBALL_RESERVE_REQUESTS = 20;
constexpr int API_BASKETBALL_CACHE_SECONDS = 24 * 60 * 60;
constexpr int API_BASKETBALL_FAILURE_RETRY_SECONDS = 24 * 60 * 60;
// SharpAPI documents 12 requests/minute and a 60-second delay on Free. Poll
// every five minutes and follow at most four odds pages, keeping even a burst
// well below that documented per-minute limit. Usage reports expose daily
// counts, but no daily request cap is published.
constexpr int SHARPAPI_CACHE_SECONDS = 5 * 60;
constexpr int SHARPAPI_MAX_STALE_SECONDS = 15 * 60;
constexpr int SHARPAPI_FAILURE_RETRY_SECONDS = 5 * 60;
constexpr int SHARPAPI_MAX_PAGES = 4;
// Sportsbook API2 plan limits conflict (publisher: 50/day; RapidAPI listing:
// 150/day). Use the lower figure and leave ten calls for other consumers of
// the user's RapidAPI key. Normal polling is below this hard daily ceiling.
constexpr int SPORTSBOOK_API2_DAILY_LIMIT = 50;
constexpr int SPORTSBOOK_API2_RESERVE_REQUESTS = 10;
constexpr int SPORTSBOOK_API2_COMPETITIONS_CACHE_SECONDS = 24 * 60 * 60;
constexpr int SPORTSBOOK_API2_EVENTS_CACHE_SECONDS = 6 * 60 * 60;
constexpr int SPORTSBOOK_API2_ODDS_CACHE_SECONDS = 2 * 60 * 60;
constexpr int NBA_GO_CACHE_SECONDS = 6 * 60 * 60;
constexpr int THESPORTSDB_CACHE_SECONDS = 6 * 60 * 60;
constexpr int SPORTSCORE_CACHE_SECONDS = 6 * 60 * 60;
constexpr char SPORTSBOOK_API2_RAPIDAPI_HOST[] = "sportsbook-api2.p.rapidapi.com";
bool api_basketball_host(const std::string &url) {
    return url.rfind("https://v1.basketball.api-sports.io/", 0) == 0;
}
bool sharpapi_host(const std::string &url) {
    return url.rfind("https://api.sharpapi.io/", 0) == 0;
}
// Inferred from the RapidAPI product slug, not confirmed by an authenticated
// call; compare with RapidAPI's generated X-RapidAPI-Host snippet before use.
bool sportsbook_api2_host(const std::string &url) {
    return url.rfind(std::string("https://") + SPORTSBOOK_API2_RAPIDAPI_HOST + "/", 0) == 0;
}
bool thesportsdb_host(const std::string &url) {
    return url.rfind("https://www.thesportsdb.com/api/v2/json/", 0) == 0;
}
bool sportscore_host(const std::string &url) {
    return url.rfind("https://sportscore.com/api/", 0) == 0;
}
std::string environment_key(const char *name) {
    const char *value = std::getenv(name);
    if (!value)
        return "";
    std::string key(value);
    // Do not let environment-variable newlines inject additional HTTP headers.
    while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back())))
        key.pop_back();
    size_t first = 0;
    while (first < key.size() && std::isspace(static_cast<unsigned char>(key[first])))
        ++first;
    key.erase(0, first);
    if (key.size() > 4096 ||
        std::any_of(key.begin(), key.end(), [](unsigned char c) { return std::isspace(c) || std::iscntrl(c); }))
        return "";
    return key;
}
std::string api_basketball_key() {
    return environment_key("API_BASKETBALL_KEY");
}
std::string sharpapi_key() {
    return environment_key("SHARPAPI_KEY");
}
std::string rapidapi_key() {
    return environment_key("RAPIDAPI_KEY");
}
bool sportsbook_api2_host_confirmed() {
    // Never send a RapidAPI credential to an inferred product host until the
    // user confirms that exact host against RapidAPI's generated code snippet.
    return environment_key("RAPIDAPI_HOST") == SPORTSBOOK_API2_RAPIDAPI_HOST;
}
std::string thesportsdb_key() {
    return environment_key("THESPORTSDB_KEY");
}
std::string sportscore_key() {
    return environment_key("SPORTSCORE_KEY");
}
json read_small_json(const std::filesystem::path &path, size_t max_size = 4096) {
    try {
        if (std::filesystem::exists(path) && std::filesystem::file_size(path) <= max_size) {
            std::ifstream stream(path);
            json result;
            stream >> result;
            return result;
        }
    } catch (...) {
    }
    return json::object();
}
void write_json_file(const std::filesystem::path &path, const json &value) {
    const auto temporary = std::filesystem::path(path.string() + ".tmp");
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream)
            return;
        stream << value.dump();
        stream.flush();
        if (!stream)
            return;
    }
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        std::filesystem::remove(temporary);
#else
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    if (error)
        std::filesystem::remove(temporary);
#endif
}
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
// ESPN season.type: 1 preseason, 2 regular season, 3 postseason, 4 off-season.
// Games are kept under the type ESPN publishes for them; preseason stays in the
// schedule/data (with its prices) even though model projections stay guarded.
std::string espn_season_type(const json &event) {
    const auto season = event.value("season", json::object());
    const auto slug = season.value("slug", "");
    if (slug.find("preseason") != std::string::npos || slug.find("pre-season") != std::string::npos)
        return "preseason";
    if (slug.find("post") != std::string::npos || slug.find("playoff") != std::string::npos)
        return "playoffs";
    switch (static_cast<int>(number(season.value("type", json(2))))) {
    case 1:
        return "preseason";
    case 3:
        return "playoffs";
    default:
        return "regular";
    }
}
// NBA CDN game ids lead with a competition prefix: 001 preseason, 002 regular
// season, 004 playoffs (other prefixes keep the regular-season default).
std::string nba_season_type(const std::string &id) {
    if (id.rfind("001", 0) == 0)
        return "preseason";
    if (id.rfind("004", 0) == 0)
        return "playoffs";
    return "regular";
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
        "^https://(site\\.api\\.espn\\.com|cdn\\.nba\\.com|stats\\.nba\\.com|www\\.espn\\.com|"
        "gamma-api\\.polymarket\\.com|external-api\\.kalshi\\.com|guest\\.api\\.arcadia\\.pinnacle\\.com|"
        "v1\\.basketball\\.api-sports\\.io|api\\.sharpapi\\.io|sportsbook-api2\\.p\\.rapidapi\\.com|"
        "api\\.server\\.nbaapi\\.com|www\\.thesportsdb\\.com|sportscore\\.com)"
        "/[^\\r\\n\\t \"]*$");
    return url.size() < 4096 && std::regex_match(url, expression);
}
std::vector<std::string> espn_scoreboard_day_urls(long long first_epoch, long long last_epoch) {
    std::vector<std::string> urls;
    const auto first = parse_time(iso_at(first_epoch).substr(0, 10) + "T00:00:00Z"),
               last = parse_time(iso_at(last_epoch).substr(0, 10) + "T00:00:00Z");
    if (!first || !last || last < first)
        return urls;
    // Inclusive UTC days, hard-capped so a bad window can never fan out into an
    // unbounded request burst against a public, undocumented endpoint.
    for (auto day = first; day <= last && urls.size() < 14; day += 86400) {
        auto stamp = iso_at(day).substr(0, 10);
        stamp.erase(std::remove(stamp.begin(), stamp.end(), '-'), stamp.end());
        urls.push_back("https://site.api.espn.com/apis/site/v2/sports/basketball/nba/scoreboard?dates=" +
                       stamp + "&limit=100");
    }
    return urls;
}
std::string api_basketball_season(long long epoch_seconds) {
    const auto date = iso_at(epoch_seconds);
    int year = std::stoi(date.substr(0, 4));
    const int month = std::stoi(date.substr(5, 2));
    if (month < 10)
        --year;
    return std::to_string(year) + "-" + std::to_string(year + 1);
}
bool api_basketball_quota_allows(int current, int daily_limit, int reserve, int calls_needed) {
    return current >= 0 && daily_limit > 0 && reserve >= 0 && calls_needed > 0 &&
           daily_limit - current >= reserve + calls_needed;
}
std::string pinnacle_device_uuid() {
    // Pinnacle's public web client identifies itself with an anonymous,
    // per-visitor device UUID (visible in any browser's request headers to
    // the guest API). It is not an account or credential; generate one per
    // process and reuse it, exactly like a browser profile would.
    static const std::string uuid = [] {
        std::mt19937_64 rng(std::random_device{}());
        auto part = [&rng] {
            std::ostringstream out;
            out << std::hex << std::setw(8) << std::setfill('0')
                << static_cast<unsigned long>(rng() & 0xffffffffULL);
            return out.str();
        };
        return part() + "-" + part() + "-" + part() + "-" + part();
    }();
    return uuid;
}
std::vector<std::pair<std::string, std::string>> public_request_headers(const std::string &url) {
    std::vector<std::pair<std::string, std::string>> headers = {
        {"Accept", "application/json, text/html;q=0.9"}, {"Referer", "https://www.nba.com/"}};
    if (api_basketball_host(url)) {
        const auto key = api_basketball_key();
        if (!key.empty())
            headers.push_back({"x-apisports-key", key});
    }
    if (sharpapi_host(url)) {
        const auto key = sharpapi_key();
        if (!key.empty())
            headers.push_back({"X-API-Key", key});
    }
    if (sportsbook_api2_host(url) && sportsbook_api2_host_confirmed()) {
        const auto key = rapidapi_key();
        if (!key.empty()) {
            headers.push_back({"X-RapidAPI-Key", key});
            headers.push_back({"X-RapidAPI-Host", SPORTSBOOK_API2_RAPIDAPI_HOST});
        }
    }
    if (thesportsdb_host(url)) {
        const auto key = thesportsdb_key();
        if (!key.empty())
            headers.push_back({"X-API-KEY", key});
    }
    if (sportscore_host(url)) {
        const auto key = sportscore_key();
        if (!key.empty())
            headers.push_back({"X-Api-Key", key});
    }
    if (url.rfind("https://guest.api.arcadia.pinnacle.com/", 0) == 0) {
        // Pinnacle's guest tier (the API its public website calls) answers
        // 401/404 without the site key. This is the key Pinnacle publishes to
        // every anonymous website visitor (pinnacle.com/config/app.json,
        // stable since 2019) — not an account, login or user credential. If
        // Pinnacle rotates it, the source reports unavailable honestly.
        headers[1].second = "https://www.pinnacle.com/";
        headers.push_back({"X-API-Key", "CmX2KcMrXuFmNg6YFbmTxE0y9CIrOi0R"});
        headers.push_back({"X-Device-UUID", pinnacle_device_uuid()});
    }
    return headers;
}
HttpResponse public_get(const std::string &url) {
    HttpResponse response;
    if (!allowed_data_url(url)) {
        response.error = "Data URL is outside the fixed public-source allowlist";
        return response;
    }
    const auto request_headers = public_request_headers(url);
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
    std::wstring header_block;
    for (const auto &header : request_headers)
        header_block += wide(header.first + ": " + header.second + "\r\n");
    if (request &&
        WinHttpSendRequest(request, header_block.c_str(), static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0,
                           0, 0) &&
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
    auto sensitive_header = [](const std::string &name) {
        std::string normalized = name;
        std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return normalized == "x-apisports-key" || normalized == "x-api-key" ||
               normalized == "x-rapidapi-key" || normalized == "authorization";
    };
    auto quote_curl_config = [](const std::string &value) {
        std::string escaped = "\"";
        for (char c : value) {
            if (c == '\\' || c == '"')
                escaped += '\\';
            escaped += c;
        }
        escaped += '"';
        return escaped;
    };
    std::string secret_config;
    std::vector<std::pair<std::string, std::string>> ordinary_headers;
    for (const auto &header : request_headers) {
        if (sensitive_header(header.first))
            secret_config += "header = " + quote_curl_config(header.first + ": " + header.second) + "\n";
        else
            ordinary_headers.push_back(header);
    }
    int pipes[2];
    if (pipe(pipes) != 0) {
        response.error = "Cannot start HTTPS transport";
        return response;
    }
    int secret_sockets[2] = {-1, -1};
    if (!secret_config.empty() && socketpair(AF_UNIX, SOCK_STREAM, 0, secret_sockets) != 0) {
        close(pipes[0]);
        close(pipes[1]);
        response.error = "Cannot start secure header transport";
        return response;
    }
    pid_t pid = fork();
    if (pid == 0) {
        dup2(pipes[1], STDOUT_FILENO);
        if (!secret_config.empty())
            dup2(secret_sockets[1], STDIN_FILENO);
        int nullfd = open("/dev/null", O_WRONLY);
        if (nullfd >= 0)
            dup2(nullfd, STDERR_FILENO);
        close(pipes[0]);
        close(pipes[1]);
        if (!secret_config.empty()) {
            close(secret_sockets[0]);
            close(secret_sockets[1]);
        }
        std::vector<std::string> args = {"curl",        "--silent", "--show-error", "--connect-timeout",
                                         "4",           "--max-time", "8",          "--max-filesize",
                                         "16777216",    "--proto",    "=https",     "--user-agent",
                                         "Edgecourt/0.1 personal NBA research"};
        for (const auto &header : ordinary_headers) {
            args.push_back("-H");
            args.push_back(header.first + ": " + header.second);
        }
        if (!secret_config.empty()) {
            args.push_back("--config");
            args.push_back("-"); // Read credentials from a pipe, not the curl process arguments.
        }
        args.push_back("--write-out");
        args.push_back("\nEDGECOURT_STATUS:%{http_code}");
        args.push_back(url);
        std::vector<char *> argv;
        for (auto &arg : args)
            argv.push_back(arg.data());
        argv.push_back(nullptr);
        // The key-bearing environment belongs only to Edgecourt; curl gets
        // the sensitive header over stdin and must not inherit the keys too.
        unsetenv("API_BASKETBALL_KEY");
        unsetenv("SHARPAPI_KEY");
        unsetenv("RAPIDAPI_KEY");
        unsetenv("THESPORTSDB_KEY");
        unsetenv("SPORTSCORE_KEY");
        execvp("curl", argv.data());
        _exit(127);
    }
    close(pipes[1]);
    if (!secret_config.empty())
        close(secret_sockets[1]);
    if (pid < 0) {
        close(pipes[0]);
        if (!secret_config.empty())
            close(secret_sockets[0]);
        response.error = "Cannot fork HTTPS transport";
        return response;
    }
    if (!secret_config.empty()) {
        size_t offset = 0;
        while (offset < secret_config.size()) {
            const auto sent = send(secret_sockets[0], secret_config.data() + offset,
                                   secret_config.size() - offset, MSG_NOSIGNAL);
            if (sent < 0 && errno == EINTR)
                continue;
            if (sent <= 0) {
                response.error = "Cannot safely pass a provider key to HTTPS transport";
                break;
            }
            offset += static_cast<size_t>(sent);
        }
        shutdown(secret_sockets[0], SHUT_WR);
        close(secret_sockets[0]);
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
                  {"seasonType", espn_season_type(event)}};
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
            {"seasonType", nba_season_type(id)}};
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

namespace {
// Prediction-market / sharp-book quote helpers. External prices are only kept
// when they match a scheduled game already in the snapshot; nothing is invented.
std::string squish(std::string value) {
    std::string out;
    for (unsigned char c : value)
        if (std::isalnum(c))
            out += static_cast<char>(std::tolower(c));
    return out;
}
std::string abbr_from_name(const std::string &name) {
    static const std::map<std::string, std::string> names = {{"Atlanta Hawks", "ATL"},
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
    const auto flat = squish(name);
    for (const auto &entry : names)
        if (squish(entry.first) == flat)
            return entry.second;
    // Nickname-only labels ("Lakers") match when the label is the name's tail.
    for (const auto &entry : names) {
        const auto full = squish(entry.first);
        if (flat.size() >= 4 && full.size() > flat.size() &&
            full.compare(full.size() - flat.size(), flat.size(), flat) == 0)
            return entry.second;
    }
    return canonical_team(name);
}
std::string sharpapi_team_abbr(const std::string &name) {
    static const std::map<std::string, std::string> aliases = {
        {"atl", "ATL"}, {"atlhawks", "ATL"}, {"bkn", "BKN"}, {"bknnets", "BKN"},
        {"bos", "BOS"}, {"bosceltics", "BOS"}, {"cha", "CHA"}, {"chahornets", "CHA"},
        {"chi", "CHI"}, {"chibulls", "CHI"}, {"cle", "CLE"}, {"clecavaliers", "CLE"},
        {"dal", "DAL"}, {"dalmavericks", "DAL"}, {"den", "DEN"}, {"dennuggets", "DEN"},
        {"det", "DET"}, {"detpistons", "DET"}, {"gs", "GSW"}, {"gsw", "GSW"},
        {"gswarriors", "GSW"}, {"gswwarriors", "GSW"}, {"hou", "HOU"}, {"hourockets", "HOU"}, {"ind", "IND"},
        {"indpacers", "IND"}, {"lac", "LAC"}, {"laclippers", "LAC"}, {"lal", "LAL"},
        {"lalakers", "LAL"}, {"mem", "MEM"}, {"memgrizzlies", "MEM"}, {"mia", "MIA"},
        {"miaheat", "MIA"}, {"mil", "MIL"}, {"milbucks", "MIL"}, {"min", "MIN"},
        {"mintimberwolves", "MIN"}, {"no", "NOP"}, {"nop", "NOP"}, {"nopelicans", "NOP"}, {"nospelicans", "NOP"},
        {"ny", "NYK"}, {"nyk", "NYK"}, {"nyknicks", "NYK"}, {"okc", "OKC"},
        {"okcthunder", "OKC"}, {"orl", "ORL"}, {"orlmagic", "ORL"}, {"phi", "PHI"},
        {"phi76ers", "PHI"}, {"pho", "PHX"}, {"phosuns", "PHX"}, {"phx", "PHX"},
        {"phxsuns", "PHX"},
        {"por", "POR"}, {"porttrailblazers", "POR"}, {"sac", "SAC"}, {"sackings", "SAC"},
        {"sa", "SAS"}, {"sas", "SAS"}, {"sasspurs", "SAS"}, {"tor", "TOR"},
        {"torraptors", "TOR"}, {"uta", "UTA"}, {"utajazz", "UTA"}, {"utahjazz", "UTA"},
        {"wsh", "WAS"}, {"wshwizards", "WAS"}, {"was", "WAS"}, {"washwizards", "WAS"},
        {"waswizards", "WAS"}};
    const auto found = aliases.find(squish(name));
    return found == aliases.end() ? abbr_from_name(name) : found->second;
}
std::string sharpapi_book_name(const std::string &id) {
    static const std::map<std::string, std::string> names = {
        {"draftkings", "DraftKings"}, {"fanduel", "FanDuel"}, {"betmgm", "BetMGM"},
        {"caesars", "Caesars"}, {"fanatics", "Fanatics"}, {"betrivers", "BetRivers"},
        {"thescorebet", "theScore Bet"}, {"ballybet", "Bally Bet"}, {"rebet", "Rebet"},
        {"pinnacle", "Pinnacle"}, {"circasports", "Circa Sports"}, {"sbobet", "SBOBET"},
        {"1xbet", "1xBet"}, {"bet365", "bet365"}, {"betonline", "BetOnline"},
        {"betway", "Betway"}, {"skybet", "Sky Bet"}, {"unibet", "Unibet"},
        {"ladbrokes", "Ladbrokes"}, {"betfred", "Betfred"}, {"cloudbet", "Cloudbet"},
        {"prophetx", "ProphetX"}, {"betfair", "Betfair"}, {"novig", "Novig"},
        {"polymarket", "Polymarket"}, {"kalshi", "Kalshi"}};
    const auto normalized = normalize_name(id);
    const auto found = names.find(normalized);
    if (found != names.end())
        return found->second;
    std::string result;
    bool capitalize = true;
    for (unsigned char c : id) {
        if (std::isalnum(c)) {
            result += capitalize ? static_cast<char>(std::toupper(c)) : static_cast<char>(c);
            capitalize = false;
        } else {
            if (!result.empty() && result.back() != ' ')
                result += ' ';
            capitalize = true;
        }
    }
    return result.empty() ? "SharpAPI" : result;
}
bool sharpapi_duplicate_exchange(const std::string &id) {
    const auto normalized = normalize_name(id);
    return normalized == "polymarket" || normalized == "kalshi";
}
std::string encode_query_component(const std::string &value) {
    static const char hex[] = "0123456789ABCDEF";
    std::string encoded;
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            encoded += static_cast<char>(c);
        else {
            encoded += '%';
            encoded += hex[c >> 4];
            encoded += hex[c & 0x0f];
        }
    }
    return encoded;
}
json array_field(const json &value) {
    if (value.is_array())
        return value;
    if (value.is_string()) {
        try {
            auto parsed = json::parse(value.get<std::string>());
            if (parsed.is_array())
                return parsed;
        } catch (...) {
        }
    }
    return json::array();
}
// Public payloads routinely carry explicit JSON nulls where a string is
// expected; nlohmann's value(key, "") throws type_error.302 on those. Always
// read optional string fields through this helper in feed parsers.
std::string string_field(const json &object, const char *key, const std::string &fallback = "") {
    if (!object.is_object())
        return fallback;
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : fallback;
}
const json *find_game(const json &snapshot, const std::string &home, const std::string &away) {
    for (const auto &game : snapshot["games"])
        if (game.value("home", "") == home && game.value("away", "") == away)
            return &game;
    return nullptr;
}
bool within_days(const std::string &a, const std::string &b, long long days) {
    const auto ta = parse_time(a), tb = parse_time(b);
    return ta > 0 && tb > 0 && std::llabs(ta - tb) <= days * 86400;
}
void add_external_quote(json &snapshot, json quote) {
    quote["id"] = quote_id(quote);
    for (auto &old : snapshot["quotes"])
        if (old.value("id", "") == quote.value("id", "")) {
            old = quote;
            return;
        }
    snapshot["quotes"].push_back(std::move(quote));
}
json external_moneyline(const json &game, const std::string &team, const std::string &side,
                        const std::string &book, const std::string &source_name, const std::string &fetched,
                        const std::string &url) {
    return {{"gameId", game.value("id", "")},
            {"market", "moneyline"},
            {"player", ""},
            {"team", team},
            {"side", side},
            {"line", nullptr},
            {"book", book},
            {"observedAt", fetched},
            {"timestampKind", "retrieved"},
            {"source", source_name},
            {"status", "open"},
            {"settlement", "full_game_including_ot"},
            {"demo", false},
            {"sourceUrl", url}};
}
} // namespace
void parse_polymarket_events(const json &doc, json &s, const std::string &fetched) {
    if (!doc.is_array())
        return;
    for (const auto &event : doc) {
        if (!event.is_object())
            continue;
        const auto slug = event.value("slug", "");
        for (const auto &market : array_field(event.value("markets", json()))) {
            if (!market.is_object() || market.value("closed", false) || !market.value("active", true))
                continue;
            // Game winner markets only: spreads/totals/futures never map onto app markets.
            const auto type = market.value("sportsMarketType", "");
            if (!type.empty() && type != "moneyline")
                continue;
            auto outcomes = array_field(market.value("outcomes", json())),
                 prices = array_field(market.value("outcomePrices", json()));
            if (outcomes.size() != 2 || prices.size() != 2)
                continue;
            std::string abbr[2];
            double price[2];
            for (int i = 0; i < 2; ++i) {
                abbr[i] = abbr_from_name(text(outcomes[i]));
                price[i] = number(prices[i], -1);
                if (abbr[i].empty() || price[i] <= 0.001 || price[i] >= 0.999)
                    break;
            }
            if (abbr[0].empty() || abbr[1].empty() || abbr[0] == abbr[1] || price[0] <= 0.001 ||
                price[1] <= 0.001 || price[0] >= 0.999 || price[1] >= 0.999)
                continue;
            const json *game = find_game(s, abbr[0], abbr[1]);
            std::string home = abbr[0], away = abbr[1];
            if (!game) {
                game = find_game(s, abbr[1], abbr[0]);
                home = abbr[1];
                away = abbr[0];
            }
            if (!game || !within_days(game->value("startsAt", ""), market.value("endDate", ""), 2))
                continue;
            for (int i = 0; i < 2; ++i) {
                auto q = external_moneyline(*game, abbr[i], abbr[i] == home ? "home" : "away", "Polymarket",
                                            "polymarket", fetched, "https://polymarket.com/event/" + slug);
                q["oddsPercent"] = price[i] * 100;
                q["americanOdds"] = nullptr;
                add_external_quote(s, q);
            }
        }
    }
}
void parse_kalshi_markets(const json &doc, json &s, const std::string &fetched) {
    static const std::regex event_pattern(R"(^KXNBAGAME-[0-9]{2}[A-Z]{3}[0-9]{2}([A-Z]{6})$)");
    for (const auto &market : doc.value("markets", json::array())) {
        if (!market.is_object())
            continue;
        const auto status = market.value("status", "");
        if (status != "active" && status != "open")
            continue;
        std::smatch match;
        const auto event_ticker = market.value("event_ticker", "");
        if (!std::regex_match(event_ticker, match, event_pattern))
            continue;
        const auto pair = match[1].str();
        const auto away = canonical_team(pair.substr(0, 3)), home = canonical_team(pair.substr(3, 3));
        const auto ticker = market.value("ticker", "");
        const auto dash = ticker.rfind('-');
        if (dash == std::string::npos)
            continue;
        const auto winner = canonical_team(ticker.substr(dash + 1));
        if (winner != home && winner != away)
            continue;
        const json *game = find_game(s, home, away);
        if (!game || !within_days(game->value("startsAt", ""), market.value("occurrence_datetime", ""), 2))
            continue;
        // Traded probability: prefer the two-sided midpoint, else the last trade.
        const double bid = number(market.value("yes_bid_dollars", json()), -1),
                     ask = number(market.value("yes_ask_dollars", json()), -1);
        double price =
            (bid > 0 && ask > bid) ? (bid + ask) / 2 : number(market.value("last_price_dollars", json()), -1);
        if (price <= 0.01 || price >= 0.99)
            continue;
        auto q = external_moneyline(*game, winner, winner == home ? "home" : "away", "Kalshi", "kalshi",
                                    fetched, "https://kalshi.com/markets/kxnbagame");
        q["oddsPercent"] = price * 100;
        q["americanOdds"] = nullptr;
        add_external_quote(s, q);
    }
}
void parse_sharpapi_odds(const json &doc, const json &schedule, json &snapshot, const std::string &fetched) {
    if (!doc.is_object() || !doc.contains("data") || !doc["data"].is_array() ||
        !schedule.contains("games") || !schedule["games"].is_array())
        return;
    for (const auto &item : doc["data"]) {
        if (!item.is_object() || normalize_name(string_field(item, "league")) != "nba" ||
            normalize_name(string_field(item, "market_type")) != "moneyline")
            continue;
        if (item.contains("is_live") && !item["is_live"].is_null() &&
            (!item["is_live"].is_boolean() || item["is_live"].get<bool>()))
            continue;
        const auto home = sharpapi_team_abbr(string_field(item, "home_team"));
        const auto away = sharpapi_team_abbr(string_field(item, "away_team"));
        if (home.empty() || away.empty() || home == away)
            continue;
        auto start = clean_timestamp(string_field(item, "event_start_time"));
        const auto source_time = parse_time(start);
        if (!source_time)
            continue;
        const json *game = nullptr;
        long long best_difference = std::numeric_limits<long long>::max();
        for (const auto &candidate : schedule["games"]) {
            if (!candidate.is_object() || candidate.value("home", "") != home ||
                candidate.value("away", "") != away || candidate.value("status", "upcoming") != "upcoming")
                continue;
            const auto scheduled_time = parse_time(candidate.value("startsAt", ""));
            if (!scheduled_time)
                continue;
            const auto difference = std::llabs(scheduled_time - source_time);
            if (difference <= 6 * 60 * 60 && difference < best_difference) {
                game = &candidate;
                best_difference = difference;
            }
        }
        if (!game)
            continue;
        const auto sportsbook_id = string_field(item, "sportsbook");
        if (sportsbook_id.empty() || sharpapi_duplicate_exchange(sportsbook_id))
            continue; // Polymarket and Kalshi already have direct, keyless adapters.
        const double raw_american = number(item.value("odds_american", json()),
                                           std::numeric_limits<double>::quiet_NaN());
        if (!std::isfinite(raw_american) || std::floor(raw_american) != raw_american ||
            raw_american < -100000 || raw_american > 100000 ||
            (raw_american > -100 && raw_american < 100))
            continue;
        auto side = normalize_name(string_field(item, "selection_type"));
        if (side != "home" && side != "away") {
            const auto selected_team = sharpapi_team_abbr(string_field(item, "selection"));
            side = selected_team == home ? "home" : selected_team == away ? "away" : "";
        }
        if (side.empty())
            continue;
        auto observed = clean_timestamp(string_field(item, "timestamp"));
        if (!parse_time(observed))
            observed = fetched;
        const auto team = game->value(side, "");
        auto quote = external_moneyline(*game, team, side, sharpapi_book_name(sportsbook_id), "sharpapi", observed,
                                        "https://api.sharpapi.io/api/v1/odds");
        quote["americanOdds"] = static_cast<int>(raw_american);
        add_external_quote(snapshot, std::move(quote));
    }
}
namespace {
template <typename Callback> void each_provider_object(const json &value, Callback callback) {
    if (value.is_array()) {
        for (const auto &entry : value)
            if (entry.is_object())
                callback(entry, std::string());
    } else if (value.is_object()) {
        for (const auto &entry : value.items())
            if (entry.value().is_object())
                callback(entry.value(), entry.key());
    }
}
const json *provider_collection(const json &document, std::initializer_list<const char *> names) {
    if (document.is_array())
        return &document;
    if (!document.is_object())
        return nullptr;
    for (const auto *name : names) {
        const auto found = document.find(name);
        if (found == document.end())
            continue;
        if (found->is_array())
            return &*found;
        if (found->is_object()) {
            for (const auto *nested : {"matches", "fixtures", "games", "schedule", "events", "competitions",
                                       "data", "response"}) {
                const auto child = found->find(nested);
                if (child != found->end() && (child->is_array() || child->is_object()))
                    return &*child;
            }
            return &*found;
        }
    }
    const auto data = document.find("data");
    if (data != document.end() && data->is_object())
        for (const auto *name : names) {
            const auto child = data->find(name);
            if (child != data->end() && (child->is_array() || child->is_object()))
                return &*child;
        }
    return nullptr;
}
std::string provider_date_token(std::string value) {
    if (value.empty())
        return "";
    std::string date = clean_date(value);
    if (date.empty())
        for (const auto *format : {"%B %d, %Y", "%b %d %Y", "%B %d %Y"}) {
            std::tm parsed{};
            std::istringstream stream(value);
            stream >> std::get_time(&parsed, format);
            if (stream.fail())
                continue;
            char formatted[16]{};
            std::strftime(formatted, sizeof formatted, "%Y-%m-%d", &parsed);
            date = formatted;
            break;
        }
    if (date.size() < 10 || date[4] != '-' || date[7] != '-')
        return "";
    date.erase(std::remove(date.begin(), date.end(), '-'), date.end());
    return date.size() == 8 ? date : "";
}
long long provider_epoch(std::string value) {
    if (value.size() >= 6 && value.substr(value.size() - 6) == "+00:00")
        value = value.substr(0, value.size() - 6) + "Z";
    if (value.size() == 19 && value[10] == 'T')
        value += "Z"; // Some sports event feeds omit their explicit UTC suffix.
    return parse_time(clean_timestamp(std::move(value)));
}
json optional_score(const json &value) {
    const double parsed = number(value, std::numeric_limits<double>::quiet_NaN());
    if (!std::isfinite(parsed) || parsed < 0 || parsed > 1000)
        return nullptr;
    return static_cast<int>(std::lround(parsed));
}
std::string provider_team(const json &value, const std::string &side) {
    auto name = string_field(value, side.c_str());
    if (!name.empty())
        return sharpapi_team_abbr(name);
    const auto direct = value.find(side);
    if (direct != value.end() && direct->is_object()) {
        for (const auto *field : {"name", "shortName", "abbreviation", "abbr", "teamAbbreviation"}) {
            name = string_field(*direct, field);
            if (!name.empty())
                return sharpapi_team_abbr(name);
        }
    }
    name = string_field(value, (side + "_team").c_str());
    return sharpapi_team_abbr(name);
}
struct ContextGameMatch {
    json *game = nullptr;
    bool reversed = false;
    long long difference = std::numeric_limits<long long>::max();
};
ContextGameMatch match_context_game(json &snapshot, const std::string &home, const std::string &away,
                                    const std::string &date_token, long long source_time,
                                    long long max_difference_seconds, bool upcoming_only = false) {
    ContextGameMatch best;
    if (!snapshot.contains("games") || !snapshot["games"].is_array() || home.empty() || away.empty() ||
        home == away)
        return best;
    for (auto &candidate : snapshot["games"]) {
        if (!candidate.is_object())
            continue;
        const auto status = string_field(candidate, "status", "upcoming");
        if (upcoming_only && status != "upcoming" && status != "live")
            continue;
        const auto candidate_home = sharpapi_team_abbr(string_field(candidate, "home"));
        const auto candidate_away = sharpapi_team_abbr(string_field(candidate, "away"));
        const bool direct = candidate_home == home && candidate_away == away;
        const bool reversed = candidate_home == away && candidate_away == home;
        if (!direct && !reversed)
            continue;
        const auto candidate_start = parse_time(string_field(candidate, "startsAt"));
        const auto candidate_date = candidate_start ? daily_lineup_date_token(candidate_start) : "";
        const bool same_date = !date_token.empty() && candidate_date == date_token;
        long long difference = std::numeric_limits<long long>::max();
        if (source_time > 0 && candidate_start > 0) {
            difference = std::llabs(candidate_start - source_time);
            if (!same_date && difference > max_difference_seconds)
                continue;
        } else if (same_date) {
            difference = 0;
        } else {
            continue;
        }
        if (!best.game || difference < best.difference) {
            best = {&candidate, reversed, difference};
        }
    }
    return best;
}
const json *match_upcoming_moneyline_game(const json &schedule, const std::string &home, const std::string &away,
                                          long long source_time, bool &reversed) {
    if (!schedule.is_object() || !schedule.contains("games") || !schedule["games"].is_array())
        return nullptr;
    const json *best = nullptr;
    long long best_difference = std::numeric_limits<long long>::max();
    for (const auto &candidate : schedule["games"]) {
        if (!candidate.is_object() || string_field(candidate, "status", "upcoming") != "upcoming")
            continue;
        const auto candidate_home = sharpapi_team_abbr(string_field(candidate, "home"));
        const auto candidate_away = sharpapi_team_abbr(string_field(candidate, "away"));
        const bool direct = candidate_home == home && candidate_away == away;
        const bool flipped = candidate_home == away && candidate_away == home;
        if (!direct && !flipped)
            continue;
        const auto scheduled_time = parse_time(string_field(candidate, "startsAt"));
        if (!scheduled_time || source_time <= 0)
            continue;
        const auto difference = std::llabs(scheduled_time - source_time);
        if (difference <= 6 * 60 * 60 && difference < best_difference) {
            best = &candidate;
            best_difference = difference;
            reversed = flipped;
        }
    }
    return best;
}
void api2_event_teams(const json &event, std::string &home, std::string &away, std::string &home_key,
                      std::string &away_key) {
    home_key = text(event.value("homeParticipantKey", json()));
    const auto participants = event.value("participants", json::array());
    if (participants.is_array())
        for (const auto &participant : participants) {
            if (!participant.is_object())
                continue;
            auto name = string_field(participant, "name");
            if (name.empty())
                name = string_field(participant, "shortName");
            const auto abbr = sharpapi_team_abbr(name);
            if (abbr.empty())
                continue;
            const auto participant_key = text(participant.value("key", json()));
            const auto alignment = normalize_name(string_field(participant, "alignment"));
            if ((!home_key.empty() && participant_key == home_key) || alignment == "home") {
                home = abbr;
                home_key = participant_key.empty() ? home_key : participant_key;
            } else if ((!home_key.empty() && !participant_key.empty()) || alignment == "away") {
                away = abbr;
                away_key = participant_key;
            } else if (!home.empty() && home != abbr) {
                away = abbr;
                away_key = participant_key;
            }
        }
    if (home.empty())
        home = provider_team(event, "home");
    if (away.empty())
        away = provider_team(event, "away");
}
bool api2_full_game_segment(const std::string &segment) {
    const auto value = normalize_name(segment);
    return value.empty() || value == "full" || value == "fullgame" || value == "game" ||
           value == "fullmatch" || value == "match" || value == "fulltime" || value == "regulation";
}
int api2_american_odds(double payout) {
    if (!std::isfinite(payout))
        return 0;
    if (std::floor(payout) == payout && payout <= -100 && payout >= -100000)
        return static_cast<int>(payout);
    if (std::floor(payout) == payout && payout >= 100 && payout <= 50000)
        return static_cast<int>(payout);
    // OpenAPI calls this value `payout` but does not specify the odds convention.
    // Accept only ordinary decimal prices; fractional/implied-return values are
    // deliberately skipped instead of being guessed.
    if (payout > 1.0 && payout <= 20.0) {
        const double american = payout >= 2.0 ? (payout - 1.0) * 100.0 : -100.0 / (payout - 1.0);
        if (!std::isfinite(american) || american < -100000 || american > 50000)
            return 0;
        const int rounded = static_cast<int>(std::lround(american));
        return rounded <= -100 || rounded >= 100 ? rounded : 0;
    }
    return 0;
}
void set_external_context(json &game, const char *provider, json value) {
    if (!game.contains("externalContext") || !game["externalContext"].is_object())
        game["externalContext"] = json::object();
    game["externalContext"][provider] = std::move(value);
}
} // namespace
bool sportsbook_api2_quota_allows(int current, int daily_limit, int reserve, int calls_needed) {
    return current >= 0 && daily_limit > 0 && reserve >= 0 && calls_needed > 0 &&
           daily_limit - current >= reserve + calls_needed;
}
std::string sportsbook_api2_nba_competition_key(const json &document) {
    const json *competitions = provider_collection(document, {"competitions"});
    if (!competitions && document.is_object() && document.contains("data"))
        competitions = provider_collection(document["data"], {"competitions"});
    if (!competitions)
        return "";
    std::string result;
    each_provider_object(*competitions, [&](const json &competition, const std::string &) {
        if (!result.empty())
            return;
        const auto name = normalize_name(string_field(competition, "name"));
        const auto short_name = normalize_name(string_field(competition, "shortName"));
        const auto slug = normalize_name(string_field(competition, "slug"));
        const auto sport = normalize_name(string_field(competition, "sport"));
        const bool nba = name == "nba" || name == "nationalbasketballassociation" || short_name == "nba" ||
                         slug == "nba";
        if (!nba || (!sport.empty() && sport != "basketball"))
            return;
        result = text(competition.value("key", json()));
    });
    return result;
}
std::vector<std::string> sportsbook_api2_match_event_keys(const json &document, const json &schedule) {
    std::vector<std::string> result;
    const json *events = provider_collection(document, {"events"});
    if (!events)
        return result;
    std::set<std::string> seen;
    each_provider_object(*events, [&](const json &event, const std::string &) {
        const auto key = text(event.value("key", json()));
        const auto start = provider_epoch(string_field(event, "startTime"));
        std::string home, away, home_key, away_key;
        api2_event_teams(event, home, away, home_key, away_key);
        bool reversed = false;
        if (key.empty() || home.empty() || away.empty() ||
            !match_upcoming_moneyline_game(schedule, home, away, start, reversed))
            return;
        if (seen.insert(key).second)
            result.push_back(key);
    });
    if (result.size() > 100)
        result.resize(100);
    return result;
}
int parse_sportsbook_api2_odds(const json &document, const json &schedule, json &snapshot,
                               const std::string &fetched_at) {
    const json *events = provider_collection(document, {"events"});
    if (!events || !schedule.is_object() || !schedule.contains("games") || !schedule["games"].is_array())
        return 0;
    std::set<std::string> seen_quotes;
    int imported = 0;
    each_provider_object(*events, [&](const json &event, const std::string &) {
        const auto event_start = provider_epoch(string_field(event, "startTime"));
        std::string event_home, event_away, home_participant_key, away_participant_key;
        api2_event_teams(event, event_home, event_away, home_participant_key, away_participant_key);
        bool reversed = false;
        const auto *game = match_upcoming_moneyline_game(schedule, event_home, event_away, event_start, reversed);
        if (!game)
            return;
        const bool event_home_is_home = event_home == sharpapi_team_abbr(string_field(*game, "home"));
        for (const auto &market : array_field(event.value("markets", json()))) {
            if (!market.is_object())
                continue;
            const auto market_type = normalize_name(string_field(market, "type"));
            if (!(market_type.find("moneyline") != std::string::npos || market_type == "ml" ||
                  market_type == "h2h" || market_type == "matchwinner") ||
                !api2_full_game_segment(string_field(market, "segment")))
                continue;
            const auto outcomes = market.value("outcomes", json());
            auto import_outcome = [&](const std::string &source, const json &outcome) {
                if (!outcome.is_object())
                    return;
                if (outcome.contains("live") && !outcome["live"].is_null() &&
                    (!outcome["live"].is_boolean() || outcome["live"].get<bool>()))
                    return;
                const auto book_id = string_field(outcome, "source", source);
                if (book_id.empty() || sharpapi_duplicate_exchange(book_id))
                    return;
                const double raw_payout = number(outcome.value("payout", json()),
                                                 std::numeric_limits<double>::quiet_NaN());
                const int american = api2_american_odds(raw_payout);
                if (!american)
                    return;
                const auto participant = outcome.value("participant", json::object());
                auto participant_key = text(outcome.value("participantKey", json()));
                if (participant_key.empty())
                    participant_key = text(market.value("participantKey", json()));
                auto participant_name = string_field(participant, "name");
                if (participant_name.empty())
                    participant_name = string_field(participant, "shortName");
                const auto selected_team = sharpapi_team_abbr(participant_name);
                std::string side;
                if (!participant_key.empty()) {
                    if (!home_participant_key.empty() && participant_key == home_participant_key)
                        side = event_home_is_home ? "home" : "away";
                    else if (!away_participant_key.empty() && participant_key == away_participant_key)
                        side = event_home_is_home ? "away" : "home";
                }
                if (side.empty() && selected_team == sharpapi_team_abbr(string_field(*game, "home")))
                    side = "home";
                else if (side.empty() && selected_team == sharpapi_team_abbr(string_field(*game, "away")))
                    side = "away";
                else if (side.empty()) {
                    const auto outcome_type = normalize_name(string_field(outcome, "type"));
                    if (outcome_type == "home" || outcome_type == "1")
                        side = event_home_is_home ? "home" : "away";
                    else if (outcome_type == "away" || outcome_type == "2")
                        side = event_home_is_home ? "away" : "home";
                }
                if (side.empty())
                    return;
                auto observed = provider_epoch(string_field(outcome, "readAt"))
                                    ? clean_timestamp(string_field(outcome, "readAt"))
                                    : clean_timestamp(string_field(outcome, "lastFoundAt"));
                if (!parse_time(observed))
                    observed = fetched_at;
                const auto team = game->value(side, "");
                auto quote = external_moneyline(*game, team, side, sharpapi_book_name(book_id), "sportsbookapi2",
                                                observed, "https://sportsbookapi.com/documentation/");
                quote["americanOdds"] = american;
                quote["payoutFormat"] = raw_payout >= 100 || raw_payout <= -100 ? "american" : "decimal-heuristic";
                quote["id"] = quote_id(quote);
                const auto id = quote.value("id", "");
                if (id.empty() || !seen_quotes.insert(id).second)
                    return;
                const auto new_time = parse_time(quote.value("observedAt", ""));
                bool older_than_existing = false;
                for (const auto &existing : snapshot["quotes"])
                    if (existing.value("id", "") == id) {
                        const auto old_time = parse_time(existing.value("observedAt", ""));
                        older_than_existing = old_time > 0 && (new_time <= 0 || new_time < old_time);
                        break;
                    }
                // A two-hour-cadence adapter must not replace a fresher direct
                // quote for the same book/selection with an older cached price.
                if (!older_than_existing) {
                    add_external_quote(snapshot, std::move(quote));
                    ++imported;
                }
            };
            if (outcomes.is_object()) {
                for (const auto &book_outcomes : outcomes.items())
                    for (const auto &outcome : array_field(book_outcomes.value()))
                        import_outcome(book_outcomes.key(), outcome);
            } else if (outcomes.is_array()) {
                for (const auto &outcome : outcomes)
                    import_outcome(string_field(outcome, "source"), outcome);
            }
        }
    });
    return imported;
}
int parse_nba_go_games(const json &document, json &snapshot, const std::string &fetched_at) {
    const json *games = provider_collection(document, {"data", "games", "response"});
    if (!games)
        return 0;
    std::set<std::string> matched_ids;
    each_provider_object(*games, [&](const json &raw, const std::string &) {
        auto home = sharpapi_team_abbr(string_field(raw, "homeTeam"));
        auto away = sharpapi_team_abbr(string_field(raw, "visitorTeam"));
        if (home.empty() || away.empty() || home == away)
            return;
        auto date = provider_date_token(string_field(raw, "date"));
        if (date.empty())
            date = provider_date_token(string_field(raw, "startTimeET"));
        const auto start = provider_epoch(string_field(raw, "startTimeET"));
        const auto match = match_context_game(snapshot, home, away, date, start, 12 * 60 * 60);
        if (!match.game)
            return;
        auto &game = *match.game;
        const bool same_orientation = !match.reversed;
        json top_performers = json::array();
        std::vector<json> ranked;
        const auto player_stats = raw.value("playerGameBasicStats", json::array());
        if (player_stats.is_array())
            for (const auto &player : player_stats) {
                if (!player.is_object() || string_field(player, "playerName").empty())
                    continue;
                const auto points = optional_score(player.value("pts", json()));
                ranked.push_back({{"name", string_field(player, "playerName")},
                                  {"team", sharpapi_team_abbr(string_field(player, "team"))},
                                  {"points", points},
                                  {"rebounds", optional_score(player.value("trb", json()))},
                                  {"assists", optional_score(player.value("ast", json()))}});
            }
        std::sort(ranked.begin(), ranked.end(), [](const json &a, const json &b) {
            return number(a.value("points", json()), -1) > number(b.value("points", json()), -1);
        });
        for (size_t i = 0; i < std::min<size_t>(3, ranked.size()); ++i)
            top_performers.push_back(ranked[i]);
        json team_advanced = json::object();
        const auto advanced_rows = raw.value("teamGameAdvStats", json::array());
        if (advanced_rows.is_array())
            for (const auto &row : advanced_rows) {
                if (!row.is_object())
                    continue;
                const auto team = sharpapi_team_abbr(string_field(row, "team"));
                if (team != game.value("home", "") && team != game.value("away", ""))
                    continue;
                json advanced = json::object();
                for (const auto &field : std::vector<std::pair<const char *, const char *>>{
                         {"offensiveRating", "oRtg"}, {"defensiveRating", "dRtg"},
                         {"effectiveFgPercent", "efgPercent"}, {"trueShootingPercent", "tsPercent"}}) {
                    const double value = number(row.value(field.second, json()),
                                                std::numeric_limits<double>::quiet_NaN());
                    if (std::isfinite(value))
                        advanced[field.first] = value;
                }
                team_advanced[team] = std::move(advanced);
            }
        json lines = json::array();
        const auto line_scores = raw.value("lineScores", json::array());
        if (line_scores.is_array())
            for (const auto &line : line_scores) {
                if (!line.is_object())
                    continue;
                const auto team = sharpapi_team_abbr(string_field(line, "team"));
                if (team != game.value("home", "") && team != game.value("away", ""))
                    continue;
                lines.push_back({{"team", team},
                                 {"q1", optional_score(line.value("q1", json()))},
                                 {"q2", optional_score(line.value("q2", json()))},
                                 {"q3", optional_score(line.value("q3", json()))},
                                 {"q4", optional_score(line.value("q4", json()))},
                                 {"total", optional_score(line.value("total", json()))}});
            }
        const auto raw_home = optional_score(raw.value("homePts", json()));
        const auto raw_away = optional_score(raw.value("visitorPts", json()));
        set_external_context(game, "nbaGo",
                             {{"gameId", text(raw.value("gameId", json()))},
                              {"date", string_field(raw, "date")},
                              {"arena", string_field(raw, "arena")},
                              {"homeScore", same_orientation ? raw_home : raw_away},
                              {"awayScore", same_orientation ? raw_away : raw_home},
                              {"playerCount", player_stats.is_array() ? player_stats.size() : 0},
                              {"topPerformers", top_performers},
                              {"teamAdvanced", team_advanced},
                              {"lineScores", lines},
                              {"asOf", fetched_at},
                              {"contextOnly", true}});
        matched_ids.insert(game.value("id", ""));
    });
    return static_cast<int>(matched_ids.size());
}
int parse_thesportsdb_schedule(const json &document, json &snapshot, const std::string &fetched_at) {
    const json *events = provider_collection(document, {"schedule", "events"});
    if (!events)
        return 0;
    std::set<std::string> matched_ids;
    each_provider_object(*events, [&](const json &raw, const std::string &) {
        const auto sport = normalize_name(string_field(raw, "strSport"));
        const auto league = text(raw.value("idLeague", json()));
        if ((!sport.empty() && sport != "basketball") || (!league.empty() && league != "4387"))
            return;
        auto home = sharpapi_team_abbr(string_field(raw, "strHomeTeam"));
        auto away = sharpapi_team_abbr(string_field(raw, "strAwayTeam"));
        if (home.empty())
            home = provider_team(raw, "home");
        if (away.empty())
            away = provider_team(raw, "away");
        const auto raw_date = string_field(raw, "dateEvent").empty() ? string_field(raw, "strTimestamp")
                                                                      : string_field(raw, "dateEvent");
        const auto date = provider_date_token(raw_date);
        const auto start = provider_epoch(string_field(raw, "strTimestamp"));
        const auto match = match_context_game(snapshot, home, away, date, start, 12 * 60 * 60, true);
        if (!match.game)
            return;
        const bool same_orientation = !match.reversed;
        const auto home_score = optional_score(raw.value("intHomeScore", json()));
        const auto away_score = optional_score(raw.value("intAwayScore", json()));
        set_external_context(*match.game, "theSportsDB",
                             {{"eventId", text(raw.value("idEvent", json()))},
                              {"date", string_field(raw, "dateEvent")},
                              {"status", string_field(raw, "strStatus")},
                              {"venue", string_field(raw, "strVenue")},
                              {"city", string_field(raw, "strCity")},
                              {"homeScore", same_orientation ? home_score : away_score},
                              {"awayScore", same_orientation ? away_score : home_score},
                              {"asOf", fetched_at},
                              {"contextOnly", true}});
        matched_ids.insert(match.game->value("id", ""));
    });
    return static_cast<int>(matched_ids.size());
}
int parse_sportscore_fixtures(const json &document, json &snapshot, const std::string &fetched_at) {
    const auto sport = normalize_name(string_field(document, "sport"));
    if (!sport.empty() && sport != "basketball")
        return 0;
    const json *matches = provider_collection(document, {"matches", "fixtures", "data", "response"});
    if (!matches)
        return 0;
    std::set<std::string> matched_ids;
    each_provider_object(*matches, [&](const json &raw, const std::string &) {
        auto home = provider_team(raw, "home");
        auto away = provider_team(raw, "away");
        if (home.empty() || away.empty() || home == away)
            return;
        std::string raw_time = string_field(raw, "time");
        if (raw_time.empty())
            raw_time = string_field(raw, "startTime");
        if (raw_time.empty())
            raw_time = string_field(raw, "start_time");
        if (raw_time.empty())
            raw_time = string_field(raw, "kickoff");
        const auto start = provider_epoch(raw_time);
        std::string raw_date = string_field(raw, "date");
        if (raw_date.empty())
            raw_date = raw_time;
        const auto date = provider_date_token(raw_date);
        const auto match = match_context_game(snapshot, home, away, date, start, 12 * 60 * 60);
        if (!match.game)
            return;
        const bool same_orientation = !match.reversed;
        auto home_score = optional_score(raw.value("home_score", raw.value("homeScore", json())));
        auto away_score = optional_score(raw.value("away_score", raw.value("awayScore", json())));
        if (!same_orientation)
            std::swap(home_score, away_score);
        set_external_context(*match.game, "sportScore",
                             {{"eventId", string_field(raw, "slug", text(raw.value("id", json())))},
                              {"status", string_field(raw, "status")},
                              {"statusText", string_field(raw, "status_text")},
                              {"time", raw_time},
                              {"homeScore", home_score},
                              {"awayScore", away_score},
                              {"asOf", fetched_at},
                              {"contextOnly", true}});
        matched_ids.insert(match.game->value("id", ""));
    });
    return static_cast<int>(matched_ids.size());
}
std::map<std::string, json> match_api_basketball_games(const json &doc, const json &snapshot) {
    std::map<std::string, json> matched;
    if (!doc.is_object() || !doc.contains("response") || !doc["response"].is_array() ||
        !snapshot.contains("games") || !snapshot["games"].is_array())
        return matched;
    for (const auto &item : doc["response"]) {
        if (!item.is_object())
            continue;
        const auto id = text(item.value("id", json()));
        const auto teams_obj = item.value("teams", json::object());
        const auto home_obj = teams_obj.is_object() ? teams_obj.value("home", json::object()) : json::object();
        const auto away_obj = teams_obj.is_object() ? teams_obj.value("away", json::object()) : json::object();
        const auto home = abbr_from_name(string_field(home_obj, "name"));
        const auto away = abbr_from_name(string_field(away_obj, "name"));
        if (id.empty() || home.empty() || away.empty() || home == away)
            continue;
        const auto timestamp = number(item.value("timestamp", json()), 0);
        std::string starts_at = timestamp > 0 ? iso_at(static_cast<long long>(timestamp))
                                              : string_field(item, "date");
        if (starts_at.size() >= 6 && starts_at.substr(starts_at.size() - 6) == "+00:00")
            starts_at = starts_at.substr(0, starts_at.size() - 6) + "Z";
        starts_at = clean_timestamp(starts_at);
        const auto source_time = parse_time(starts_at);
        if (!source_time)
            continue;
        const json *best = nullptr;
        long long best_difference = std::numeric_limits<long long>::max();
        for (const auto &game : snapshot["games"]) {
            if (game.value("home", "") != home || game.value("away", "") != away ||
                game.value("status", "upcoming") != "upcoming")
                continue;
            const auto scheduled_time = parse_time(game.value("startsAt", ""));
            if (!scheduled_time)
                continue;
            const auto difference = std::llabs(scheduled_time - source_time);
            // The NBA schedule is the canonical event list. A tight tip-off
            // match prevents two games between the same teams from being
            // confused when they meet more than once in a short window.
            if (difference <= 6 * 60 * 60 && difference < best_difference) {
                best = &game;
                best_difference = difference;
            }
        }
        if (best)
            matched[id] = *best;
    }
    return matched;
}
void parse_api_basketball_odds(const json &doc, const std::map<std::string, json> &matched_games, json &s,
                               const std::string &fetched) {
    if (!doc.is_object() || !doc.contains("response") || !doc["response"].is_array())
        return;
    for (const auto &item : doc["response"]) {
        if (!item.is_object())
            continue;
        const auto game_obj = item.value("game", json::object());
        const auto game_id = game_obj.is_object() ? text(game_obj.value("id", json())) : "";
        const auto found_game = matched_games.find(game_id);
        if (found_game == matched_games.end())
            continue;
        const auto &game = found_game->second;
        const auto bookmakers = array_field(item.value("bookmakers", json()));
        for (const auto &bookmaker : bookmakers) {
            if (!bookmaker.is_object())
                continue;
            const auto book_id = text(bookmaker.value("id", json()));
            auto book_name = string_field(bookmaker, "name");
            if (book_name.empty())
                book_name = book_id.empty() ? "API-Basketball" : "API-Basketball #" + book_id;
            for (const auto &bet : array_field(bookmaker.value("bets", json()))) {
                if (!bet.is_object())
                    continue;
                const auto bet_id = static_cast<int>(number(bet.value("id", json()), -1));
                const auto bet_name = normalize_name(string_field(bet, "name"));
                const bool winner_market = bet_name == "homeaway" || bet_name == "moneyline" ||
                                           bet_name == "matchwinner" ||
                                           (bet_id == 2 && (bet_name.empty() || bet_name == "homeaway"));
                if (!winner_market)
                    continue;
                for (const auto &outcome : array_field(bet.value("values", json()))) {
                    if (!outcome.is_object())
                        continue;
                    const auto value = normalize_name(string_field(outcome, "value"));
                    const std::string side = value == "home" || value == "1" ? "home"
                                             : value == "away" || value == "2" ? "away"
                                                                                : "";
                    if (side.empty())
                        continue;
                    const double decimal = number(outcome.value("odd", json()), 0);
                    if (!std::isfinite(decimal) || decimal <= 1.0 || decimal > 1001.0)
                        continue;
                    const double raw_american = decimal >= 2.0 ? (decimal - 1.0) * 100.0
                                                               : -100.0 / (decimal - 1.0);
                    if (!std::isfinite(raw_american) || raw_american < -100000 || raw_american > 100000)
                        continue;
                    const int american = static_cast<int>(std::lround(raw_american));
                    const auto team = game.value(side, "");
                    auto quote = external_moneyline(game, team, side, book_name, "api_basketball", fetched,
                                                    "https://v1.basketball.api-sports.io/odds");
                    quote["americanOdds"] = american;
                    add_external_quote(s, std::move(quote));
                }
            }
        }
    }
}
void parse_pinnacle_markets(const json &matchups, const json &markets, json &s, const std::string &fetched) {
    std::map<long long, json> participants;
    std::map<long long, json> games_by_matchup;
    for (const auto &matchup : array_field(matchups)) {
        if (!matchup.is_object() || string_field(matchup, "type", "matchup") != "matchup")
            continue;
        std::string home, away;
        for (const auto &participant : array_field(matchup.value("participants", json()))) {
            const auto abbr = abbr_from_name(string_field(participant, "name"));
            if (abbr.empty())
                continue;
            const auto alignment = string_field(participant, "alignment");
            participants[static_cast<long long>(number(participant.value("id", json()), -1))] = {
                {"abbr", abbr}, {"alignment", alignment}};
            if (alignment == "home")
                home = abbr;
            else if (alignment == "away")
                away = abbr;
        }
        if (!home.empty() && !away.empty()) {
            auto starts_at = string_field(matchup, "startTime");
            if (starts_at.empty())
                starts_at = string_field(matchup, "startsAt");
            games_by_matchup[static_cast<long long>(number(matchup.value("id", json()), -1))] = {
                {"home", home}, {"away", away}, {"startsAt", starts_at}};
        }
    }
    for (const auto &market : array_field(markets)) {
        if (!market.is_object() || string_field(market, "key") != "s;0;m" ||
            string_field(market, "status", "open") == "suspended")
            continue;
        const auto found =
            games_by_matchup.find(static_cast<long long>(number(market.value("matchupId", json()), -1)));
        if (found == games_by_matchup.end())
            continue;
        const json *game = find_game(s, found->second["home"], found->second["away"]);
        if (!game || !within_days(game->value("startsAt", ""), found->second.value("startsAt", ""), 2))
            continue;
        for (const auto &price_entry : array_field(market.value("prices", json()))) {
            const auto participant = participants.find(
                static_cast<long long>(number(price_entry.value("participantId", json()), -1)));
            if (participant == participants.end())
                continue;
            const double raw = number(price_entry.value("price", json()), 0);
            if (raw == 0 || (raw > -100 && raw < 100) || std::floor(raw) != raw)
                continue;
            auto q = external_moneyline(*game, participant->second["abbr"],
                                        participant->second.value("alignment", ""), "Pinnacle", "pinnacle",
                                        fetched, "https://www.pinnacle.com/");
            if (q.value("side", "") != "home" && q.value("side", "") != "away")
                continue;
            q["americanOdds"] = static_cast<int>(raw);
            add_external_quote(s, q);
        }
    }
}
void parse_nba_cdn_odds(const json &doc, json &s, const std::string &fetched) {
    // https://cdn.nba.com/static/json/liveData/odds/odds_todaysGames.json
    // (schema documented by swar/nba_api live Odds). Games carry the official
    // NBA game id; markets named "2way" are moneylines with decimal odds.
    static const std::map<std::string, std::string> kTeamIds = {
        {"1610612737", "ATL"}, {"1610612738", "BOS"}, {"1610612739", "CLE"}, {"1610612740", "NOP"},
        {"1610612741", "CHI"}, {"1610612742", "DAL"}, {"1610612743", "DEN"}, {"1610612744", "GSW"},
        {"1610612745", "HOU"}, {"1610612746", "LAC"}, {"1610612747", "LAL"}, {"1610612748", "MIA"},
        {"1610612749", "MIL"}, {"1610612750", "MIN"}, {"1610612751", "BKN"}, {"1610612752", "NYK"},
        {"1610612753", "ORL"}, {"1610612754", "IND"}, {"1610612755", "PHI"}, {"1610612756", "PHX"},
        {"1610612757", "POR"}, {"1610612758", "SAC"}, {"1610612759", "SAS"}, {"1610612760", "OKC"},
        {"1610612761", "TOR"}, {"1610612762", "UTA"}, {"1610612763", "MEM"}, {"1610612764", "WAS"},
        {"1610612765", "DET"}, {"1610612766", "CHA"}};
    auto american_from_decimal = [](double decimal) -> int {
        if (decimal >= 2)
            return static_cast<int>(std::llround((decimal - 1) * 100));
        return static_cast<int>(std::llround(-100 / (decimal - 1)));
    };
    for (const auto &raw_game : array_field(doc.value("games", json()))) {
        if (!raw_game.is_object())
            continue;
        const auto official_id = text(raw_game.value("gameId", json("")));
        const json *game = nullptr;
        for (const auto &candidate : s["games"])
            if (!official_id.empty() && (candidate.value("nbaId", "") == official_id ||
                                         candidate.value("officialGameId", "") == official_id)) {
                game = &candidate;
                break;
            }
        if (!game) {
            const auto home_it = kTeamIds.find(text(raw_game.value("homeTeamId", json("")))),
                       away_it = kTeamIds.find(text(raw_game.value("awayTeamId", json(""))));
            if (home_it != kTeamIds.end() && away_it != kTeamIds.end())
                game = find_game(s, home_it->second, away_it->second);
        }
        if (!game)
            continue;
        int books_used = 0;
        for (const auto &market : array_field(raw_game.value("markets", json()))) {
            if (string_field(market, "name") != "2way")
                continue;
            for (const auto &book : array_field(market.value("books", json()))) {
                if (books_used >= 10)
                    break;
                const auto book_name = string_field(book, "name");
                if (book_name.empty())
                    continue;
                bool used = false;
                for (const auto &outcome : array_field(book.value("outcomes", json()))) {
                    const auto type = string_field(outcome, "type");
                    if (type != "home" && type != "away")
                        continue;
                    const double decimal = number(outcome.value("odds", json()), 0);
                    if (decimal <= 1.01 || decimal > 1000)
                        continue;
                    auto q = external_moneyline(*game, game->value(type, ""), type, book_name, "nba_cdn_odds",
                                                fetched,
                                                "https://cdn.nba.com/static/json/liveData/odds/odds_todaysGames.json");
                    q["americanOdds"] = american_from_decimal(decimal);
                    add_external_quote(s, q);
                    used = true;
                }
                if (used)
                    ++books_used;
            }
        }
    }
}
void parse_standings(const json &doc, json &s, const std::string &fetched) {
    // nba_api LeagueStandingsV3: one row per team, no abbreviation column, so
    // TeamCity + TeamName are mapped through the full-name table.
    for (const auto &row : nba_rows(doc)) {
        if (!row.contains("TeamName") || !row.contains("WINS"))
            continue;
        auto abbr = abbr_from_name(text(row.value("TeamCity", json(""))) + " " + text(row["TeamName"]));
        if (abbr.empty() || abbr.size() > 3)
            continue;
        auto &t = ensure_team(s, abbr);
        t["standings"] = {{"wins", number(row.value("WINS", json(0)))},
                          {"losses", number(row.value("LOSSES", json(0)))},
                          {"winPct", number(row.value("WinPCT", json(0)))},
                          {"record", text(row.value("Record", json("")))},
                          {"home", text(row.value("HOME", json("")))},
                          {"road", text(row.value("ROAD", json("")))},
                          {"last10", text(row.value("L10", json("")))},
                          {"streak", text(row.value("strCurrentStreak", json("")))},
                          {"conference", text(row.value("Conference", json("")))},
                          {"conferenceRank", number(row.value("PlayoffRank", json(0)))},
                          {"gamesBack", number(row.value("ConferenceGamesBack", json(0)))},
                          {"pointsPg", number(row.value("PointsPG", json(0)))},
                          {"oppPointsPg", number(row.value("OppPointsPG", json(0)))},
                          {"diffPointsPg", number(row.value("DiffPointsPG", json(0)))},
                          {"asOf", fetched}};
    }
}
void parse_player_season_stats(const json &doc, json &s, const std::string &fetched) {
    // nba_api LeagueDashPlayerStats (Base, PerGame). Enriches players already in
    // the snapshot from game logs; a season line alone never creates a player,
    // because it carries no dated game-by-game history for the model.
    for (const auto &row : nba_rows(doc)) {
        if (!row.contains("PLAYER_NAME") || !row.contains("PTS"))
            continue;
        const auto key = normalize_name(text(row["PLAYER_NAME"]));
        if (!s["players"].contains(key))
            continue;
        auto &p = s["players"][key];
        p["seasonStats"] = {{"gp", number(row.value("GP", json(0)))},
                            {"min", number(row.value("MIN", json(0)))},
                            {"pts", number(row.value("PTS", json(0)))},
                            {"reb", number(row.value("REB", json(0)))},
                            {"ast", number(row.value("AST", json(0)))},
                            {"fg3m", number(row.value("FG3M", json(0)))},
                            {"stl", number(row.value("STL", json(0)))},
                            {"blk", number(row.value("BLK", json(0)))},
                            {"fgPct", number(row.value("FG_PCT", json(0)))},
                            {"fg3Pct", number(row.value("FG3_PCT", json(0)))},
                            {"ftPct", number(row.value("FT_PCT", json(0)))},
                            {"plusMinus", number(row.value("PLUS_MINUS", json(0)))},
                            {"dd2", number(row.value("DD2", json(0)))},
                            {"td3", number(row.value("TD3", json(0)))},
                            {"asOf", fetched}};
    }
}
void parse_team_four_factors(const json &doc, json &s, const std::string &fetched) {
    // nba_api LeagueDashTeamStats with MeasureType=Four Factors.
    for (const auto &row : nba_rows(doc)) {
        if (!row.contains("EFG_PCT"))
            continue;
        auto abbr = canonical_team(text(row.value("TEAM_ABBREVIATION", json(""))));
        if (abbr.size() > 3 || abbr.empty())
            abbr = abbr_from_name(text(row.value("TEAM_NAME", json(""))));
        if (abbr.empty() || abbr.size() > 3)
            continue;
        auto &t = ensure_team(s, abbr);
        t["fourFactors"] = {{"efgPct", number(row.value("EFG_PCT", json(0)))},
                            {"ftaRate", number(row.value("FTA_RATE", json(0)))},
                            {"tovPct", number(row.value("TM_TOV_PCT", json(0)))},
                            {"orebPct", number(row.value("OREB_PCT", json(0)))},
                            {"oppEfgPct", number(row.value("OPP_EFG_PCT", json(0)))},
                            {"oppFtaRate", number(row.value("OPP_FTA_RATE", json(0)))},
                            {"oppTovPct", number(row.value("OPP_TOV_PCT", json(0)))},
                            {"oppOrebPct", number(row.value("OPP_OREB_PCT", json(0)))},
                            {"asOf", fetched}};
    }
}
namespace {
// Lower-case helper for the official lineup/transaction parsers below.
std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}
std::string titleize_slug(const std::string &slug) {
    std::string out;
    bool capital = true;
    for (char c : slug) {
        if (c == '-' || c == '_') {
            if (!out.empty() && out.back() != ' ')
                out += ' ';
            capital = true;
        } else if (capital) {
            out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            capital = false;
        } else
            out += c;
    }
    while (!out.empty() && out.back() == ' ')
        out.pop_back();
    return out;
}
std::string abbr_from_slug(const std::string &slug) {
    static const std::map<std::string, std::string> slugs = {
        {"hawks", "ATL"},          {"celtics", "BOS"},       {"nets", "BKN"},
        {"hornets", "CHA"},        {"bulls", "CHI"},         {"cavaliers", "CLE"},
        {"mavericks", "DAL"},      {"nuggets", "DEN"},       {"pistons", "DET"},
        {"warriors", "GSW"},       {"rockets", "HOU"},       {"pacers", "IND"},
        {"clippers", "LAC"},       {"lakers", "LAL"},        {"grizzlies", "MEM"},
        {"heat", "MIA"},           {"bucks", "MIL"},         {"timberwolves", "MIN"},
        {"pelicans", "NOP"},       {"knicks", "NYK"},        {"thunder", "OKC"},
        {"magic", "ORL"},          {"76ers", "PHI"},         {"suns", "PHX"},
        {"trail-blazers", "POR"},  {"trailblazers", "POR"},  {"kings", "SAC"},
        {"spurs", "SAS"},          {"raptors", "TOR"},       {"jazz", "UTA"},
        {"wizards", "WAS"}};
    auto found = slugs.find(lower_copy(slug));
    return found == slugs.end() ? "" : found->second;
}
} // namespace
void parse_official_schedule(const json &doc, json &s, const std::string &fetched) {
    // NBA.com CDN scheduleLeagueV2 (same leagueSchedule envelope the NBA Stats
    // scheduleleaguev2 endpoint returns). One request covers the full season;
    // only games already in the snapshot window are enriched, so a 1,200-game
    // file never floods the dashboard. The CDN schema contains explicit nulls
    // for optional metadata; those fields must not abort the whole source.
    if (!doc.is_object() || !doc.contains("leagueSchedule") || !doc["leagueSchedule"].is_object())
        return;
    const auto &league = doc["leagueSchedule"];
    auto broadcast_names = [](const json &list) {
        json out = json::array();
        if (!list.is_array())
            return out;
        for (const auto &entry : list) {
            const auto name = string_field(entry, "broadcasterDisplay");
            if (!name.empty())
                out.push_back(name);
        }
        return out;
    };
    int games = 0, matched = 0;
    const auto game_dates = league.value("gameDates", json::array());
    if (game_dates.is_array()) {
        for (const auto &date_group : game_dates) {
            if (!date_group.is_object())
                continue;
            const auto date_games = date_group.value("games", json::array());
            if (!date_games.is_array())
                continue;
            for (const auto &raw : date_games) {
                if (!raw.is_object())
                    continue;
                const auto home_it = raw.find("homeTeam");
                const auto away_it = raw.find("awayTeam");
                if (home_it == raw.end() || away_it == raw.end() || !home_it->is_object() ||
                    !away_it->is_object())
                    continue;
                ++games;
                const auto home = canonical_team(string_field(*home_it, "teamTricode"));
                const auto away = canonical_team(string_field(*away_it, "teamTricode"));
                const auto starts = clean_timestamp(string_field(raw, "gameDateTimeUTC"));
                const auto official_id = text(raw.value("gameId", json("")));
                if (home.empty() || away.empty() || official_id.empty() || !parse_time(starts))
                    continue;
                json *best = nullptr;
                long long best_diff = 0;
                for (auto &game : s["games"]) {
                    if (!official_id.empty() &&
                        (string_field(game, "nbaId") == official_id ||
                         string_field(game, "officialGameId") == official_id)) {
                        best = &game;
                        best_diff = 0;
                        break;
                    }
                    if (string_field(game, "home") != home || string_field(game, "away") != away)
                        continue;
                    const long long diff =
                        std::llabs(parse_time(string_field(game, "startsAt")) - parse_time(starts));
                    if (diff <= 36 * 3600 && (!best || diff < best_diff)) {
                        best = &game;
                        best_diff = diff;
                    }
                }
                if (!best)
                    continue;
                ++matched;
                auto &game = *best;
                const auto arena_name = string_field(raw, "arenaName");
                game["officialGameId"] = official_id;
                game["gameCode"] = string_field(raw, "gameCode");
                game["officialStatus"] = number(raw.value("gameStatus", json(0)));
                game["officialStatusText"] = string_field(raw, "gameStatusText");
                game["arenaName"] = arena_name;
                game["arenaCity"] = string_field(raw, "arenaCity");
                game["arenaState"] = string_field(raw, "arenaState");
                if (string_field(game, "venue").empty() && !arena_name.empty())
                    game["venue"] = arena_name;
                const auto neutral = raw.value("isNeutral", json());
                game["neutralSite"] = neutral.is_boolean() && neutral.get<bool>();
                game["postponedStatus"] = string_field(raw, "postponedStatus");
                game["seriesText"] = string_field(raw, "seriesText");
                game["gameLabel"] = string_field(raw, "gameLabel");
                game["gameSubLabel"] = string_field(raw, "gameSubLabel");
                const auto casters = raw.value("broadcasters", json::object());
                const json empty_casters = json::object();
                const auto &safe_casters = casters.is_object() ? casters : empty_casters;
                game["broadcasts"] = {
                    {"national", broadcast_names(safe_casters.value("nationalBroadcasters", json::array()))},
                    {"nationalRadio", broadcast_names(safe_casters.value("nationalRadioBroadcasters", json::array()))},
                    {"nationalOtt", broadcast_names(safe_casters.value("nationalOttBroadcasters", json::array()))},
                    {"homeTv", broadcast_names(safe_casters.value("homeTvBroadcasters", json::array()))},
                    {"homeRadio", broadcast_names(safe_casters.value("homeRadioBroadcasters", json::array()))},
                    {"awayTv", broadcast_names(safe_casters.value("awayTvBroadcasters", json::array()))},
                    {"awayRadio", broadcast_names(safe_casters.value("awayRadioBroadcasters", json::array()))}};
                json leaders = json::array();
                const auto points_leaders = raw.value("pointsLeaders", json::array());
                if (points_leaders.is_array()) {
                    for (const auto &leader : points_leaders) {
                        if (!leader.is_object())
                            continue;
                        auto name = string_field(leader, "firstName") + " " + string_field(leader, "lastName");
                        while (!name.empty() && name.front() == ' ')
                            name.erase(name.begin());
                        while (!name.empty() && name.back() == ' ')
                            name.pop_back();
                        leaders.push_back({{"name", name},
                                           {"team", string_field(leader, "teamTricode")},
                                           {"points", number(leader.value("points", json(0)))}});
                    }
                }
                game["pointsLeaders"] = leaders;
            }
        }
    }
    const auto weeks = league.value("weeks", json::array());
    s["leagueContext"]["schedule"] = {{"seasonYear", string_field(league, "seasonYear")},
                                      {"leagueId", text(league.value("leagueId", json("")))},
                                      {"games", games},
                                      {"weeks", weeks.is_array() ? weeks.size() : 0},
                                      {"matchedGames", matched},
                                      {"asOf", fetched}};
}
std::string daily_lineup_date_token(long long epoch_seconds) {
    // NBA daily-lineup files are named for the league's Eastern game date.
    // A fixed UTC-4 offset tracks that date for the whole NBA season window
    // closely enough to pick the right file; the parser then matches games
    // by teams, never by the token alone.
    auto token = iso_at(epoch_seconds - 4 * 3600).substr(0, 10);
    token.erase(std::remove(token.begin(), token.end(), '-'), token.end());
    return token;
}
void parse_daily_lineups(const json &doc, json &s, const std::string &fetched, const std::string &date_token) {
    // stats.nba.com/js/data/leaders/00_daily_lineups_YYYYMMDD.json. Only an
    // explicitly confirmed starter (position listed, roster active) may set
    // starterConfirmed; expected/projected entries stay visible context.
    if (!doc.contains("games") || !doc["games"].is_array())
        return;
    const auto target = date_token.size() == 8
                            ? parse_time(date_token.substr(0, 4) + "-" + date_token.substr(4, 2) + "-" +
                                         date_token.substr(6, 2) + "T12:00:00Z")
                            : 0;
    int players = 0, confirmed = 0, games_matched = 0;
    if (!s.contains("lineups") || !s["lineups"].is_array())
        s["lineups"] = json::array();
    for (const auto &raw_game : doc["games"]) {
        if (!raw_game.contains("homeTeam") || !raw_game.contains("awayTeam"))
            continue;
        const auto official_id = text(raw_game.value("gameId", json("")));
        const auto home = canonical_team(raw_game["homeTeam"].value("teamAbbreviation", "")),
                   away = canonical_team(raw_game["awayTeam"].value("teamAbbreviation", ""));
        json *best = nullptr;
        long long best_diff = 0;
        for (auto &game : s["games"]) {
            if (!official_id.empty() &&
                (game.value("nbaId", "") == official_id || game.value("officialGameId", "") == official_id)) {
                best = &game;
                best_diff = 0;
                break;
            }
            if (game.value("home", "") != home || game.value("away", "") != away || !target)
                continue;
            const long long diff = std::llabs(parse_time(game.value("startsAt", "")) - target);
            if (diff <= 48 * 3600 && (!best || diff < best_diff)) {
                best = &game;
                best_diff = diff;
            }
        }
        json summary = {{"homeConfirmed", 0}, {"awayConfirmed", 0}, {"homeListed", 0}, {"awayListed", 0},
                        {"asOf", fetched}};
        for (const std::string side : {"home", "away"}) {
            const auto &team_doc = raw_game[side + "Team"];
            const auto abbr = canonical_team(team_doc.value("teamAbbreviation", side == "home" ? home : away));
            for (const auto &raw_player : team_doc.value("players", json::array())) {
                ++players;
                summary[side + "Listed"] = number(summary[side + "Listed"]) + 1;
                auto name = raw_player.value("playerName", "");
                if (name.empty())
                    name = raw_player.value("firstName", "") + std::string(" ") + raw_player.value("lastName", "");
                const auto key = normalize_name(name);
                if (key.empty())
                    continue;
                const auto lineup_status = raw_player.value("lineupStatus", ""),
                           roster_status = raw_player.value("rosterStatus", ""),
                           position = raw_player.value("position", "");
                const auto status_key = lower_copy(lineup_status), roster_key = lower_copy(roster_status);
                const bool is_confirmed =
                    (status_key == "confirmed" || status_key == "starter" || status_key == "starting") &&
                    !position.empty() && roster_key != "inactive" && roster_key != "out";
                if (is_confirmed) {
                    ++confirmed;
                    summary[side + "Confirmed"] = number(summary[side + "Confirmed"]) + 1;
                }
                if (!best)
                    continue;
                const auto game_id = best->value("id", "");
                if (s["players"].contains(key)) {
                    auto &p = s["players"][key];
                    p["lineup"] = {{"gameId", game_id},
                                   {"officialGameId", official_id},
                                   {"team", abbr},
                                   {"position", position},
                                   {"lineupStatus", lineup_status},
                                   {"rosterStatus", roster_status},
                                   {"timestamp", text(raw_player.value("timestamp", json("")))},
                                   {"confirmed", is_confirmed},
                                   {"asOf", fetched}};
                    if (is_confirmed) {
                        p["starterConfirmed"] = true;
                        p["starterGameId"] = game_id;
                    } else if (p.value("starterGameId", "") == game_id)
                        p["starterConfirmed"] = false;
                }
                if (is_confirmed) {
                    bool dup = false;
                    for (const auto &entry : s["lineups"])
                        if (entry.value("gameId", "") == game_id && normalize_name(entry.value("player", "")) == key)
                            dup = true;
                    if (!dup)
                        s["lineups"].push_back({{"gameId", game_id},
                                                {"player", name},
                                                {"team", abbr},
                                                {"confirmed", true},
                                                {"source", "nba_daily_lineups"}});
                }
            }
        }
        if (best) {
            ++games_matched;
            (*best)["lineupSummary"] = summary;
        }
    }
    s["leagueContext"]["dailyLineups"] = {{"date", date_token},
                                          {"games", doc["games"].size()},
                                          {"matchedGames", games_matched},
                                          {"players", players},
                                          {"confirmedStarters", confirmed},
                                          {"asOf", fetched}};
}
void parse_player_movement(const json &doc, json &s, const std::string &fetched, const std::string &season) {
    // stats.nba.com/js/data/playermovement/NBA_Player_Movement.json: the static
    // archive behind the NBA transactions page. Rows are dated facts with no
    // effective time, so they are context only — never eligibility evidence.
    const auto root = doc.value("NBA_Player_Movement", json::object());
    const auto rows = root.value("rows", json::array());
    if (!rows.is_array())
        return;
    std::string cutoff;
    if (season.size() >= 4)
        cutoff = season.substr(0, 4) + "-07-01";
    std::vector<json> kept;
    for (const auto &row : rows) {
        if (!row.is_object())
            continue;
        const auto date = text(row.value("TRANSACTION_DATE", json(""))).substr(0, 10);
        if (!parse_time(date) || (!cutoff.empty() && date < cutoff))
            continue;
        const auto player_slug = text(row.value("PLAYER_SLUG", json(""))),
                   team_slug = text(row.value("TEAM_SLUG", json("")));
        auto player_name = titleize_slug(player_slug);
        const auto key = normalize_name(player_slug);
        if (!key.empty() && s["players"].contains(key))
            player_name = s["players"][key].value("name", player_name);
        kept.push_back({{"date", date},
                        {"type", text(row.value("Transaction_Type", row.value("TRANSACTION_TYPE", json(""))))},
                        {"description", text(row.value("TRANSACTION_DESCRIPTION", json("")))},
                        {"teamSlug", team_slug},
                        {"teamAbbr", abbr_from_slug(team_slug)},
                        {"teamId", text(row.value("TEAM_ID", json("")))},
                        {"playerSlug", player_slug},
                        {"playerId", text(row.value("PLAYER_ID", json("")))},
                        {"playerName", player_name},
                        {"asOf", fetched}});
    }
    std::sort(kept.begin(), kept.end(), [](const json &a, const json &b) {
        if (a.value("date", "") != b.value("date", ""))
            return a.value("date", "") > b.value("date", "");
        return a.value("playerSlug", "") < b.value("playerSlug", "");
    });
    if (kept.size() > 120)
        kept.resize(120);
    s["transactions"] = kept;
    std::set<std::string> attached;
    for (const auto &entry : kept) {
        const auto key = normalize_name(entry.value("playerSlug", ""));
        if (key.empty() || attached.count(key) || !s["players"].contains(key))
            continue;
        s["players"][key]["latestTransaction"] = {{"date", entry.value("date", "")},
                                                  {"type", entry.value("type", "")},
                                                  {"description", entry.value("description", "")},
                                                  {"teamSlug", entry.value("teamSlug", "")},
                                                  {"teamAbbr", entry.value("teamAbbr", "")},
                                                  {"asOf", fetched}};
        attached.insert(key);
    }
    s["leagueContext"]["transactions"] = {{"totalRows", rows.size()},
                                          {"retained", kept.size()},
                                          {"cutoff", cutoff},
                                          {"asOf", fetched}};
}
void parse_player_index(const json &doc, json &s, const std::string &fetched) {
    // nba_api PlayerIndex: league roster crosswalk with bio/draft context and
    // current per-game PTS/REB/AST. It may create a bare roster record (the
    // refresh later keeps only active-team players); it never creates logs.
    for (const auto &row : nba_rows(doc)) {
        const auto first = text(row.value("PLAYER_FIRST_NAME", json(""))),
                   last = text(row.value("PLAYER_LAST_NAME", json("")));
        const auto abbr = canonical_team(text(row.value("TEAM_ABBREVIATION", json(""))));
        if (first.empty() || last.empty() || abbr.empty() || abbr.size() > 3)
            continue;
        const auto defunct = row.value("IS_DEFUNCT", json(false));
        if (defunct == true || number(defunct, 0) == 1)
            continue;
        const auto name = first + " " + last;
        const auto key = normalize_name(name);
        if (key.empty())
            continue;
        if (!s["players"].contains(key))
            s["players"][key] = {{"name", name}, {"team", abbr}, {"injury", "Unknown"}, {"source", "nba_stats"}};
        auto &p = s["players"][key];
        if (p.value("name", "").empty())
            p["name"] = name;
        double height_inches = 0;
        const auto height = text(row.value("HEIGHT", json("")));
        const auto dash = height.find('-');
        if (dash != std::string::npos)
            height_inches = number(height.substr(0, dash), 0) * 12 + number(height.substr(dash + 1), 0);
        if (height_inches < 60 || height_inches > 96)
            height_inches = 0;
        p["playerIndex"] = {{"personId", text(row.value("PERSON_ID", json("")))},
                            {"slug", text(row.value("PLAYER_SLUG", json("")))},
                            {"teamId", text(row.value("TEAM_ID", json("")))},
                            {"teamSlug", text(row.value("TEAM_SLUG", json("")))},
                            {"teamAbbr", abbr},
                            {"jersey", text(row.value("JERSEY_NUMBER", json("")))},
                            {"position", text(row.value("POSITION", json("")))},
                            {"height", height},
                            {"heightInches", height_inches ? json(height_inches) : json(nullptr)},
                            {"weight", number(row.value("WEIGHT", json(0)))},
                            {"college", text(row.value("COLLEGE", json("")))},
                            {"country", text(row.value("COUNTRY", json("")))},
                            {"draftYear", text(row.value("DRAFT_YEAR", json("")))},
                            {"draftRound", text(row.value("DRAFT_ROUND", json("")))},
                            {"draftNumber", text(row.value("DRAFT_NUMBER", json("")))},
                            {"rosterStatus", row.value("ROSTER_STATUS", json(nullptr))},
                            {"pts", number(row.value("PTS", json(0)))},
                            {"reb", number(row.value("REB", json(0)))},
                            {"ast", number(row.value("AST", json(0)))},
                            {"statsTimeframe", text(row.value("STATS_TIMEFRAME", json("")))},
                            {"fromYear", text(row.value("FROM_YEAR", json("")))},
                            {"toYear", text(row.value("TO_YEAR", json("")))},
                            {"asOf", fetched}};
        if (!text(row.value("PERSON_ID", json(""))).empty())
            p["nbaId"] = text(row.value("PERSON_ID", json("")));
        if (p.value("position", "").empty() && !text(row.value("POSITION", json(""))).empty())
            p["position"] = text(row.value("POSITION", json("")));
    }
}
void parse_player_clutch(const json &doc, json &s, const std::string &fetched) {
    // nba_api LeagueDashPlayerClutch (Last 5 Minutes, within 5, Totals).
    for (const auto &row : nba_rows(doc)) {
        const auto key = normalize_name(text(row.value("PLAYER_NAME", json(""))));
        if (key.empty() || !s["players"].contains(key))
            continue;
        s["players"][key]["clutch"] = {{"gp", number(row.value("GP", json(0)))},
                                       {"w", number(row.value("W", json(0)))},
                                       {"l", number(row.value("L", json(0)))},
                                       {"winPct", number(row.value("W_PCT", json(0)))},
                                       {"min", number(row.value("MIN", json(0)))},
                                       {"fgPct", number(row.value("FG_PCT", json(0)))},
                                       {"fg3Pct", number(row.value("FG3_PCT", json(0)))},
                                       {"ftPct", number(row.value("FT_PCT", json(0)))},
                                       {"reb", number(row.value("REB", json(0)))},
                                       {"ast", number(row.value("AST", json(0)))},
                                       {"tov", number(row.value("TOV", json(0)))},
                                       {"stl", number(row.value("STL", json(0)))},
                                       {"blk", number(row.value("BLK", json(0)))},
                                       {"pts", number(row.value("PTS", json(0)))},
                                       {"plusMinus", number(row.value("PLUS_MINUS", json(0)))},
                                       {"fantasyPts", number(row.value("NBA_FANTASY_PTS", json(0)))},
                                       {"dd2", number(row.value("DD2", json(0)))},
                                       {"td3", number(row.value("TD3", json(0)))},
                                       {"asOf", fetched}};
    }
}
void parse_team_clutch(const json &doc, json &s, const std::string &fetched) {
    // nba_api LeagueDashTeamClutch (Last 5 Minutes, within 5, Totals).
    for (const auto &row : nba_rows(doc)) {
        auto abbr = canonical_team(text(row.value("TEAM_ABBREVIATION", json(""))));
        if (abbr.empty() || abbr.size() > 3)
            abbr = abbr_from_name(text(row.value("TEAM_NAME", json(""))));
        if (abbr.empty() || abbr.size() > 3)
            continue;
        auto &t = ensure_team(s, abbr);
        t["clutch"] = {{"gp", number(row.value("GP", json(0)))},
                       {"w", number(row.value("W", json(0)))},
                       {"l", number(row.value("L", json(0)))},
                       {"winPct", number(row.value("W_PCT", json(0)))},
                       {"min", number(row.value("MIN", json(0)))},
                       {"fgPct", number(row.value("FG_PCT", json(0)))},
                       {"fg3Pct", number(row.value("FG3_PCT", json(0)))},
                       {"ftPct", number(row.value("FT_PCT", json(0)))},
                       {"reb", number(row.value("REB", json(0)))},
                       {"ast", number(row.value("AST", json(0)))},
                       {"tov", number(row.value("TOV", json(0)))},
                       {"stl", number(row.value("STL", json(0)))},
                       {"blk", number(row.value("BLK", json(0)))},
                       {"pts", number(row.value("PTS", json(0)))},
                       {"plusMinus", number(row.value("PLUS_MINUS", json(0)))},
                       {"asOf", fetched}};
    }
}
void parse_player_hustle(const json &doc, json &s, const std::string &fetched) {
    // nba_api LeagueHustleStatsPlayer (season totals).
    for (const auto &row : nba_rows(doc)) {
        const auto key = normalize_name(text(row.value("PLAYER_NAME", json(""))));
        if (key.empty() || !s["players"].contains(key))
            continue;
        s["players"][key]["hustle"] = {{"g", number(row.value("G", json(0)))},
                                       {"min", number(row.value("MIN", json(0)))},
                                       {"contestedShots", number(row.value("CONTESTED_SHOTS", json(0)))},
                                       {"contestedShots2pt", number(row.value("CONTESTED_SHOTS_2PT", json(0)))},
                                       {"contestedShots3pt", number(row.value("CONTESTED_SHOTS_3PT", json(0)))},
                                       {"deflections", number(row.value("DEFLECTIONS", json(0)))},
                                       {"chargesDrawn", number(row.value("CHARGES_DRAWN", json(0)))},
                                       {"screenAssists", number(row.value("SCREEN_ASSISTS", json(0)))},
                                       {"screenAstPts", number(row.value("SCREEN_AST_PTS", json(0)))},
                                       {"looseBalls", number(row.value("LOOSE_BALLS_RECOVERED", json(0)))},
                                       {"offLooseBalls", number(row.value("OFF_LOOSE_BALLS_RECOVERED", json(0)))},
                                       {"defLooseBalls", number(row.value("DEF_LOOSE_BALLS_RECOVERED", json(0)))},
                                       {"boxOuts", number(row.value("BOX_OUTS", json(0)))},
                                       {"offBoxOuts", number(row.value("OFF_BOXOUTS", json(0)))},
                                       {"defBoxOuts", number(row.value("DEF_BOXOUTS", json(0)))},
                                       {"asOf", fetched}};
    }
}
void parse_team_hustle(const json &doc, json &s, const std::string &fetched) {
    // nba_api LeagueHustleStatsTeam (season totals).
    for (const auto &row : nba_rows(doc)) {
        auto abbr = canonical_team(text(row.value("TEAM_ABBREVIATION", json(""))));
        if (abbr.empty() || abbr.size() > 3)
            abbr = abbr_from_name(text(row.value("TEAM_NAME", json(""))));
        if (abbr.empty() || abbr.size() > 3)
            continue;
        auto &t = ensure_team(s, abbr);
        t["hustle"] = {{"min", number(row.value("MIN", json(0)))},
                       {"contestedShots", number(row.value("CONTESTED_SHOTS", json(0)))},
                       {"contestedShots2pt", number(row.value("CONTESTED_SHOTS_2PT", json(0)))},
                       {"contestedShots3pt", number(row.value("CONTESTED_SHOTS_3PT", json(0)))},
                       {"deflections", number(row.value("DEFLECTIONS", json(0)))},
                       {"chargesDrawn", number(row.value("CHARGES_DRAWN", json(0)))},
                       {"screenAssists", number(row.value("SCREEN_ASSISTS", json(0)))},
                       {"screenAstPts", number(row.value("SCREEN_AST_PTS", json(0)))},
                       {"looseBalls", number(row.value("LOOSE_BALLS_RECOVERED", json(0)))},
                       {"offLooseBalls", number(row.value("OFF_LOOSE_BALLS_RECOVERED", json(0)))},
                       {"defLooseBalls", number(row.value("DEF_LOOSE_BALLS_RECOVERED", json(0)))},
                       {"boxOuts", number(row.value("BOX_OUTS", json(0)))},
                       {"offBoxOuts", number(row.value("OFF_BOXOUTS", json(0)))},
                       {"defBoxOuts", number(row.value("DEF_BOXOUTS", json(0)))},
                       {"asOf", fetched}};
    }
}
void parse_player_estimated_metrics(const json &doc, json &s, const std::string &fetched) {
    // nba_api PlayerEstimatedMetrics.
    for (const auto &row : nba_rows(doc)) {
        const auto key = normalize_name(text(row.value("PLAYER_NAME", json(""))));
        if (key.empty() || !s["players"].contains(key))
            continue;
        s["players"][key]["estimatedMetrics"] = {
            {"gp", number(row.value("GP", json(0)))},
            {"w", number(row.value("W", json(0)))},
            {"l", number(row.value("L", json(0)))},
            {"winPct", number(row.value("W_PCT", json(0)))},
            {"min", number(row.value("MIN", json(0)))},
            {"eOffRating", number(row.value("E_OFF_RATING", json(0)))},
            {"eDefRating", number(row.value("E_DEF_RATING", json(0)))},
            {"eNetRating", number(row.value("E_NET_RATING", json(0)))},
            {"eAstRatio", number(row.value("E_AST_RATIO", json(0)))},
            {"eOrebPct", number(row.value("E_OREB_PCT", json(0)))},
            {"eDrebPct", number(row.value("E_DREB_PCT", json(0)))},
            {"eRebPct", number(row.value("E_REB_PCT", json(0)))},
            {"eTovPct", number(row.value("E_TOV_PCT", json(0)))},
            {"eUsgPct", number(row.value("E_USG_PCT", json(0)))},
            {"ePace", number(row.value("E_PACE", json(0)))},
            {"ranks", {{"eOffRating", number(row.value("E_OFF_RATING_RANK", json(0)))},
                       {"eDefRating", number(row.value("E_DEF_RATING_RANK", json(0)))},
                       {"eNetRating", number(row.value("E_NET_RATING_RANK", json(0)))},
                       {"eUsgPct", number(row.value("E_USG_PCT_RANK", json(0)))},
                       {"ePace", number(row.value("E_PACE_RANK", json(0)))}}},
            {"asOf", fetched}};
    }
}
void parse_team_estimated_metrics(const json &doc, json &s, const std::string &fetched) {
    // nba_api TeamEstimatedMetrics (no abbreviation column; map the full name).
    for (const auto &row : nba_rows(doc)) {
        auto abbr = abbr_from_name(text(row.value("TEAM_NAME", json(""))));
        if (abbr.empty() || abbr.size() > 3)
            continue;
        auto &t = ensure_team(s, abbr);
        t["estimatedMetrics"] = {{"gp", number(row.value("GP", json(0)))},
                                 {"w", number(row.value("W", json(0)))},
                                 {"l", number(row.value("L", json(0)))},
                                 {"winPct", number(row.value("W_PCT", json(0)))},
                                 {"min", number(row.value("MIN", json(0)))},
                                 {"eOffRating", number(row.value("E_OFF_RATING", json(0)))},
                                 {"eDefRating", number(row.value("E_DEF_RATING", json(0)))},
                                 {"eNetRating", number(row.value("E_NET_RATING", json(0)))},
                                 {"ePace", number(row.value("E_PACE", json(0)))},
                                 {"eAstRatio", number(row.value("E_AST_RATIO", json(0)))},
                                 {"eOrebPct", number(row.value("E_OREB_PCT", json(0)))},
                                 {"eDrebPct", number(row.value("E_DREB_PCT", json(0)))},
                                 {"eRebPct", number(row.value("E_REB_PCT", json(0)))},
                                 {"eTovPct", number(row.value("E_TM_TOV_PCT", json(0)))},
                                 {"ranks", {{"eOffRating", number(row.value("E_OFF_RATING_RANK", json(0)))},
                                            {"eDefRating", number(row.value("E_DEF_RATING_RANK", json(0)))},
                                            {"eNetRating", number(row.value("E_NET_RATING_RANK", json(0)))},
                                            {"ePace", number(row.value("E_PACE_RANK", json(0)))}}},
                                 {"asOf", fetched}};
    }
}
void parse_player_tracking_shots(const json &doc, json &s, const std::string &fetched) {
    // nba_api LeagueDashPlayerPtShot (overall tracking shooting profile).
    for (const auto &row : nba_rows(doc)) {
        const auto key = normalize_name(text(row.value("PLAYER_NAME", json(""))));
        if (key.empty() || !s["players"].contains(key))
            continue;
        s["players"][key]["trackingShooting"] = {
            {"gp", number(row.value("GP", json(0)))},
            {"g", number(row.value("G", json(0)))},
            {"lastTeam", canonical_team(text(row.value("PLAYER_LAST_TEAM_ABBREVIATION", json(""))))},
            {"fgaFrequency", number(row.value("FGA_FREQUENCY", json(0)))},
            {"fgm", number(row.value("FGM", json(0)))},
            {"fga", number(row.value("FGA", json(0)))},
            {"fgPct", number(row.value("FG_PCT", json(0)))},
            {"efgPct", number(row.value("EFG_PCT", json(0)))},
            {"fg2aFrequency", number(row.value("FG2A_FREQUENCY", json(0)))},
            {"fg2m", number(row.value("FG2M", json(0)))},
            {"fg2a", number(row.value("FG2A", json(0)))},
            {"fg2Pct", number(row.value("FG2_PCT", json(0)))},
            {"fg3aFrequency", number(row.value("FG3A_FREQUENCY", json(0)))},
            {"fg3m", number(row.value("FG3M", json(0)))},
            {"fg3a", number(row.value("FG3A", json(0)))},
            {"fg3Pct", number(row.value("FG3_PCT", json(0)))},
            {"asOf", fetched}};
    }
}
void parse_player_tracking_defense(const json &doc, json &s, const std::string &fetched) {
    // nba_api LeagueDashPtDefend (DefenseCategory=Overall).
    for (const auto &row : nba_rows(doc)) {
        const auto key = normalize_name(text(row.value("PLAYER_NAME", json(""))));
        if (key.empty() || !s["players"].contains(key))
            continue;
        s["players"][key]["trackingDefense"] = {
            {"gp", number(row.value("GP", json(0)))},
            {"g", number(row.value("G", json(0)))},
            {"lastTeam", canonical_team(text(row.value("PLAYER_LAST_TEAM_ABBREVIATION", json(""))))},
            {"position", text(row.value("PLAYER_POSITION", json("")))},
            {"freq", number(row.value("FREQ", json(0)))},
            {"dFgm", number(row.value("D_FGM", json(0)))},
            {"dFga", number(row.value("D_FGA", json(0)))},
            {"dFgPct", number(row.value("D_FG_PCT", json(0)))},
            {"normalFgPct", number(row.value("NORMAL_FG_PCT", json(0)))},
            {"pctPlusMinus", number(row.value("PCT_PLUSMINUS", json(0)))},
            {"asOf", fetched}};
    }
}
void parse_team_tracking_defense(const json &doc, json &s, const std::string &fetched) {
    // nba_api LeagueDashPtTeamDefend (DefenseCategory=Overall).
    for (const auto &row : nba_rows(doc)) {
        auto abbr = canonical_team(text(row.value("TEAM_ABBREVIATION", json(""))));
        if (abbr.empty() || abbr.size() > 3)
            abbr = abbr_from_name(text(row.value("TEAM_NAME", json(""))));
        if (abbr.empty() || abbr.size() > 3)
            continue;
        auto &t = ensure_team(s, abbr);
        t["trackingDefense"] = {{"gp", number(row.value("GP", json(0)))},
                                {"g", number(row.value("G", json(0)))},
                                {"freq", number(row.value("FREQ", json(0)))},
                                {"dFgm", number(row.value("D_FGM", json(0)))},
                                {"dFga", number(row.value("D_FGA", json(0)))},
                                {"dFgPct", number(row.value("D_FG_PCT", json(0)))},
                                {"normalFgPct", number(row.value("NORMAL_FG_PCT", json(0)))},
                                {"pctPlusMinus", number(row.value("PCT_PLUSMINUS", json(0)))},
                                {"asOf", fetched}};
    }
}
void parse_team_opponent_shots(const json &doc, json &s, const std::string &fetched) {
    // nba_api LeagueDashOppPtShot: the shot profile opponents post vs a team.
    for (const auto &row : nba_rows(doc)) {
        auto abbr = canonical_team(text(row.value("TEAM_ABBREVIATION", json(""))));
        if (abbr.empty() || abbr.size() > 3)
            abbr = abbr_from_name(text(row.value("TEAM_NAME", json(""))));
        if (abbr.empty() || abbr.size() > 3)
            continue;
        auto &t = ensure_team(s, abbr);
        t["opponentShotProfile"] = {{"gp", number(row.value("GP", json(0)))},
                                    {"g", number(row.value("G", json(0)))},
                                    {"fgaFrequency", number(row.value("FGA_FREQUENCY", json(0)))},
                                    {"fgm", number(row.value("FGM", json(0)))},
                                    {"fga", number(row.value("FGA", json(0)))},
                                    {"fgPct", number(row.value("FG_PCT", json(0)))},
                                    {"efgPct", number(row.value("EFG_PCT", json(0)))},
                                    {"fg2Pct", number(row.value("FG2_PCT", json(0)))},
                                    {"fg3Pct", number(row.value("FG3_PCT", json(0)))},
                                    {"asOf", fetched}};
    }
}
void parse_team_top_lineups(const json &doc, json &s, const std::string &fetched) {
    // nba_api LeagueDashLineups (GroupQuantity=5): keep each team's most-used
    // five-man unit as context; the full lineup table stays at the source.
    std::map<std::string, json> best;
    std::map<std::string, double> best_minutes;
    for (const auto &row : nba_rows(doc)) {
        auto abbr = canonical_team(text(row.value("TEAM_ABBREVIATION", json(""))));
        if (abbr.empty() || abbr.size() > 3)
            abbr = abbr_from_name(text(row.value("TEAM_NAME", json(""))));
        if (abbr.empty() || abbr.size() > 3)
            continue;
        const double minutes = number(row.value("MIN", json(0)));
        if (best.count(abbr) && minutes <= best_minutes[abbr])
            continue;
        const double gp = number(row.value("GP", json(0)));
        best[abbr] = {{"groupName", text(row.value("GROUP_NAME", json("")))},
                      {"groupId", text(row.value("GROUP_ID", json("")))},
                      {"gp", gp},
                      {"w", number(row.value("W", json(0)))},
                      {"l", number(row.value("L", json(0)))},
                      {"winPct", number(row.value("W_PCT", json(0)))},
                      {"min", minutes},
                      {"minutesPerGame", gp > 0 ? minutes / gp : 0},
                      {"fgPct", number(row.value("FG_PCT", json(0)))},
                      {"fg3Pct", number(row.value("FG3_PCT", json(0)))},
                      {"ftPct", number(row.value("FT_PCT", json(0)))},
                      {"reb", number(row.value("REB", json(0)))},
                      {"ast", number(row.value("AST", json(0)))},
                      {"tov", number(row.value("TOV", json(0)))},
                      {"stl", number(row.value("STL", json(0)))},
                      {"blk", number(row.value("BLK", json(0)))},
                      {"pts", number(row.value("PTS", json(0)))},
                      {"plusMinus", number(row.value("PLUS_MINUS", json(0)))},
                      {"asOf", fetched}};
        best_minutes[abbr] = minutes;
    }
    for (auto &item : best) {
        auto &t = ensure_team(s, item.first);
        t["topLineup"] = item.second;
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
                if (doc.is_object() &&
                    (doc.contains("quotes") || doc.contains("players") || doc.contains("teams") ||
                     doc.contains("games") || doc.contains("lineups")))
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
FeedResponse PublicFeeds::get(
    const std::string &url, int ttl, int max_stale, int failed_retry_seconds,
    const std::function<bool(const std::string &, std::string &)> &may_request) {
    FeedResponse response;
    if (cancelled_ && cancelled_->load()) {
        response.error = "Refresh cancelled";
        return response;
    }
    const auto path = cache_dir_ / (stable_id(url) + ".json");
    const auto retry_path = cache_dir_ / (stable_id(url) + ".retry.json");
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
    const bool matching_cache = cache.is_object() && cache.value("url", "") == url && age >= 0;
    if (matching_cache && age < ttl) {
        response.data = cache.value("body", json());
        response.fetched_at = cache.value("fetchedAt", "");
        response.cached = true;
        return response;
    }
    auto use_stale_cache = [&] {
        if (max_stale > 0 && matching_cache && age < max_stale) {
            response.data = cache.value("body", json());
            response.fetched_at = cache.value("fetchedAt", "");
            response.cached = true;
            response.stale = true;
        }
    };
    if (failed_retry_seconds > 0) {
        const auto failed = read_small_json(retry_path);
        const auto failed_at = failed.value("attemptedAt", "");
        const auto failed_age = now_seconds() - parse_time(failed_at);
        if (failed.value("url", "") == url && failed_age >= 0 && failed_age < failed_retry_seconds) {
            response.status = failed.value("status", 0);
            response.fetched_at = failed_at;
            response.error = failed.value("error", "Previous request failed; retry is deferred.");
            response.cached = true;
            use_stale_cache();
            return response;
        }
    }
    std::string permission_error;
    if (requests_ >= MAX_REQUESTS_PER_REFRESH) {
        response.error = "Per-refresh public-source request budget reached";
    } else if (may_request && !may_request(url, permission_error)) {
        response.error = permission_error.empty() ? "Provider request was deferred by its daily quota guard."
                                                  : permission_error;
    } else if (requests_ >= MAX_REQUESTS_PER_REFRESH) {
        response.error = "Per-refresh public-source request budget reached";
    } else {
        if (requests_ > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        ++requests_;
        if (api_basketball_host(url)) {
            ++api_basketball_current_;
            api_basketball_quota_checked_at_ = now_iso();
            save_api_basketball_quota();
        }
        if (sportsbook_api2_host(url)) {
            if (sportsbook_api2_quota_date_ != now_iso().substr(0, 10)) {
                sportsbook_api2_quota_date_ = now_iso().substr(0, 10);
                sportsbook_api2_current_ = 0;
            }
            ++sportsbook_api2_current_;
            save_sportsbook_api2_quota();
        }
        // Record an attempt before I/O. A crash or transport timeout must not
        // turn the one-daily odds poll into a tight retry loop on next startup.
        if (failed_retry_seconds > 0)
            write_json_file(retry_path, {{"url", url},
                                         {"attemptedAt", now_iso()},
                                         {"status", 0},
                                         {"error", "Request outcome is unknown; retry is deferred to the "
                                                    "provider refresh interval."}});
        auto http = public_get(url);
        response.status = http.status;
        response.error = http.error;
        if (http.status == 200 && http.error.empty()) {
            try {
                response.data = json::parse(http.body);
                response.fetched_at = now_iso();
                json entry = {{"url", url}, {"fetchedAt", response.fetched_at}, {"body", response.data}};
                std::ofstream stream(path, std::ios::binary | std::ios::trunc);
                stream << entry.dump();
                if (api_basketball_host(url))
                    record_api_basketball_response(response.data);
                if (failed_retry_seconds > 0) {
                    std::error_code error;
                    std::filesystem::remove(retry_path, error);
                }
                return response;
            } catch (...) {
                response.error = "Public source returned an unsupported/non-JSON response";
            }
        }
        if (failed_retry_seconds > 0)
            write_json_file(retry_path, {{"url", url},
                                         {"attemptedAt", now_iso()},
                                         {"status", response.status},
                                         {"error", response.error.empty() ? "Provider request failed." : response.error}});
    }
    use_stale_cache();
    return response;
}
bool PublicFeeds::ensure_api_basketball_quota(int calls_needed, std::string &reason) {
    if (api_basketball_key().empty()) {
        reason = "Set API_BASKETBALL_KEY in the application process environment to enable API-Basketball. "
                 "The credential is never stored in the repository or cache.";
        return false;
    }
    const auto today = now_iso().substr(0, 10);
    if (!api_basketball_quota_checked_) {
        api_basketball_quota_checked_ = true;
        const auto usage_path = cache_dir_ / "api-basketball-usage.json";
        const auto saved = read_small_json(usage_path);
        const auto checked_at = saved.value("checkedAt", "");
        if (saved.value("date", "") == today && parse_time(checked_at) > 0) {
            api_basketball_quota_date_ = today;
            api_basketball_quota_checked_at_ = checked_at;
            api_basketball_current_ = static_cast<int>(number(saved.value("current", json(-1)), -1));
            api_basketball_limit_ = static_cast<int>(number(saved.value("limitDay", json(-1)), -1));
            api_basketball_active_ =
                saved.contains("active") && saved["active"].is_boolean() && saved["active"].get<bool>();
            api_basketball_quota_error_ = string_field(saved, "error");
            const bool saved_ok = saved.contains("ok") && saved["ok"].is_boolean() && saved["ok"].get<bool>();
            if (!saved_ok) {
                reason = api_basketball_quota_error_.empty()
                             ? "API-Sports quota status could not be verified; the paid request was skipped."
                             : api_basketball_quota_error_;
                return false;
            }
        } else {
            if (cancelled_ && cancelled_->load()) {
                reason = "Refresh cancelled";
                return false;
            }
            if (requests_ >= MAX_REQUESTS_PER_REFRESH) {
                reason = "Per-refresh public-source request budget reached before API-Sports quota check.";
                return false;
            }
            if (requests_ > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            ++requests_;
            const auto status = public_get("https://v1.basketball.api-sports.io/status");
            api_basketball_quota_date_ = today;
            api_basketball_quota_checked_at_ = now_iso();
            api_basketball_current_ = -1;
            api_basketball_limit_ = -1;
            api_basketball_active_ = false;
            api_basketball_quota_error_.clear();
            if (status.status == 200 && status.error.empty()) {
                try {
                    const auto document = json::parse(status.body);
                    const auto account = document.value("response", json::object());
                    const auto requests = account.is_object() ? account.value("requests", json::object())
                                                               : json::object();
                    const auto subscription = account.is_object()
                                                  ? account.value("subscription", json::object())
                                                  : json::object();
                    api_basketball_current_ = requests.is_object()
                                                  ? static_cast<int>(number(requests.value("current", json(-1)), -1))
                                                  : -1;
                    api_basketball_limit_ = requests.is_object()
                                                ? static_cast<int>(number(requests.value("limit_day", json(-1)), -1))
                                                : -1;
                    api_basketball_active_ = subscription.is_object() && subscription.contains("active") &&
                                             subscription["active"].is_boolean() &&
                                             subscription["active"].get<bool>();
                    if (api_basketball_current_ < 0 || api_basketball_limit_ <= 0)
                        api_basketball_quota_error_ =
                            "API-Sports status response did not include daily request counters; odds were "
                            "skipped to protect the key's quota.";
                    else if (!api_basketball_active_)
                        api_basketball_quota_error_ =
                            "API-Sports subscription is not active; odds were not requested.";
                } catch (...) {
                    api_basketball_quota_error_ =
                        "API-Sports status response was not valid JSON; odds were skipped to protect the "
                        "key's quota.";
                }
            } else {
                api_basketball_quota_error_ =
                    "API-Sports quota status could not be checked (" +
                    (status.error.empty() ? "HTTP " + std::to_string(status.status) : status.error) +
                    "); the daily odds request was skipped.";
            }
            save_api_basketball_quota();
            if (api_basketball_current_ < 0 || api_basketball_limit_ <= 0 || !api_basketball_active_) {
                reason = api_basketball_quota_error_;
                return false;
            }
        }
    }
    if (!api_basketball_active_) {
        reason = api_basketball_quota_error_.empty() ? "API-Sports subscription is not active." :
                                                       api_basketball_quota_error_;
        return false;
    }
    if (!api_basketball_quota_allows(api_basketball_current_, api_basketball_limit_,
                                     API_BASKETBALL_RESERVE_REQUESTS, calls_needed)) {
        const auto remaining = std::max(0, api_basketball_limit_ - api_basketball_current_);
        reason = "API-Sports daily quota guard: " + std::to_string(remaining) + " of " +
                 std::to_string(api_basketball_limit_) + " requests remain; the app needs " +
                 std::to_string(calls_needed) + " and reserves " +
                 std::to_string(API_BASKETBALL_RESERVE_REQUESTS) + " for other use, so no request was made.";
        return false;
    }
    return true;
}
void PublicFeeds::save_api_basketball_quota() {
    write_json_file(cache_dir_ / "api-basketball-usage.json",
                    {{"date", api_basketball_quota_date_},
                     {"checkedAt", api_basketball_quota_checked_at_},
                     {"current", api_basketball_current_},
                     {"limitDay", api_basketball_limit_},
                     {"active", api_basketball_active_},
                     {"ok", api_basketball_limit_ > 0 && api_basketball_active_ &&
                                api_basketball_current_ >= 0},
                     {"error", api_basketball_quota_error_}});
}
bool PublicFeeds::ensure_sportsbook_api2_quota(int calls_needed, std::string &reason) {
    if (rapidapi_key().empty()) {
        reason = "Set RAPIDAPI_KEY in the application process environment to enable Sportsbook API2. "
                 "The credential is never stored in the repository or cache.";
        return false;
    }
    const auto today = now_iso().substr(0, 10);
    if (!sportsbook_api2_quota_loaded_ || sportsbook_api2_quota_date_ != today) {
        const auto saved = read_small_json(cache_dir_ / "sportsbook-api2-usage.json");
        sportsbook_api2_quota_date_ = today;
        sportsbook_api2_current_ = saved.value("date", "") == today
                                       ? static_cast<int>(number(saved.value("current", json(0)), 0))
                                       : 0;
        if (sportsbook_api2_current_ < 0)
            sportsbook_api2_current_ = 0;
        sportsbook_api2_quota_loaded_ = true;
    }
    if (!sportsbook_api2_quota_allows(sportsbook_api2_current_, SPORTSBOOK_API2_DAILY_LIMIT,
                                      SPORTSBOOK_API2_RESERVE_REQUESTS, calls_needed)) {
        const auto remaining = std::max(0, SPORTSBOOK_API2_DAILY_LIMIT - sportsbook_api2_current_);
        reason = "Sportsbook API2 daily quota guard: " + std::to_string(remaining) + " of " +
                 std::to_string(SPORTSBOOK_API2_DAILY_LIMIT) + " requests remain; Edgecourt needs " +
                 std::to_string(calls_needed) + " and leaves " +
                 std::to_string(SPORTSBOOK_API2_RESERVE_REQUESTS) +
                 " for other RapidAPI use. The lower published 50/day limit is being honored.";
        return false;
    }
    return true;
}
void PublicFeeds::save_sportsbook_api2_quota() {
    write_json_file(cache_dir_ / "sportsbook-api2-usage.json",
                    {{"date", sportsbook_api2_quota_date_},
                     {"current", sportsbook_api2_current_},
                     {"dailyLimit", SPORTSBOOK_API2_DAILY_LIMIT},
                     {"reserved", SPORTSBOOK_API2_RESERVE_REQUESTS},
                     {"updatedAt", now_iso()}});
}
void PublicFeeds::record_api_basketball_response(const json &document) {
    if (!document.is_object() || !document.contains("requests") || !document["requests"].is_object())
        return;
    const auto &requests = document["requests"];
    const auto current = static_cast<int>(number(requests.value("current", json(-1)), -1));
    const auto limit = static_cast<int>(number(requests.value("limit_day", json(-1)), -1));
    if (current >= 0)
        api_basketball_current_ = std::max(api_basketball_current_, current);
    if (limit > 0)
        api_basketball_limit_ = limit;
    api_basketball_quota_date_ = now_iso().substr(0, 10);
    api_basketball_quota_checked_at_ = now_iso();
    save_api_basketball_quota();
}
json PublicFeeds::refresh(const json &previous, const std::atomic<bool> *cancelled) {
    cancelled_ = cancelled;
    requests_ = 0;
    api_basketball_quota_checked_ = false;
    api_basketball_active_ = false;
    api_basketball_current_ = 0;
    api_basketball_limit_ = 0;
    api_basketball_quota_date_.clear();
    api_basketball_quota_checked_at_.clear();
    api_basketball_quota_error_.clear();
    // One malformed feed must never abort the whole refresh (a parser reading
    // an unexpected null used to escape as a generic "Refresh worker" error
    // and discard every other source). Parse exceptions are contained per
    // source and reported on that source's card at the end of the refresh.
    std::map<std::string, std::string> parse_errors;
    auto parse_guard = [&parse_errors](const std::string &source_id, auto &&fn) {
        try {
            fn();
        } catch (const std::exception &e) {
            parse_errors[source_id] = e.what();
        }
    };
    json s = empty_snapshot("live");
    s["players"] = previous.value("players", json::object());
    s["teams"] = previous.value("teams", json::object());
    // The date window uses UTC dates so no local timezone assumption enters scheduled starts.
    // ESPN's site API rejects hyphenated date ranges with HTTP 400 ("Failed to get
    // events endpoint"), so the window is fetched one day at a time and merged by
    // event id (upsert_game deduplicates). Preseason, regular-season and postseason
    // events are all kept exactly as ESPN types them for each day.
    const auto day_urls = espn_scoreboard_day_urls(now_seconds() - 86400, now_seconds() + 7 * 86400);
    FeedResponse espn;
    int espn_days_ok = 0, espn_days_failed = 0;
    bool espn_contract_ok = false, espn_denied = false, espn_cached = false;
    for (const auto &url : day_urls) {
        if (cancelled_ && cancelled_->load())
            break;
        auto day = get(url, 45, 0);
        if (day.data.is_null()) {
            ++espn_days_failed;
            if (espn.error.empty())
                espn.error = day.error;
            if (!espn.status)
                espn.status = day.status;
            if (day.status == 403 || day.status == 429)
                espn_denied = true;
            continue;
        }
        ++espn_days_ok;
        espn_cached = espn_cached || day.cached;
        if (day.data.contains("events") && day.data["events"].is_array())
            espn_contract_ok = true;
        if (espn.fetched_at.empty() || day.fetched_at > espn.fetched_at)
            espn.fetched_at = day.fetched_at;
        parse_guard("espn", [&] { parse_espn_scoreboard(day.data, s, day.fetched_at); });
    }
    if (espn_days_ok) {
        espn.data = json::object(); // At least one day answered; details go in the source card.
        espn.cached = espn_cached;
    }
    {
        json espn_source = source("espn", "ESPN schedule + public moneylines", espn,
                                  static_cast<int>(s["games"].size()),
                                  "Public, undocumented endpoint, fetched one day at a time because "
                                  "date ranges now return HTTP 400. Moneylines only when a response "
                                  "explicitly includes them; no inferred player prices.");
        if (espn_days_ok && espn_days_failed) {
            espn_source["state"] = "partial";
            espn_source["message"] = std::string(espn_source.value("message", "")) + " " +
                                     std::to_string(espn_days_ok) + " of " +
                                     std::to_string(day_urls.size()) +
                                     " days returned; failed days are missing, not backfilled (" +
                                     espn.error + ").";
        } else if (espn_days_ok) {
            espn_source["message"] = std::string(espn_source.value("message", "")) + " " +
                                     std::to_string(espn_days_ok) + " daily scoreboards merged, " +
                                     "including preseason and upcoming games as published.";
        }
        s["sources"].push_back(espn_source);
    }
    // A fixed public-page scraper, not a challenge bypass: only use it when no daily
    // JSON response followed the known contract, never after 403/429 access denial.
    if (!espn_contract_ok && !espn_denied && requests_ < MAX_REQUESTS_PER_REFRESH &&
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
        parse_guard("nba-live", [&] { parse_nba_scoreboard(nba.data, s, nba.fetched_at); });
    s["sources"].push_back(
        source("nba-live", "NBA live scoreboard", nba, static_cast<int>(s["games"].size()),
               "Scores, period and game clock. NBA CDN is keyless; availability is not guaranteed."));
    // The league's own odds file: named sportsbook moneylines (Sportradar
    // sourced) for today's games. Public league data — no sportsbook page is
    // scraped. Converted to American prices; spread markets are ignored.
    auto nba_odds = get("https://cdn.nba.com/static/json/liveData/odds/odds_todaysGames.json", 120, 0);
    int nba_odds_count = 0;
    if (!nba_odds.data.is_null()) {
        parse_guard("nba-odds", [&] {
            const auto before = s["quotes"].size();
            parse_nba_cdn_odds(nba_odds.data, s, nba_odds.fetched_at);
            nba_odds_count = static_cast<int>(s["quotes"].size() - before);
        });
    }
    s["sources"].push_back(source(
        "nba-odds", "NBA CDN sportsbook moneylines", nba_odds, nba_odds_count,
        "Moneylines from named sportsbooks published in the NBA's own public odds file "
        "(odds_todaysGames.json, Sportradar-sourced, decimal prices converted to American). "
        "Indicative prices; the file carries no book timestamps, so retrieval time is used."));
    // Keyless traded/sharp moneyline prices run before stats/roster enrichment so
    // the shared request budget cannot starve them. Only quotes matching an already
    // scheduled game (preseason included) are kept; these venues publish
    // probabilities/American prices, never player statistics, and their prices are
    // indicative exchange/book prices.
    auto polymarket = get(
        "https://gamma-api.polymarket.com/events?tag_slug=nba&active=true&closed=false&limit=100", 60, 300);
    int polymarket_count = 0;
    if (!polymarket.data.is_null()) {
        parse_guard("polymarket", [&] {
            const auto before = s["quotes"].size();
            parse_polymarket_events(polymarket.data, s, polymarket.fetched_at);
            polymarket_count = static_cast<int>(s["quotes"].size() - before);
        });
    }
    s["sources"].push_back(
        source("polymarket", "Polymarket NBA game prices", polymarket, polymarket_count,
               "Keyless public prediction-market moneylines (prices are traded "
               "probabilities). Futures/spreads/totals are ignored; not a sportsbook price."));
    auto kalshi = get(
        "https://external-api.kalshi.com/trade-api/v2/markets?series_ticker=KXNBAGAME&status=open&limit=200",
        60, 300);
    int kalshi_count = 0;
    if (!kalshi.data.is_null()) {
        parse_guard("kalshi", [&] {
            const auto before = s["quotes"].size();
            parse_kalshi_markets(kalshi.data, s, kalshi.fetched_at);
            kalshi_count = static_cast<int>(s["quotes"].size() - before);
        });
    }
    s["sources"].push_back(source("kalshi", "Kalshi NBA game prices", kalshi, kalshi_count,
                                  "Keyless public exchange market data; midpoint of bid/ask when "
                                  "two-sided, else last trade. Fees/settlement differ from sportsbooks."));
    if (sharpapi_key().empty()) {
        s["sources"].push_back({{"id", "sharpapi"},
                                {"label", "SharpAPI NBA sportsbook moneylines"},
                                {"state", "idle"},
                                {"updatedAt", ""},
                                {"count", 0},
                                {"message", "Optional keyed odds feed. Set SHARPAPI_KEY in the "
                                            "application process environment; keys are never stored in "
                                            "source code or cache."}});
    } else if (std::none_of(s["games"].begin(), s["games"].end(), [](const json &game) {
                   return game.is_object() && game.value("status", "") == "upcoming";
               })) {
        s["sources"].push_back({{"id", "sharpapi"},
                                {"label", "SharpAPI NBA sportsbook moneylines"},
                                {"state", "idle"},
                                {"updatedAt", ""},
                                {"count", 0},
                                {"message", "No upcoming NBA games are available to match. The odds "
                                            "request is deferred to conserve the plan's request allowance."}});
    } else {
        constexpr const char *sharpapi_base =
            "https://api.sharpapi.io/api/v1/odds?league=nba&market=moneyline&is_live=false&limit=200";
        FeedResponse sharpapi;
        json sharpapi_rows = json::array();
        std::string cursor;
        bool sharpapi_partial = false, sharpapi_has_page = false, sharpapi_all_cached = true;
        int sharpapi_pages = 0;
        for (int page = 0; page < SHARPAPI_MAX_PAGES; ++page) {
            std::string url = sharpapi_base;
            if (!cursor.empty())
                url += "&cursor=" + encode_query_component(cursor);
            auto response = get(url, SHARPAPI_CACHE_SECONDS, SHARPAPI_MAX_STALE_SECONDS,
                                SHARPAPI_FAILURE_RETRY_SECONDS);
            if (response.data.is_null()) {
                sharpapi.status = response.status;
                sharpapi.error = response.error;
                sharpapi.fetched_at = response.fetched_at;
                sharpapi_partial = sharpapi_has_page;
                break;
            }
            ++sharpapi_pages;
            sharpapi_has_page = true;
            sharpapi_all_cached = sharpapi_all_cached && response.cached;
            sharpapi.stale = sharpapi.stale || response.stale;
            sharpapi.status = response.status;
            if (response.fetched_at > sharpapi.fetched_at)
                sharpapi.fetched_at = response.fetched_at;
            if (!response.data.is_object() || !response.data.contains("data") ||
                !response.data["data"].is_array()) {
                sharpapi_partial = true;
                sharpapi.error = "SharpAPI response did not contain the documented data array.";
                break;
            }
            for (const auto &row : response.data["data"])
                sharpapi_rows.push_back(row);
            const auto pagination = response.data.value("pagination", json::object());
            const bool has_more = pagination.is_object() && pagination.value("has_more", false);
            if (!has_more)
                break;
            cursor = string_field(pagination, "next_cursor");
            if (cursor.empty() || cursor.size() > 1024) {
                sharpapi_partial = true;
                sharpapi.error = "SharpAPI indicated another page but supplied no safe next cursor.";
                break;
            }
            if (page + 1 == SHARPAPI_MAX_PAGES) {
                sharpapi_partial = true;
                sharpapi.error = "SharpAPI pagination cap reached; remaining odds pages were deferred.";
            }
        }
        if (sharpapi_has_page) {
            sharpapi.data = {{"data", std::move(sharpapi_rows)}};
            sharpapi.cached = sharpapi_all_cached;
            int sharpapi_count = 0;
            parse_guard("sharpapi", [&] {
                const auto before = s["quotes"].size();
                parse_sharpapi_odds(sharpapi.data, s, s, sharpapi.fetched_at);
                sharpapi_count = static_cast<int>(s["quotes"].size() - before);
            });
            auto sharpapi_source = source(
                "sharpapi", "SharpAPI NBA sportsbook moneylines", sharpapi, sharpapi_count,
                "Optional SharpAPI feed of pregame moneylines from books allowed by your plan. The Free "
                "tier documents 12 requests/minute and 60-second data delay; Edgecourt polls every "
                "5 minutes and reads at most 4 pages (at most 0.8 requests/minute averaged over the "
                "poll interval). Free does not publish a daily cap; per-key daily usage is available "
                "at /account/usage, but Edgecourt does not poll it to conserve calls. Docs publish "
                "X-RateLimit-* headers and recommend exponential backoff for 429s; Edgecourt currently "
                "uses a fixed 5-minute failure defer. Exchange rows already covered by direct "
                "Polymarket/Kalshi feeds are ignored.");
            if (sharpapi_pages > 0)
                sharpapi_source["message"] =
                    std::string(sharpapi_source.value("message", "")) + " Read " +
                    std::to_string(sharpapi_pages) + " page(s).";
            if (sharpapi_partial) {
                sharpapi_source["state"] = "partial";
                sharpapi_source["message"] = std::string(sharpapi_source.value("message", "")) + " " +
                                               sharpapi.error;
            }
            s["sources"].push_back(std::move(sharpapi_source));
        } else {
            s["sources"].push_back(source(
                "sharpapi", "SharpAPI NBA sportsbook moneylines", sharpapi, 0,
                "Optional SharpAPI pregame odds feed. Requests are cached for five minutes; "
                "provider errors are deferred for five minutes."));
        }
    }
    if (api_basketball_key().empty()) {
        s["sources"].push_back({{"id", "api-basketball"},
                                {"label", "API-Basketball NBA moneylines"},
                                {"state", "idle"},
                                {"updatedAt", ""},
                                {"count", 0},
                                {"message", "Optional keyed odds feed. Set API_BASKETBALL_KEY in the "
                                            "application process environment; keys are never stored in "
                                            "source code or cache."}});
    } else if (s["games"].empty()) {
        s["sources"].push_back({{"id", "api-basketball"},
                                {"label", "API-Basketball NBA moneylines"},
                                {"state", "idle"},
                                {"updatedAt", ""},
                                {"count", 0},
                                {"message", "No scheduled games are available to cross-reference. The "
                                            "keyed schedule and odds calls are deferred to avoid spending "
                                            "quota without a safe event match."}});
    } else {
        const auto season = api_basketball_season(now_seconds());
        const auto games_url = "https://v1.basketball.api-sports.io/games?league=12&season=" + season +
                               "&timezone=UTC";
        const auto odds_url =
            "https://v1.basketball.api-sports.io/odds?league=12&season=" + season + "&bet=2";
        const auto quota_gate = [this](const std::string &url, std::string &reason) {
            const int planned = url.find("/games?") != std::string::npos ? 2 : 1;
            return ensure_api_basketball_quota(planned, reason);
        };
        auto api_games = get(games_url, API_BASKETBALL_CACHE_SECONDS, 7 * API_BASKETBALL_CACHE_SECONDS,
                             API_BASKETBALL_FAILURE_RETRY_SECONDS, quota_gate);
        std::map<std::string, json> matched_api_games;
        if (!api_games.data.is_null())
            parse_guard("api-basketball", [&] {
                matched_api_games = match_api_basketball_games(api_games.data, s);
            });
        FeedResponse api_odds;
        if (api_games.data.is_null()) {
            api_odds.error = api_games.error;
        } else if (matched_api_games.empty()) {
            api_odds.error = "No API-Basketball game IDs matched the published NBA schedule; odds "
                             "were not requested.";
        } else {
            api_odds = get(odds_url, API_BASKETBALL_CACHE_SECONDS, 7 * API_BASKETBALL_CACHE_SECONDS,
                           API_BASKETBALL_FAILURE_RETRY_SECONDS, quota_gate);
        }
        int api_basketball_count = 0;
        if (!api_odds.data.is_null())
            parse_guard("api-basketball", [&] {
                const auto before = s["quotes"].size();
                parse_api_basketball_odds(api_odds.data, matched_api_games, s, api_odds.fetched_at);
                api_basketball_count = static_cast<int>(s["quotes"].size() - before);
            });
        auto api_basketball_source = source(
            "api-basketball", "API-Basketball NBA moneylines", api_odds, api_basketball_count,
            "Optional API-Sports feed. Event IDs are joined to the existing NBA schedule by both teams "
            "and tip-off; only pre-game Home/Away moneylines are retained. The provider documents odds "
            "updates once daily. The app caches both requests for 24 hours, reserves 20 calls, and checks "
            "the provider's non-quota status endpoint before the first billed request each UTC day; "
            "later attempts are tracked locally.");
        if (api_odds.data.is_null() && !api_odds.error.empty())
            api_basketball_source["message"] = api_odds.error;
        if (api_basketball_quota_checked_ && api_basketball_limit_ > 0) {
            api_basketball_source["message"] =
                std::string(api_basketball_source.value("message", "")) + " Quota status: " +
                std::to_string(api_basketball_current_) + "/" + std::to_string(api_basketball_limit_) +
                " daily requests used; 20 are reserved for other use.";
        }
        if (!api_basketball_quota_error_.empty())
            api_basketball_source["message"] =
                std::string(api_basketball_source.value("message", "")) + " " +
                api_basketball_quota_error_;
        if (!api_games.data.is_null() && matched_api_games.empty() && api_odds.data.is_null()) {
            api_basketball_source["state"] = "idle";
            api_basketball_source["updatedAt"] = api_games.fetched_at;
            api_basketball_source["message"] = api_odds.error;
        }
        s["sources"].push_back(std::move(api_basketball_source));
    }
    int pinnacle_count = 0;
    auto pinnacle_leagues = get("https://guest.api.arcadia.pinnacle.com/0.1/sports/4/leagues", 600, 7200, 21600);
    FeedResponse pinnacle_state = pinnacle_leagues;
    // Name the failing stage on the source card: a bare "HTTP 404" does not
    // say whether the league list, matchup list or markets request failed.
    std::string pinnacle_stage = "league list";
    if (pinnacle_leagues.data.is_array()) {
        pinnacle_stage.clear();
        int leagues_used = 0;
        for (const auto &league : pinnacle_leagues.data) {
            const auto name = string_field(league, "name");
            if ((name != "NBA" && name != "NBA Preseason") || leagues_used >= 2)
                continue;
            ++leagues_used;
            const auto league_id =
                std::to_string(static_cast<long long>(number(league.value("id", json()), -1)));
            const auto league_base = "https://guest.api.arcadia.pinnacle.com/0.1/leagues/" + league_id;
            auto matchups = get(league_base + "/matchups?brandId=0", 120, 600, 21600);
            // Pinnacle's own site and public integrations read straight
            // markets at /markets/straight. Keep the old period route only as
            // a 404 fallback; never try alternate routes after 429/5xx errors.
            auto straight = get(league_base + "/markets/straight?brandId=0", 45, 0, 21600);
            if (straight.data.is_null() && straight.status == 404)
                straight = get(league_base + "/markets/straight/period/0?brandId=0", 45, 0, 21600);
            pinnacle_state = straight;
            if (!matchups.data.is_null() && !straight.data.is_null()) {
                pinnacle_stage.clear();
                parse_guard("pinnacle", [&] {
                    const auto before = s["quotes"].size();
                    parse_pinnacle_markets(matchups.data, straight.data, s, straight.fetched_at);
                    pinnacle_count += static_cast<int>(s["quotes"].size() - before);
                });
            } else {
                pinnacle_stage = matchups.data.is_null() ? "matchup list" : "straight markets";
                pinnacle_state = matchups.data.is_null() ? matchups : straight;
            }
        }
        if (!leagues_used)
            pinnacle_stage = "league list (no NBA league is currently published)";
    }
    if (!pinnacle_count && !pinnacle_stage.empty()) {
        if (pinnacle_state.data.is_null()) {
            pinnacle_state.error = "Pinnacle " + pinnacle_stage + " request failed: " + pinnacle_state.error;
            if (pinnacle_state.status == 404 || pinnacle_state.status == 429 || pinnacle_state.status >= 500)
                pinnacle_state.error += " Retry deferred for up to 6 hours; no challenge, login or "
                                        "rate-limit bypass was attempted.";
        } else {
            pinnacle_state.data = nullptr;
            pinnacle_state.error = "Pinnacle " + pinnacle_stage + "; no moneylines this refresh.";
        }
    }
    s["sources"].push_back(source("pinnacle", "Pinnacle NBA moneylines", pinnacle_state, pinnacle_count,
                                  "Anonymous guest feed used by Pinnacle's public site (requests carry "
                                  "the site's published guest key; keyless calls now return 404). American "
                                  "prices converted to percentages. Undocumented and can change or block."));
    const bool has_upcoming_games = std::any_of(s["games"].begin(), s["games"].end(), [](const json &game) {
        return game.is_object() && string_field(game, "status", "upcoming") == "upcoming";
    });
    const bool has_scheduled_or_live_games = std::any_of(s["games"].begin(), s["games"].end(), [](const json &game) {
        const auto status = string_field(game, "status", "upcoming");
        return game.is_object() && (status == "upcoming" || status == "live");
    });
    if (rapidapi_key().empty()) {
        s["sources"].push_back({{"id", "sportsbook-api2"},
                                {"label", "Sportsbook API2 NBA moneylines"},
                                {"state", "idle"},
                                {"updatedAt", ""},
                                {"count", 0},
                                {"message", "Optional keyed odds feed. Set RAPIDAPI_KEY in the "
                                            "application process environment and confirm the exact RapidAPI "
                                            "host from its generated code snippet in RAPIDAPI_HOST; neither "
                                            "value is stored in source code, URLs or cache."}});
    } else if (!sportsbook_api2_host_confirmed()) {
        s["sources"].push_back({{"id", "sportsbook-api2"},
                                {"label", "Sportsbook API2 NBA moneylines"},
                                {"state", "idle"},
                                {"updatedAt", ""},
                                {"count", 0},
                                {"message", std::string("RAPIDAPI_KEY is present, but no request was made. "
                                                         "Compare the RapidAPI host in its generated code "
                                                         "snippet; set RAPIDAPI_HOST to exactly '") +
                                                SPORTSBOOK_API2_RAPIDAPI_HOST + "' only if it matches."}});
    } else if (!has_upcoming_games) {
        s["sources"].push_back({{"id", "sportsbook-api2"},
                                {"label", "Sportsbook API2 NBA moneylines"},
                                {"state", "idle"},
                                {"updatedAt", ""},
                                {"count", 0},
                                {"message", "No upcoming NBA games are available to match. The odds "
                                            "request is deferred to conserve the lower published daily "
                                            "allowance."}});
    } else {
        auto allow_sportsbook_request = [this](const std::string &, std::string &reason) {
            return ensure_sportsbook_api2_quota(1, reason);
        };
        FeedResponse sportsbook_api2;
        int sportsbook_api2_count = 0;
        auto competitions = get("https://sportsbook-api2.p.rapidapi.com/v0/competitions?includeInstances=false",
                                SPORTSBOOK_API2_COMPETITIONS_CACHE_SECONDS, 0,
                                6 * 60 * 60, allow_sportsbook_request);
        sportsbook_api2 = competitions;
        if (!competitions.data.is_null()) {
            const auto competition_key = sportsbook_api2_nba_competition_key(competitions.data);
            if (competition_key.empty()) {
                sportsbook_api2.data = nullptr;
                sportsbook_api2.error = "Sportsbook API2 returned no NBA competition key; no odds request was made.";
            } else {
                const auto from = iso_at(now_seconds()).substr(0, 10) + "T00:00:00Z";
                const auto to = iso_at(now_seconds() + 8 * 86400).substr(0, 10) + "T23:59:59Z";
                const auto events_url =
                    "https://sportsbook-api2.p.rapidapi.com/v0/competitions/" +
                    encode_query_component(competition_key) + "/events?eventType=MATCH&startTimeFrom=" +
                    encode_query_component(from) + "&startTimeTo=" + encode_query_component(to);
                auto events = get(events_url, SPORTSBOOK_API2_EVENTS_CACHE_SECONDS, 0,
                                  SPORTSBOOK_API2_EVENTS_CACHE_SECONDS, allow_sportsbook_request);
                sportsbook_api2 = events;
                if (!events.data.is_null()) {
                    std::vector<std::string> event_keys;
                    parse_guard("sportsbook-api2", [&] {
                        event_keys = sportsbook_api2_match_event_keys(events.data, s);
                    });
                    if (!event_keys.empty()) {
                        std::string odds_url =
                            "https://sportsbook-api2.p.rapidapi.com/v0/events/?returnType=array&"
                            "includeParticipants=true&includeCompetitions=false&includeLinks=false";
                        for (const auto &event_key : event_keys)
                            odds_url += "&eventKeys=" + encode_query_component(event_key);
                        auto odds = get(odds_url, SPORTSBOOK_API2_ODDS_CACHE_SECONDS, 0,
                                        SPORTSBOOK_API2_ODDS_CACHE_SECONDS, allow_sportsbook_request);
                        sportsbook_api2 = odds;
                        if (!odds.data.is_null())
                            parse_guard("sportsbook-api2", [&] {
                                sportsbook_api2_count = parse_sportsbook_api2_odds(
                                    odds.data, s, s, odds.fetched_at);
                            });
                    } else {
                        sportsbook_api2.error =
                            "No upcoming Sportsbook API2 NBA events matched the published schedule; "
                            "the detail/odds request was skipped.";
                    }
                }
            }
        }
        auto sportsbook_api2_source = source(
            "sportsbook-api2", "Sportsbook API2 NBA moneylines", sportsbook_api2, sportsbook_api2_count,
            "Schedule-matched full-game pregame sportsbook moneylines; odds refreshed every 2 hours, "
            "event list every 6 hours, competition list daily. Uses the lower published 50-requests/day "
            "limit with ten calls reserved for other RapidAPI use. Provider `payout` units are not "
            "documented: only plausible American prices or decimal prices (1–20) are imported; decimal "
            "conversion is explicitly heuristic and must be checked against a sanitized live sample. "
            "Indicative prices only; confirm at the sportsbook.");
        if (!sportsbook_api2.data.is_null() && sportsbook_api2.error.size())
            sportsbook_api2_source["message"] =
                std::string(sportsbook_api2_source.value("message", "")) + " " + sportsbook_api2.error;
        s["sources"].push_back(std::move(sportsbook_api2_source));
    }
    const auto current_eastern_date = daily_lineup_date_token(now_seconds());
    const auto nba_go_date = current_eastern_date.size() == 8
                                 ? current_eastern_date.substr(0, 4) + "-" + current_eastern_date.substr(4, 2) +
                                       "-" + current_eastern_date.substr(6, 2)
                                 : iso_at(now_seconds()).substr(0, 10);
    const bool nba_go_day_scheduled = std::any_of(s["games"].begin(), s["games"].end(), [&](const json &game) {
        const auto start = parse_time(string_field(game, "startsAt"));
        return game.is_object() && start > 0 && daily_lineup_date_token(start) == current_eastern_date;
    });
    if (!nba_go_day_scheduled) {
        s["sources"].push_back({{"id", "nba-go"},
                                {"label", "NBA_Go game stats and box-score context"},
                                {"state", "idle"},
                                {"updatedAt", ""},
                                {"count", 0},
                                {"message", "No scheduled NBA game falls on the current Eastern game date; "
                                            "the request is deferred. NBA_Go's inspected Swagger schema "
                                            "documents no authentication requirement, so no key is sent."}});
    } else {
        const auto nba_go_url =
            "https://api.server.nbaapi.com/api/games?date=" + nba_go_date +
            "&page=1&pageSize=100&sortBy=date&ascending=false&include="
            "lineScores%2CplayerGameBasicStats%2CteamGameAdvStats";
        auto nba_go = get(nba_go_url, NBA_GO_CACHE_SECONDS, 0, NBA_GO_CACHE_SECONDS);
        int nba_go_count = 0;
        if (!nba_go.data.is_null())
            parse_guard("nba-go", [&] { nba_go_count = parse_nba_go_games(nba_go.data, s, nba_go.fetched_at); });
        s["sources"].push_back(source(
            "nba-go", "NBA_Go game stats and box-score context", nba_go, nba_go_count,
            "Same-day box scores, line scores, player leaders and team rates are attached only to a "
            "teams/date match. Context only: does not overwrite the app's canonical score, roster or "
            "probabilities. Polls no more than every 6 hours; no authentication or quota was published "
            "in the inspected Swagger schema."));
    }
    if (thesportsdb_key().empty()) {
        s["sources"].push_back({{"id", "thesportsdb"},
                                {"label", "TheSportsDB NBA schedule context"},
                                {"state", "idle"},
                                {"updatedAt", ""},
                                {"count", 0},
                                {"message", "TheSportsDB V2 is premium-only. Set THESPORTSDB_KEY in the "
                                            "application process environment; Edgecourt uses the documented "
                                            "X-API-KEY header and never places the key in the URL."}});
    } else if (!has_upcoming_games) {
        s["sources"].push_back({{"id", "thesportsdb"},
                                {"label", "TheSportsDB NBA schedule context"},
                                {"state", "idle"},
                                {"updatedAt", ""},
                                {"count", 0},
                                {"message", "No upcoming NBA games are available to match; no schedule "
                                            "request was made."}});
    } else {
        auto thesportsdb = get("https://www.thesportsdb.com/api/v2/json/schedule/next/league/4387",
                               THESPORTSDB_CACHE_SECONDS, 0, THESPORTSDB_CACHE_SECONDS);
        int thesportsdb_count = 0;
        if (!thesportsdb.data.is_null())
            parse_guard("thesportsdb", [&] {
                thesportsdb_count = parse_thesportsdb_schedule(thesportsdb.data, s, thesportsdb.fetched_at);
            });
        s["sources"].push_back(source(
            "thesportsdb", "TheSportsDB NBA schedule context", thesportsdb, thesportsdb_count,
            "TheSportsDB V2 premium NBA next-events endpoint. Matched venue, status and any published "
            "score are context only and never affect probabilities. Cached for 6 hours (at most four "
            "polls/day); V1's key-in-URL format is not used."));
    }
    if (!has_scheduled_or_live_games) {
        s["sources"].push_back({{"id", "sportscore"},
                                {"label", "SportScore basketball fixtures"},
                                {"state", "idle"},
                                {"updatedAt", ""},
                                {"count", 0},
                                {"message", "No scheduled or live NBA games are available to match; no "
                                            "fixture request was made. SportScore's public tier is keyless."}});
    } else {
        const auto sportscore_date = iso_at(now_seconds()).substr(0, 10);
        const auto sportscore_url = "https://sportscore.com/api/v1/fixtures/?sport=basketball&date=" +
                                    sportscore_date + "&limit=200";
        auto sportscore = get(sportscore_url, SPORTSCORE_CACHE_SECONDS, 0, SPORTSCORE_CACHE_SECONDS);
        int sportscore_count = 0;
        if (!sportscore.data.is_null())
            parse_guard("sportscore", [&] {
                sportscore_count = parse_sportscore_fixtures(sportscore.data, s, sportscore.fetched_at);
            });
        s["sources"].push_back(source(
            "sportscore", "SportScore basketball fixtures", sportscore, sportscore_count,
            std::string("Keyless fixtures/scores joined to NBA games by team and time; an optional "
                        "SPORTSCORE_KEY is sent only as X-Api-Key to SportScore. Context only, never a "
                        "probability input. One request at most every 6 hours (four/day), well below the "
                        "documented ~10,000 requests/24h/IP tier. A visible dofollow attribution link is "
                        "provided wherever SportScore data is shown.")));
    }
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
        parse_guard("nba-players", [&] { parse_player_logs(players.data, s, players.fetched_at); });
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
        parse_guard("nba-teams", [&] { parse_team_ratings(ratings.data, s, ratings.fetched_at); });
    auto teamlogs = get("https://stats.nba.com/stats/leaguegamelog?Counter=0&DateFrom=" + date_from +
                            "&DateTo=&Direction=DESC&LeagueID=00&PlayerOrTeam=T&Season=" + season +
                            "&SeasonType=Regular%20Season&Sorter=DATE",
                        21600, 172800);
    if (!teamlogs.data.is_null())
        parse_guard("nba-teamlogs", [&] { parse_team_logs(teamlogs.data, s, teamlogs.fetched_at); });
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
    // More keyless context from swar/nba_api endpoint definitions (standings,
    // player season lines, team Four Factors). Cached for 6 hours like the other
    // stats feeds and fetched before the per-game/per-team fan-out below, so
    // the request budget can only ever starve roster refresh — never these
    // single-request feeds. They never gate or alter a probability.
    auto standings = get("https://stats.nba.com/stats/leaguestandingsv3?LeagueID=00&Season=" + season +
                             "&SeasonType=Regular%20Season&SeasonYear=",
                         21600, 172800);
    if (!standings.data.is_null())
        parse_guard("nba-standings", [&] { parse_standings(standings.data, s, standings.fetched_at); });
    int standings_count = 0;
    for (const auto &item : s["teams"].items())
        if (item.value().contains("standings"))
            ++standings_count;
    s["sources"].push_back(source("nba-standings", "NBA standings (nba_api LeagueStandingsV3)", standings,
                                  standings_count,
                                  "Wins/losses, win %, home/road and last-10 splits, streak, conference "
                                  "rank and games back. Context for moneyline research; not a model input."));
    auto season_stats =
        get("https://stats.nba.com/stats/leaguedashplayerstats?LastNGames=0&LeagueID=00&MeasureType=Base&"
            "Month=0&OpponentTeamID=0&PORound=&PaceAdjust=N&PerMode=PerGame&Period=0&PlusMinus=N&Rank=N&"
            "Season=" +
                season + "&SeasonType=Regular%20Season",
            21600, 172800);
    if (!season_stats.data.is_null())
        parse_guard("nba-season-stats",
                    [&] { parse_player_season_stats(season_stats.data, s, season_stats.fetched_at); });
    int season_stats_count = 0;
    for (const auto &item : s["players"].items())
        if (item.value().contains("seasonStats"))
            ++season_stats_count;
    s["sources"].push_back(source("nba-season-stats", "NBA player season stats (nba_api LeagueDashPlayerStats)",
                                  season_stats, season_stats_count,
                                  "Season per-game points/rebounds/assists/3PM, shooting percentages, "
                                  "plus-minus and double-/triple-double counts. Context beside the "
                                  "game-log model inputs; a season line never replaces dated logs."));
    const auto four_factors_url =
        "https://stats.nba.com/stats/"
        "leaguedashteamstats?Conference=&DateFrom=&DateTo=&Division=&GameScope=&GameSegment=&LastNGames=0&"
        "LeagueID=00&Location=&MeasureType=Four%20Factors&Month=0&OpponentTeamID=0&Outcome=&PORound=0&"
        "PaceAdjust=N&PerMode=PerGame&Period=0&PlayerExperience=&PlayerPosition=&PlusMinus=N&Rank=N&Season=" +
        season +
        "&SeasonSegment=&SeasonType=Regular%20Season&ShotClockRange=&StarterBench=&TeamID=0&TwoWay=0&"
        "VsConference=&VsDivision=";
    auto four_factors = get(four_factors_url, 21600, 172800);
    if (!four_factors.data.is_null())
        parse_guard("nba-four-factors",
                    [&] { parse_team_four_factors(four_factors.data, s, four_factors.fetched_at); });
    int four_factors_count = 0;
    for (const auto &item : s["teams"].items())
        if (item.value().contains("fourFactors"))
            ++four_factors_count;
    s["sources"].push_back(source("nba-four-factors", "NBA team Four Factors (nba_api LeagueDashTeamStats)",
                                  four_factors, four_factors_count,
                                  "Effective FG%, free-throw rate, turnover and offensive-rebound rates "
                                  "(plus opponent values). Context for moneyline/team-total research."));
    // Official/league expansion (awesome-nba-data Official & League Data):
    // NBA.com full-season schedule, NBA daily lineups, the static transactions
    // archive, and the remaining league-wide NBA Stats datasets that return
    // the whole league in one request. Season datasets are 6-hour cached
    // context and never alter a probability; only an explicitly confirmed
    // daily-lineup starter can satisfy the first-basket starter guard.
    auto player_field_count = [&s](const char *field) {
        int count = 0;
        for (const auto &item : s["players"].items())
            if (item.value().contains(field))
                ++count;
        return count;
    };
    auto team_field_count = [&s](const char *field) {
        int count = 0;
        for (const auto &item : s["teams"].items())
            if (item.value().contains(field))
                ++count;
        return count;
    };
    auto official_schedule = get("https://cdn.nba.com/static/json/staticData/scheduleLeagueV2.json", 3600, 21600);
    if (!official_schedule.data.is_null())
        parse_guard("nba-official-schedule", [&] {
            parse_official_schedule(official_schedule.data, s, official_schedule.fetched_at);
        });
    const int schedule_count =
        s.contains("leagueContext") && s["leagueContext"].contains("schedule")
            ? static_cast<int>(number(s["leagueContext"]["schedule"].value("games", json(0))))
            : 0;
    s["sources"].push_back(source("nba-official-schedule", "NBA.com full-season schedule (scheduleLeagueV2)",
                                  official_schedule, schedule_count,
                                  "Every scheduled game with NBA game ID/code, UTC tip, arena/city/state, "
                                  "neutral-site and postponement flags, series labels, points leaders and "
                                  "TV/radio/OTT broadcasters. Enriches games already in the window; the "
                                  "full season is counted, not dumped into the dashboard."));
    const auto lineup_token = daily_lineup_date_token(now_seconds());
    auto daily_lineups = get("https://stats.nba.com/js/data/leaders/00_daily_lineups_" + lineup_token + ".json",
                             300, 0);
    if (!daily_lineups.data.is_null())
        parse_guard("nba-daily-lineups", [&] {
            parse_daily_lineups(daily_lineups.data, s, daily_lineups.fetched_at, lineup_token);
        });
    const int daily_lineup_count =
        s.contains("leagueContext") && s["leagueContext"].contains("dailyLineups")
            ? static_cast<int>(number(s["leagueContext"]["dailyLineups"].value("players", json(0))))
            : 0;
    s["sources"].push_back(source("nba-daily-lineups", "NBA daily lineups (00_daily_lineups)", daily_lineups,
                                  daily_lineup_count,
                                  "Per-game listed players with position, lineup and roster status for " +
                                      lineup_token +
                                      ". Only status Confirmed with a listed position confirms a starter "
                                      "for first basket; Expected/projected entries are context and never "
                                      "confirm. A missing file (no games yet) stays unavailable."));
    auto player_movement = get("https://stats.nba.com/js/data/playermovement/NBA_Player_Movement.json", 21600,
                               172800);
    if (!player_movement.data.is_null())
        parse_guard("nba-transactions", [&] {
            parse_player_movement(player_movement.data, s, player_movement.fetched_at, season);
        });
    s["sources"].push_back(source("nba-transactions", "NBA player transactions (NBA_Player_Movement)",
                                  player_movement, static_cast<int>(s["transactions"].size()),
                                  "Static archive behind the NBA transactions page: signings, waivers, "
                                  "trades, waiver awards and contract conversions. Current-season rows are "
                                  "kept (latest 120) and attached to players as dated context; rows have no "
                                  "effective time and are never read as availability or eligibility."));
    auto player_index = get("https://stats.nba.com/stats/playerindex?Active=&AllStar=&College=&Country=&"
                            "DraftPick=&DraftRound=&DraftYear=&Height=&Historical=&LeagueID=00&Season=" +
                                season + "&TeamID=0&Weight=",
                            21600, 172800);
    if (!player_index.data.is_null())
        parse_guard("nba-player-index",
                    [&] { parse_player_index(player_index.data, s, player_index.fetched_at); });
    s["sources"].push_back(source("nba-player-index", "NBA player index (nba_api PlayerIndex)", player_index,
                                  player_field_count("playerIndex"),
                                  "League roster crosswalk: team, jersey, position, height/weight, college, "
                                  "country, draft year/round/pick, roster status, career years and current "
                                  "PTS/REB/AST. Context; a bare index record carries no game logs and "
                                  "cannot produce a projection by itself."));
    auto team_estimated = get("https://stats.nba.com/stats/teamestimatedmetrics?LeagueID=00&Season=" + season +
                                  "&SeasonType=Regular%20Season",
                              21600, 172800);
    if (!team_estimated.data.is_null())
        parse_guard("nba-team-estimated", [&] {
            parse_team_estimated_metrics(team_estimated.data, s, team_estimated.fetched_at);
        });
    s["sources"].push_back(source("nba-team-estimated", "NBA team estimated metrics (TeamEstimatedMetrics)",
                                  team_estimated, team_field_count("estimatedMetrics"),
                                  "Estimated offensive/defensive/net rating, pace, assist/rebound/turnover "
                                  "rates and league ranks. Context beside the measured Advanced ratings."));
    auto player_estimated = get("https://stats.nba.com/stats/playerestimatedmetrics?LeagueID=00&Season=" + season +
                                    "&SeasonType=Regular%20Season",
                                21600, 172800);
    if (!player_estimated.data.is_null())
        parse_guard("nba-player-estimated", [&] {
            parse_player_estimated_metrics(player_estimated.data, s, player_estimated.fetched_at);
        });
    s["sources"].push_back(source("nba-player-estimated", "NBA player estimated metrics (PlayerEstimatedMetrics)",
                                  player_estimated, player_field_count("estimatedMetrics"),
                                  "Estimated offensive/defensive/net rating, pace, assist/rebound/turnover/"
                                  "usage rates and league ranks. Context; dated game logs remain the model "
                                  "input."));
    const auto clutch_query =
        "?AheadBehind=Ahead%20or%20Behind&ClutchTime=Last%205%20Minutes&LeagueID=00&MeasureType=Base&"
        "PerMode=Totals&PointDiff=5&Season=" +
        season + "&SeasonType=Regular%20Season";
    auto team_clutch = get("https://stats.nba.com/stats/leaguedashteamclutch" + clutch_query, 21600, 172800);
    if (!team_clutch.data.is_null())
        parse_guard("nba-team-clutch",
                    [&] { parse_team_clutch(team_clutch.data, s, team_clutch.fetched_at); });
    s["sources"].push_back(source("nba-team-clutch", "NBA team clutch stats (LeagueDashTeamClutch)", team_clutch,
                                  team_field_count("clutch"),
                                  "Clutch totals (last 5 minutes, score within 5): record, shooting, "
                                  "rebounds, assists, turnovers, steals, blocks, points and plus-minus. "
                                  "Context only."));
    auto player_clutch = get("https://stats.nba.com/stats/leaguedashplayerclutch" + clutch_query, 21600, 172800);
    if (!player_clutch.data.is_null())
        parse_guard("nba-player-clutch",
                    [&] { parse_player_clutch(player_clutch.data, s, player_clutch.fetched_at); });
    s["sources"].push_back(source("nba-player-clutch", "NBA player clutch stats (LeagueDashPlayerClutch)",
                                  player_clutch, player_field_count("clutch"),
                                  "Clutch totals (last 5 minutes, score within 5) with fantasy points and "
                                  "double-/triple-double counts. Context beside the log-based projection."));
    auto team_hustle = get("https://stats.nba.com/stats/leaguehustlestatsteam?LeagueID=00&PerMode=Totals&Season=" +
                               season + "&SeasonType=Regular%20Season",
                           21600, 172800);
    if (!team_hustle.data.is_null())
        parse_guard("nba-team-hustle",
                    [&] { parse_team_hustle(team_hustle.data, s, team_hustle.fetched_at); });
    s["sources"].push_back(source("nba-team-hustle", "NBA team hustle stats (LeagueHustleStatsTeam)", team_hustle,
                                  team_field_count("hustle"),
                                  "Season hustle totals: contested shots, deflections, charges drawn, screen "
                                  "assists/points, loose balls recovered and box-outs. Context only."));
    auto player_hustle = get("https://stats.nba.com/stats/leaguehustlestatsplayer?LeagueID=00&PerMode=Totals&Season=" +
                                 season + "&SeasonType=Regular%20Season",
                             21600, 172800);
    if (!player_hustle.data.is_null())
        parse_guard("nba-player-hustle",
                    [&] { parse_player_hustle(player_hustle.data, s, player_hustle.fetched_at); });
    s["sources"].push_back(source("nba-player-hustle", "NBA player hustle stats (LeagueHustleStatsPlayer)",
                                  player_hustle, player_field_count("hustle"),
                                  "Season hustle totals: contested shots, deflections, charges drawn, screen "
                                  "assists/points, loose balls recovered and box-outs. Context only."));
    auto team_tracking_defense =
        get("https://stats.nba.com/stats/leaguedashptteamdefend?DefenseCategory=Overall&LeagueID=00&"
            "PerMode=Totals&Season=" +
                season + "&SeasonType=Regular%20Season",
            21600, 172800);
    if (!team_tracking_defense.data.is_null())
        parse_guard("nba-team-tracking-defense", [&] {
            parse_team_tracking_defense(team_tracking_defense.data, s, team_tracking_defense.fetched_at);
        });
    s["sources"].push_back(source("nba-team-tracking-defense", "NBA team tracking defense (LeagueDashPtTeamDefend)",
                                  team_tracking_defense, team_field_count("trackingDefense"),
                                  "Defended field-goal % versus the shooters' normal % (PCT_PLUSMINUS "
                                  "differential). Context for moneyline/team-total research."));
    auto opponent_shots = get("https://stats.nba.com/stats/leaguedashoppptshot?LeagueID=00&PerMode=Totals&Season=" +
                                  season + "&SeasonType=Regular%20Season",
                              21600, 172800);
    if (!opponent_shots.data.is_null())
        parse_guard("nba-opponent-shots", [&] {
            parse_team_opponent_shots(opponent_shots.data, s, opponent_shots.fetched_at);
        });
    s["sources"].push_back(source("nba-opponent-shots", "NBA opponent shot profile (LeagueDashOppPtShot)",
                                  opponent_shots, team_field_count("opponentShotProfile"),
                                  "The tracking shot profile opponents record against each team (frequency, "
                                  "FG%/eFG%, 2PT/3PT splits). Context only."));
    auto player_tracking_shots =
        get("https://stats.nba.com/stats/leaguedashplayerptshot?LeagueID=00&PerMode=Totals&Season=" + season +
                "&SeasonType=Regular%20Season",
            21600, 172800);
    if (!player_tracking_shots.data.is_null())
        parse_guard("nba-player-tracking-shots", [&] {
            parse_player_tracking_shots(player_tracking_shots.data, s, player_tracking_shots.fetched_at);
        });
    s["sources"].push_back(source("nba-player-tracking-shots", "NBA player tracking shots (LeagueDashPlayerPtShot)",
                                  player_tracking_shots, player_field_count("trackingShooting"),
                                  "Tracking shooting profile: attempt frequency, FG%/eFG% and 2PT/3PT "
                                  "splits. Context beside the log-derived shooting features."));
    auto player_tracking_defense =
        get("https://stats.nba.com/stats/leaguedashptdefend?DefenseCategory=Overall&LeagueID=00&"
            "PerMode=Totals&Season=" +
                season + "&SeasonType=Regular%20Season",
            21600, 172800);
    if (!player_tracking_defense.data.is_null())
        parse_guard("nba-player-tracking-defense", [&] {
            parse_player_tracking_defense(player_tracking_defense.data, s, player_tracking_defense.fetched_at);
        });
    s["sources"].push_back(source("nba-player-tracking-defense", "NBA player tracking defense (LeagueDashPtDefend)",
                                  player_tracking_defense, player_field_count("trackingDefense"),
                                  "Defended field-goal % versus the shooters' normal % (PCT_PLUSMINUS "
                                  "differential), overall category. Context only."));
    auto team_lineups = get("https://stats.nba.com/stats/leaguedashlineups?GroupQuantity=5&LastNGames=0&"
                            "LeagueID=00&MeasureType=Base&PerMode=Totals&Season=" +
                                season + "&SeasonType=Regular%20Season",
                            21600, 172800, 21600);
    if (!team_lineups.data.is_null())
        parse_guard("nba-team-lineups",
                    [&] { parse_team_top_lineups(team_lineups.data, s, team_lineups.fetched_at); });
    else if (team_lineups.status > 0)
        team_lineups.error += " Retry deferred for up to 6 hours; no challenge, login or rate-limit bypass "
                              "was attempted.";
    s["sources"].push_back(source("nba-team-lineups", "NBA five-man lineups (LeagueDashLineups)", team_lineups,
                                  team_field_count("topLineup"),
                                  "Each team's most-used five-man unit (minutes, record, shooting, points "
                                  "and plus-minus). Season context; it never confirms today's starters — "
                                  "only the daily-lineup feed can do that."));
    s["sources"].push_back(
        {{"id", "nba-injury-report"},
         {"label", "Official NBA injury report (reviewed, not auto-ingested)"},
         {"state", "import_required"},
         {"updatedAt", ""},
         {"count", 0},
         {"message", "The official report is a document/PDF surface. The awesome-nba-data source audit records "
                     "automated requests as 403-anti-bot with no stable bulk API, so Edgecourt does not guess "
                     "timestamped report URLs or bypass the restriction. Availability context comes from ESPN "
                     "roster listings and the NBA daily-lineup feed; absence of a listing is never treated as "
                     "proof a player is healthy."}});
    // Fan-out runs last, after every single-request feed: live summaries gate
    // in-play eligibility, rosters are hourly-cached context. Either can be
    // cut short by the request budget without starving the feeds above.
    int summaries = 0;
    if (!s["games"].empty())
        for (const auto &game : s["games"]) {
            if (game.value("status", "") != "live" || !game.contains("espnId") || summaries >= 6)
                continue;
            auto summary = get("https://site.api.espn.com/apis/site/v2/sports/basketball/nba/summary?event=" +
                                   game.value("espnId", ""),
                               30, 0);
            if (!summary.data.is_null())
                parse_guard("boxscores", [&] {
                    parse_espn_summary(summary.data, s, game, summary.fetched_at);
                    ++summaries;
                });
        }
    s["sources"].push_back({{"id", "boxscores"},
                            {"label", "ESPN live player box scores"},
                            {"state", summaries ? "ok" : "idle"},
                            {"updatedAt", summaries ? now_iso() : ""},
                            {"count", summaries},
                            {"message", "Fresh accrued stats are mandatory for in-play props. No live player "
                                        "value is extrapolated from a pregame-only feed."}});
    int roster_count = 0, roster_failures = 0, roster_network = 0;
    if (!s["games"].empty())
        for (const auto &team : active) {
            if (!s["teams"].contains(team) || !s["teams"][team].contains("espnId"))
                continue;
            // Fan-out cap: at most 10 roster *network* fetches per refresh
            // (cache hits are free). Hourly-cached rosters fill in over the
            // next 60-second auto-refreshes; single-request feeds above can
            // never be starved by this fan-out again.
            if (roster_network >= 10)
                break;
            auto roster = get("https://site.api.espn.com/apis/site/v2/sports/basketball/nba/teams/" +
                                  s["teams"][team].value("espnId", "") + "/roster",
                              3600, 7200);
            if (!roster.cached)
                ++roster_network;
            if (!roster.data.is_null())
                parse_guard("rosters", [&] {
                    parse_espn_roster(roster.data, s, team, roster.fetched_at);
                    ++roster_count;
                });
            else
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
    for (auto &card : s["sources"]) {
        const auto failed = parse_errors.find(card.value("id", ""));
        if (failed == parse_errors.end())
            continue;
        card["state"] = "unavailable";
        card["count"] = 0;
        card["message"] = "The source responded, but its payload could not be parsed (" + failed->second +
                          "). It contributed no data this refresh; other sources were unaffected.";
    }
    s["generatedAt"] = now_iso();
    return s;
}
} // namespace edgecourt
