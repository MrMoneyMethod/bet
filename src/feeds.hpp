#pragma once
#include "core.hpp"
#include <atomic>
#include <filesystem>
#include <functional>
#include <map>

namespace edgecourt {
struct HttpResponse {
    int status = 0;
    std::string body, error;
};
struct FeedResponse {
    json data;
    std::string fetched_at, error;
    bool cached = false, stale = false;
    int status = 0;
};
bool allowed_data_url(const std::string &url);
// Per-host request headers for public_get. Keyed feeds receive only their
// respective environment credentials on exact fixed hosts. Linux transports
// sensitive headers over a private pipe to avoid process-argument exposure.
// Pinnacle also receives its public-site guest headers; other hosts get generic
// JSON headers. User credentials never appear in URLs.
std::vector<std::pair<std::string, std::string>> public_request_headers(const std::string &url);
HttpResponse public_get(const std::string &url);
// One scoreboard URL per UTC day. ESPN's site API now rejects hyphenated
// `dates=YYYYMMDD-YYYYMMDD` ranges with HTTP 400, so a window is fetched as
// single-day requests and merged by event id. Never returns a ranged URL.
std::vector<std::string> espn_scoreboard_day_urls(long long first_epoch, long long last_epoch);
json nba_rows(const json &document);
void parse_espn_scoreboard(const json &document, json &snapshot, const std::string &fetched_at);
void parse_nba_scoreboard(const json &document, json &snapshot, const std::string &fetched_at);
void parse_espn_summary(const json &document, json &snapshot, const json &game,
                        const std::string &fetched_at);
void parse_espn_roster(const json &document, json &snapshot, const std::string &team,
                       const std::string &fetched_at);
void parse_player_logs(const json &document, json &snapshot, const std::string &fetched_at);
void parse_team_logs(const json &document, json &snapshot, const std::string &fetched_at);
void parse_team_ratings(const json &document, json &snapshot, const std::string &fetched_at);
// More data derived from swar/nba_api (MIT) endpoint definitions, re-implemented
// in C++ against stats.nba.com. All three are context data: they are displayed
// and catalogued as context, never silently fed into a probability.
void parse_standings(const json &document, json &snapshot, const std::string &fetched_at);
void parse_player_season_stats(const json &document, json &snapshot, const std::string &fetched_at);
void parse_team_four_factors(const json &document, json &snapshot, const std::string &fetched_at);
// Official/league expansion from the awesome-nba-data Official & League Data
// section (NBA.com CDN + NBA Stats). Every parser is league-wide and bounded
// to one request per refresh. Season datasets are context only; the daily
// lineup parser is the one deliberate exception to "context only" in the
// sense that an explicitly *confirmed* starter may satisfy the existing
// first-basket starter guard. Expected/projected lineups never confirm.
void parse_official_schedule(const json &document, json &snapshot, const std::string &fetched_at);
void parse_daily_lineups(const json &document, json &snapshot, const std::string &fetched_at,
                         const std::string &date_token);
void parse_player_movement(const json &document, json &snapshot, const std::string &fetched_at,
                           const std::string &season);
void parse_player_index(const json &document, json &snapshot, const std::string &fetched_at);
void parse_player_clutch(const json &document, json &snapshot, const std::string &fetched_at);
void parse_team_clutch(const json &document, json &snapshot, const std::string &fetched_at);
void parse_player_hustle(const json &document, json &snapshot, const std::string &fetched_at);
void parse_team_hustle(const json &document, json &snapshot, const std::string &fetched_at);
void parse_player_estimated_metrics(const json &document, json &snapshot, const std::string &fetched_at);
void parse_team_estimated_metrics(const json &document, json &snapshot, const std::string &fetched_at);
void parse_player_tracking_shots(const json &document, json &snapshot, const std::string &fetched_at);
void parse_player_tracking_defense(const json &document, json &snapshot, const std::string &fetched_at);
void parse_team_tracking_defense(const json &document, json &snapshot, const std::string &fetched_at);
void parse_team_opponent_shots(const json &document, json &snapshot, const std::string &fetched_at);
void parse_team_top_lineups(const json &document, json &snapshot, const std::string &fetched_at);
std::string daily_lineup_date_token(long long epoch_seconds);
void parse_polymarket_events(const json &document, json &snapshot, const std::string &fetched_at);
std::string api_basketball_season(long long epoch_seconds);
std::map<std::string, json> match_api_basketball_games(const json &document, const json &snapshot);
void parse_api_basketball_odds(const json &document, const std::map<std::string, json> &matched_games,
                               json &snapshot, const std::string &fetched_at);
bool api_basketball_quota_allows(int current, int daily_limit, int reserve, int calls_needed = 1);
// NBA.com CDN odds file (odds_todaysGames.json): Sportradar-sourced moneylines
// from named sportsbooks, published by the league itself. "2way" markets only;
// decimal odds are converted to American. This is a public league file, not a
// scraped sportsbook page.
void parse_nba_cdn_odds(const json &document, json &snapshot, const std::string &fetched_at);
void parse_kalshi_markets(const json &document, json &snapshot, const std::string &fetched_at);
void parse_sharpapi_odds(const json &document, const json &schedule, json &snapshot,
                         const std::string &fetched_at);
// Sportsbook API2/RapidAPI NBA pregame moneylines. The numeric `payout` field
// is not unit-labeled by the provider schema; parsers accept plausible American
// or decimal prices only and leave other formats unpriced.
int parse_sportsbook_api2_odds(const json &document, const json &schedule, json &snapshot,
                               const std::string &fetched_at);
std::string sportsbook_api2_nba_competition_key(const json &document);
std::vector<std::string> sportsbook_api2_match_event_keys(const json &document, const json &schedule);
// Optional cross-source context. These attach metadata to matching scheduled
// games only; none of these providers changes scores, ratings or probabilities.
int parse_nba_go_games(const json &document, json &snapshot, const std::string &fetched_at);
int parse_thesportsdb_schedule(const json &document, json &snapshot, const std::string &fetched_at);
int parse_sportscore_fixtures(const json &document, json &snapshot, const std::string &fetched_at);
bool sportsbook_api2_quota_allows(int current, int daily_limit, int reserve, int calls_needed = 1);
void parse_pinnacle_markets(const json &matchups, const json &markets, json &snapshot,
                            const std::string &fetched_at);
json scrape_saved_html(const std::string &html, const std::string &player = "", const std::string &team = "",
                       const std::string &observed_at = "");
class PublicFeeds {
  public:
    explicit PublicFeeds(std::filesystem::path cache_dir);
    json refresh(const json &previous, const std::atomic<bool> *cancelled = nullptr);

  private:
    FeedResponse get(const std::string &url, int ttl_seconds, int max_stale_seconds = 0,
                     int failed_retry_seconds = 0,
                     const std::function<bool(const std::string &, std::string &)> &may_request = {});
    bool ensure_api_basketball_quota(int calls_needed, std::string &reason);
    void save_api_basketball_quota();
    void record_api_basketball_response(const json &document);
    bool ensure_sportsbook_api2_quota(int calls_needed, std::string &reason);
    void save_sportsbook_api2_quota();
    std::filesystem::path cache_dir_;
    int requests_ = 0;
    int api_basketball_current_ = 0, api_basketball_limit_ = 0;
    int sportsbook_api2_current_ = 0;
    bool api_basketball_quota_checked_ = false, api_basketball_active_ = false;
    bool sportsbook_api2_quota_loaded_ = false;
    std::string api_basketball_quota_date_, api_basketball_quota_checked_at_, api_basketball_quota_error_;
    std::string sportsbook_api2_quota_date_;
    const std::atomic<bool> *cancelled_ = nullptr;
};
} // namespace edgecourt
