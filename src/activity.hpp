#pragma once
#include "core.hpp"

namespace edgecourt {
// Exchange context only. No volume/trade data is an input to the betting model.
void record_exchange_market(json &snapshot, const json &market);
void parse_polymarket_trades(const json &document, json &snapshot, const std::string &fetched_at);
json activity_dashboard(const json &snapshot);
void expire_activity(json &activity);
void seed_demo_activity(json &snapshot);
} // namespace edgecourt
