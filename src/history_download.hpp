#pragma once
#include "core.hpp"
#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace edgecourt {
// Automatic local history: downloads the SportsDataverse (hoopR) ESPN
// play-by-play + player box-score CSVs for the most recent five seasons from
// the public sportsdataverse-data GitHub releases, keeps them under
// <history>/downloads, and builds the local features.jsonl bundle natively
// (the same bundle schema the Python collector exports). Everything runs on
// a background thread; startup and the dashboard never block on it. Past
// seasons are downloaded once; the current season refreshes at most daily.
class HistoryDownloader {
  public:
    explicit HistoryDownloader(std::filesystem::path history_dir);
    ~HistoryDownloader();
    HistoryDownloader(const HistoryDownloader &) = delete;
    HistoryDownloader &operator=(const HistoryDownloader &) = delete;
    // Invoked (on the downloader thread, with no downloader lock held) when a
    // run finishes and a rebuilt bundle may be ready to reload.
    void set_on_finished(std::function<void()> callback);
    // Start a background sync if none is running. Non-forced starts are
    // suppressed for 10 minutes after a failed attempt; forced starts (the
    // manual button) always run. Returns true when a run was started.
    bool start(bool force = false);
    bool running() const { return running_.load(); }
    json status() const;
    // Season start-years fetched by the auto-download (most recent five).
    static std::vector<int> target_seasons(long long epoch_seconds);

  private:
    void run();
    void set_status(json status);
    std::filesystem::path history_dir_;
    mutable std::mutex mutex_;
    json status_ = {{"state", "idle"}};
    std::function<void()> on_finished_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    long long last_attempt_ = 0;
    bool last_failed_ = false;
};

// Build (or rebuild) features.jsonl in history_dir from the season CSVs in
// history_dir/downloads. Mirrors the Python hoopR export: strictly pre-cutoff
// games, 210-day recent window for logs, dated first-made-field-goal
// opportunities for historical starters. Exposed for tests. Throws
// std::invalid_argument on schema problems; the previous bundle is retained.
json build_history_bundle(const std::filesystem::path &history_dir, const std::vector<int> &seasons,
                          const std::string &as_of_date);
} // namespace edgecourt
