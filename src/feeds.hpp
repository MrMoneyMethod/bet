#pragma once
#include "core.hpp"
#include <atomic>
#include <filesystem>
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
void parse_kalshi_markets(const json &document, json &snapshot, const std::string &fetched_at);
void parse_pinnacle_markets(const json &matchups, const json &markets, json &snapshot,
                            const std::string &fetched_at);
json scrape_saved_html(const std::string &html, const std::string &player = "", const std::string &team = "",
                       const std::string &observed_at = "");
class PublicFeeds {
  public:
    explicit PublicFeeds(std::filesystem::path cache_dir);
    json refresh(const json &previous, const std::atomic<bool> *cancelled = nullptr);

  private:
    FeedResponse get(const std::string &url, int ttl_seconds, int max_stale_seconds = 0);
    std::filesystem::path cache_dir_;
    int requests_ = 0;
    const std::atomic<bool> *cancelled_ = nullptr;
};
} // namespace edgecourt
