#include "activity.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <regex>
#include <set>

namespace edgecourt {
namespace {
std::string str(const json &j, const char *key) {
    if (!j.is_object())
        return "";
    const auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : "";
}
bool hex_id(const std::string &s, size_t bytes) {
    return s.size() == bytes * 2 + 2 && s.substr(0, 2) == "0x" &&
           s.find_first_not_of("0123456789abcdefABCDEF", 2) == std::string::npos;
}
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}
} // namespace
void record_exchange_market(json &snapshot, const json &market) {
    if (!snapshot.contains("exchangeMarkets"))
        snapshot["exchangeMarkets"] = json::array();
    for (auto &old : snapshot["exchangeMarkets"])
        if (old.value("id", "") == market.value("id", "")) {
            old = market;
            return;
        }
    if (snapshot["exchangeMarkets"].size() < 1000)
        snapshot["exchangeMarkets"].push_back(market);
}
void parse_polymarket_trades(const json &doc, json &s, const std::string &fetched) {
    if (!doc.is_array())
        return;
    if (!s.contains("whaleTrades"))
        s["whaleTrades"] = json::array();
    const auto now = parse_time(now_iso()), fetched_epoch = parse_time(fetched);
    if (!fetched_epoch || fetched_epoch > now + 30)
        return;
    std::set<std::string> seen;
    for (const auto &t : s["whaleTrades"])
        seen.insert(t.value("id", ""));
    size_t processed = 0;
    for (const auto &raw : doc) {
        if (++processed > 1000)
            break;
        const auto condition = lower(str(raw, "conditionId"));
        const auto wallet = lower(str(raw, "proxyWallet"));
        const auto tx = lower(str(raw, "transactionHash"));
        const auto asset = str(raw, "asset"), side = str(raw, "side");
        if (!hex_id(condition, 32) || !hex_id(wallet, 20) || !hex_id(tx, 32) || asset.empty() ||
            asset.size() > 100 || asset.find_first_not_of("0123456789") != std::string::npos ||
            (side != "BUY" && side != "SELL"))
            continue;
        const auto nan = std::numeric_limits<double>::quiet_NaN();
        const double size = number(raw.value("size", json()), nan),
                     price = number(raw.value("price", json()), nan),
                     stamp = number(raw.value("timestamp", json()), nan);
        if (!std::isfinite(size) || size <= 0 || size > 1e12 || !std::isfinite(price) || price <= 0 ||
            price >= 1 || !std::isfinite(stamp) || stamp < now - 86400 ||
            stamp > std::min(now, fetched_epoch) + 30 || std::floor(stamp) != stamp || size * price < 1000)
            continue;
        const json *market = nullptr;
        if (!s.contains("exchangeMarkets"))
            continue;
        for (const auto &m : s["exchangeMarkets"])
            if (m.value("provider", "") == "polymarket" && m.value("marketId", "") == condition &&
                m.contains("outcomeTokens") && m["outcomeTokens"].is_object() &&
                m["outcomeTokens"].contains(asset)) {
                market = &m;
                break;
            }
        if (!market)
            continue;
        // A transaction can contain multiple fills. Never deduplicate on transaction hash alone.
        const auto identity = json::array({condition, wallet, tx, asset, side, stamp, size, price}).dump();
        const auto id = "poly-trade-" + stable_id(identity);
        if (!seen.insert(id).second)
            continue;
        s["whaleTrades"].push_back({{"id", id},
                                    {"provider", "polymarket"},
                                    {"marketId", condition},
                                    {"gameId", (*market)["gameId"]},
                                    {"wallet", wallet},
                                    {"transactionHash", tx},
                                    {"asset", asset},
                                    {"team", (*market)["outcomeTokens"][asset]},
                                    {"side", side},
                                    {"shares", size},
                                    {"price", price},
                                    {"notional", size * price},
                                    {"unit", "USDC"},
                                    {"tradedAt", iso_at(static_cast<long long>(stamp))},
                                    {"observedAt", fetched},
                                    {"demo", false}});
    }
    auto &trades = s["whaleTrades"];
    trades.erase(
        std::remove_if(trades.begin(), trades.end(),
                       [&](const json &t) { return parse_time(t.value("tradedAt", "")) < now - 86400; }),
        trades.end());
    std::sort(trades.begin(), trades.end(),
              [](const json &a, const json &b) { return a["tradedAt"] > b["tradedAt"]; });
    if (trades.size() > 1000)
        trades.erase(trades.begin() + 1000, trades.end());
}
void expire_activity(json &a) {
    const auto now = parse_time(now_iso());
    for (auto &m : a["markets"]) {
        const auto age = now - parse_time(m.value("observedAt", ""));
        m["stale"] = age < -30 || age > 300;
    }
    auto &trades = a["trades"];
    trades.erase(std::remove_if(trades.begin(), trades.end(),
                                [&](const json &t) {
                                    const auto age = now - parse_time(t.value("tradedAt", ""));
                                    return age < -30 || age > 86400;
                                }),
                 trades.end());
    const auto age = now - parse_time(a["coverage"].value("observedAt", ""));
    a["coverage"]["stale"] = age < -30 || age > 300;
}
json activity_dashboard(const json &s) {
    json a = {
        {"markets", s.value("exchangeMarkets", json::array())},
        {"trades", s.value("whaleTrades", json::array())},
        {"minimumTradeNotional", 1000},
        {"windowHours", 24},
        {"coverage",
         s.value("whaleCoverage", json{{"state", "unavailable"},
                                       {"observedAt", ""},
                                       {"message", "No verified public trade data has been retrieved. "
                                                   "Missing does not mean zero activity."}})},
        {"notice",
         "Exchange activity, not sportsbook handle or bet counts. USDC volume and Kalshi contracts "
         "are separate units. Large public wallet trades are not proof of informed betting, "
         "net positions, unique people or future results. No private bettor identities are available."}};
    expire_activity(a);
    return a;
}
void seed_demo_activity(json &s) {
    if (s["games"].empty())
        return;
    const auto now = parse_time(now_iso());
    const auto game = s["games"][0];
    s["exchangeMarkets"] = json::array({{{"id", "demo-polymarket"},
                                         {"marketId", "demo"},
                                         {"provider", "polymarket"},
                                         {"gameId", game["id"]},
                                         {"label", "Synthetic game winner"},
                                         {"volume", 842500},
                                         {"volume24h", 126800},
                                         {"liquidity", 38200},
                                         {"openInterest", nullptr},
                                         {"unit", "USDC"},
                                         {"observedAt", now_iso()},
                                         {"demo", true}},
                                        {{"id", "demo-kalshi"},
                                         {"marketId", "demo"},
                                         {"provider", "kalshi"},
                                         {"gameId", game["id"]},
                                         {"label", "Synthetic home win contract"},
                                         {"volume", 216500},
                                         {"volume24h", 52400},
                                         {"liquidity", nullptr},
                                         {"openInterest", 18400},
                                         {"unit", "contracts"},
                                         {"observedAt", now_iso()},
                                         {"demo", true}}});
    s["whaleTrades"] = json::array();
    for (int i = 0; i < 4; ++i)
        s["whaleTrades"].push_back({{"id", "demo-trade-" + std::to_string(i)},
                                    {"provider", "polymarket"},
                                    {"gameId", game["id"]},
                                    {"marketId", "demo"},
                                    {"wallet", "Synthetic wallet " + std::to_string(i % 2 + 1)},
                                    {"team", game[i % 2 ? "away" : "home"]},
                                    {"side", i == 2 ? "SELL" : "BUY"},
                                    {"shares", 20000 + i * 10000},
                                    {"price", 0.55},
                                    {"notional", (20000 + i * 10000) * 0.55},
                                    {"unit", "USDC"},
                                    {"tradedAt", iso_at(now - 60 - i * 240)},
                                    {"observedAt", now_iso()},
                                    {"demo", true}});
    s["whaleCoverage"] = {{"state", "demo"},
                          {"observedAt", now_iso()},
                          {"message", "Synthetic illustration only. No real wallet, trade or volume data."}};
}
} // namespace edgecourt
