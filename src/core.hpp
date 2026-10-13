#pragma once
#include "nlohmann/json.hpp"
#include <string>
#include <vector>

namespace edgecourt {
using json = nlohmann::json;
constexpr const char *VERSION = "0.3.0";
constexpr const char *MODEL_VERSION = "weighted-bootstrap-v1-experimental";

std::string now_iso();
std::string iso_at(long long epoch_seconds);
long long parse_time(const std::string &text);
std::string normalize_name(std::string text);
std::string stable_id(const std::string &text);
std::string quote_id(const json &quote);
double number(const json &value, double fallback = 0.0);
std::string text(const json &value, const std::string &fallback = "");
double decimal_odds(int american);
double implied_probability(int american);
bool has_quote_price(const json &quote);
double quote_implied_probability(const json &quote);
double quote_decimal_odds(const json &quote);
double expected_value(double win, double push, int american);
double expected_value(double win, double push, const json &quote);
double fractional_kelly(double win, double push, int american, double fraction = 0.25);
double fractional_kelly(double win, double push, const json &quote, double fraction = 0.25);
json first_basket_counts(const json &player, const std::string &starts_at);
json wilson_interval(double probability, double effective_n);
json empty_snapshot(const std::string &mode);
json demo_snapshot();
json feature_catalog();
json evaluate_quote(const json &quote, const json &snapshot);
json build_dashboard(const json &snapshot);
json expire_dashboard(json dashboard);
json validate_import(const json &document);
json parse_csv_import(const std::string &csv);
void merge_import(json &snapshot, const json &validated);
json score_backtest(const json &document);
std::string csv_template(const json &snapshot);
std::string nba_season(long long epoch_seconds, bool previous = false);
} // namespace edgecourt
