// Explicit diagnostic, never run on application startup or in offline CI.
#include "feeds.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <random>
using namespace edgecourt;
int main(int argc, char **argv) {
    if (argc != 2 || std::string(argv[1]) != "--live") {
        std::cerr << "Usage: edgecourt_probe --live > provider-probe.json\n"
                     "Makes bounded public requests using native transport, schema and backoff guards.\n"
                     "Optional keyed providers are skipped; no key/account data is written.\n";
        return 2;
    }
    const auto cache = std::filesystem::temp_directory_path() /
                       ("edgecourt-probe-" + std::to_string(std::random_device{}()));
    int failures = 0;
    try {
        PublicFeeds feeds(cache);
        const auto day = now_iso().substr(0, 10), season = nba_season(parse_time(now_iso()));
        const std::vector<std::pair<std::string, std::string>> endpoints = {
            {"ESPN scoreboard",
             espn_scoreboard_day_urls(parse_time(now_iso()), parse_time(now_iso())).front()},
            {"ESPN roster", "https://site.api.espn.com/apis/site/v2/sports/basketball/nba/teams/2/roster"},
            {"NBA CDN scoreboard",
             "https://cdn.nba.com/static/json/liveData/scoreboard/todaysScoreboard_00.json"},
            {"NBA CDN sportsbook odds",
             "https://cdn.nba.com/static/json/liveData/odds/odds_todaysGames.json"},
            {"NBA season schedule", "https://cdn.nba.com/static/json/staticData/scheduleLeagueV2.json"},
            {"Polymarket Gamma NBA events",
             "https://gamma-api.polymarket.com/"
             "events?tag_slug=nba&active=true&closed=false&limit=100&offset=0"},
            {"Polymarket Data API public trades",
             "https://data-api.polymarket.com/"
             "trades?limit=1&takerOnly=true&filterType=CASH&filterAmount=1000"},
            {"Kalshi NBA contracts", "https://external-api.kalshi.com/trade-api/v2/"
                                     "markets?series_ticker=KXNBAGAME&status=open&limit=200"},
            {"Pinnacle guest basketball leagues",
             "https://guest.api.arcadia.pinnacle.com/0.1/sports/4/leagues"},
            {"NBA Stats standings", "https://stats.nba.com/stats/leaguestandingsv3?LeagueID=00&Season=" +
                                        season + "&SeasonType=Regular%20Season&SeasonYear="},
            {"NBA daily lineups", "https://stats.nba.com/js/data/leaders/00_daily_lineups_" +
                                      daily_lineup_date_token(parse_time(now_iso())) + ".json"},
            {"NBA_Go context", "https://api.server.nbaapi.com/api/games?date=" + day +
                                   "&page=1&pageSize=100&sortBy=date&ascending=false&include=lineScores%"
                                   "2CplayerGameBasicStats%2CteamGameAdvStats"}};
        json report = {{"generatedAt", now_iso()},
                       {"scope", "Representative public endpoints; transport and envelope validation, not a "
                                 "guarantee of usable NBA joins or unrestricted quotas."},
                       {"warning", "Transport failures in a restricted network do not establish provider "
                                   "outages. Live rate limits were not load-tested."},
                       {"results", json::array()},
                       {"skipped", json::array({"API-Basketball: optional keyed quota",
                                                "SharpAPI: optional key", "TheSportsDB: premium key",
                                                "Sportsbook API2: removed, speculative contract",
                                                "SportScore: removed, unestablished fixtures route"})}};
        for (const auto &endpoint : endpoints) {
            const auto started = std::chrono::steady_clock::now();
            const auto r = feeds.get(endpoint.second, 60, 0, 60);
            const bool ok = !r.data.is_null();
            if (!ok)
                ++failures;
            report["results"].push_back({{"provider", endpoint.first},
                                         {"url", endpoint.second},
                                         {"httpStatus", r.status},
                                         {"envelopeValid", ok},
                                         {"classification", ok                ? "schema_ok"
                                                            : r.status == 0   ? "network_unverified"
                                                            : r.status == 200 ? "unsupported_payload"
                                                                              : "http_unavailable"},
                                         {"error", r.error},
                                         {"elapsedMs", std::chrono::duration_cast<std::chrono::milliseconds>(
                                                           std::chrono::steady_clock::now() - started)
                                                           .count()}});
        }
        report["passed"] = endpoints.size() - failures;
        report["failedOrUnverified"] = failures;
        std::cout << report.dump(2) << "\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << "\n";
        failures++;
    }
    std::error_code error;
    std::filesystem::remove_all(cache, error);
    return failures ? 1 : 0;
}
