#pragma once
#include "core.hpp"
#include <filesystem>

namespace edgecourt {
struct HistoryBundle {
    json features = {{"quotes", json::array()},
                     {"games", json::array()},
                     {"players", json::object()},
                     {"teams", json::object()}};
    json status = {
        {"state", "not_installed"},
        {"games", 0},
        {"events", 0},
        {"players", 0},
        {"teams", 0},
        {"missingGames", 0},
        {"message", "No local history bundle installed. The historical crawl has not been run here."}};
};
// A local, bounded, streaming feature bundle; raw all-season events stay in the external SQLite archive.
HistoryBundle load_history_bundle(const std::filesystem::path &directory);
void apply_history(json &snapshot, const HistoryBundle &history);
} // namespace edgecourt
