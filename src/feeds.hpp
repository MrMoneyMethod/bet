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
