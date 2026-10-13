#include "activity.hpp"
#include "feeds.hpp"
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <stdexcept>
using namespace edgecourt;
namespace {
// Integration fixtures must never consume an optional real account's quota.
struct EnvironmentGuard {
    std::map<std::string, std::pair<bool, std::string>> saved;
    static void set(const std::string &key, const char *value) {
#ifdef _WIN32
        _putenv_s(key.c_str(), value ? value : "");
#else
        if (value)
            setenv(key.c_str(), value, 1);
        else
            unsetenv(key.c_str());
#endif
    }
    EnvironmentGuard() {
        for (const auto key : {"API_BASKETBALL_KEY", "SHARPAPI_KEY", "THESPORTSDB_KEY"}) {
            const auto value = std::getenv(key);
            saved[key] = {value != nullptr, value ? value : ""};
            set(key, nullptr);
        }
    }
    ~EnvironmentGuard() {
        for (const auto &item : saved)
            set(item.first, item.second.first ? item.second.second.c_str() : nullptr);
    }
};
void check(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
const std::string condition = "0x" + std::string(64, 'a');
const std::string wallet = "0x" + std::string(40, 'b');
json snapshot() {
    auto s = empty_snapshot("live");
    s["games"].push_back({{"id", "game"},
                          {"home", "BOS"},
                          {"away", "NYK"},
                          {"startsAt", iso_at(parse_time(now_iso()) + 3600)},
                          {"status", "upcoming"}});
    return s;
}
json event(const json &s) {
    return {{"slug", "nba-game"},
            {"markets", json::array({{{"conditionId", condition},
                                      {"active", true},
                                      {"closed", false},
                                      {"sportsMarketType", "moneyline"},
                                      {"outcomes", "[\"Boston Celtics\",\"New York Knicks\"]"},
                                      {"outcomePrices", "[\"0.55\",\"0.45\"]"},
                                      {"clobTokenIds", "[\"111\",\"222\"]"},
                                      {"endDate", s["games"][0]["startsAt"]},
                                      {"volumeNum", "123456.78"},
                                      {"volume24hr", 4567.89},
                                      {"liquidityNum", 321.0},
                                      {"question", "Celtics vs Knicks"}}})}};
}
json trade() {
    return {{"conditionId", condition},
            {"proxyWallet", wallet},
            {"transactionHash", "0x" + std::string(64, 'c')},
            {"asset", "111"},
            {"size", "20000"},
            {"price", "0.55"},
            {"timestamp", parse_time(now_iso()) - 30},
            {"side", "BUY"}};
}
} // namespace
int main() {
    EnvironmentGuard environment;
    const auto dir = std::filesystem::temp_directory_path() /
                     ("edgecourt-activity-" + std::to_string(std::random_device{}()));
    std::filesystem::remove_all(dir);
    try {
        auto s = snapshot();
        auto e = event(s);
        auto t = trade();
        parse_polymarket_events(json::array({e, e}), s, now_iso());
        check(s["exchangeMarkets"].size() == 1 && s["quotes"].size() == 2,
              "Volume once per market, not per outcome/page");
        check(std::abs(number(s["exchangeMarkets"][0]["volume"]) - 123456.78) < 1e-6,
              "Numeric string volume");
        parse_polymarket_trades(json::array({t, t}), s, now_iso());
        check(s["whaleTrades"].size() == 1, "Duplicate fill rows");
        check(number(s["whaleTrades"][0]["notional"]) == 11000, "Shares times price, not share count");
        check(s["whaleTrades"][0]["team"] == "BOS", "Selection comes from validated token, not trade title");
        auto second = t;
        second["side"] = "SELL";
        parse_polymarket_trades(json::array({second}), s, now_iso());
        check(s["whaleTrades"].size() == 2, "Separate fills in same transaction retained");
        for (const auto &field : {"conditionId", "proxyWallet", "transactionHash", "asset", "size", "price",
                                  "timestamp", "side"}) {
            auto bad = t;
            bad[field] = nullptr;
            parse_polymarket_trades(json::array({bad}), s, now_iso());
        }
        for (auto bad : {t, t, t, t, t}) {
            bad["price"] = 1;
            parse_polymarket_trades(json::array({bad}), s, now_iso());
        }
        auto bad = t;
        bad["timestamp"] = parse_time(now_iso()) + 300;
        parse_polymarket_trades(json::array({bad}), s, now_iso());
        bad = t;
        bad["timestamp"] = parse_time(now_iso()) - 90000;
        parse_polymarket_trades(json::array({bad}), s, now_iso());
        bad = t;
        bad["size"] = 100;
        parse_polymarket_trades(json::array({bad}), s, now_iso());
        bad = t;
        bad["asset"] = "333";
        parse_polymarket_trades(json::array({bad}), s, now_iso());
        bad = t;
        bad["conditionId"] = "0x" + std::string(64, 'd');
        parse_polymarket_trades(json::array({bad}), s, now_iso());
        check(s["whaleTrades"].size() == 2, "Malformed, unmatched, small and out-of-window trades rejected");
        auto no_price = snapshot();
        e = event(no_price);
        e["markets"][0]["outcomePrices"] = nullptr;
        e["markets"][0]["volumeNum"] = nullptr;
        e["markets"][0]["volume24hr"] = -1;
        parse_polymarket_events(json::array({e}), no_price, now_iso());
        check(no_price["quotes"].empty() && no_price["exchangeMarkets"].size() == 1,
              "Volume independent of quote availability");
        check(no_price["exchangeMarkets"][0]["volume"].is_null() &&
                  no_price["exchangeMarkets"][0]["volume24h"].is_null(),
              "Unknown volumes are null");
        auto repeat = snapshot();
        auto game = repeat["games"][0];
        game["id"] = "closer";
        repeat["games"][0]["startsAt"] = iso_at(parse_time(now_iso()) + 86400);
        repeat["games"].push_back(game);
        e = event(s);
        parse_polymarket_events(json::array({e}), repeat, now_iso());
        check(repeat["exchangeMarkets"][0]["gameId"] == "closer",
              "Nearest fixture, not first same-team game");
        json kalshi = {{"markets", json::array({{{"status", "active"},
                                                 {"event_ticker", "KXNBAGAME-26OCT13NYKBOS"},
                                                 {"ticker", "KXNBAGAME-26OCT13NYKBOS-BOS"},
                                                 {"occurrence_datetime", s["games"][0]["startsAt"]},
                                                 {"volume_fp", "125.50"},
                                                 {"volume_24h_fp", "20.00"},
                                                 {"open_interest_fp", "50.00"}}})}};
        parse_kalshi_markets(kalshi, s, now_iso());
        check(s["exchangeMarkets"].size() == 2 && s["exchangeMarkets"][1]["unit"] == "contracts",
              "Kalshi contracts are not dollars");
        check(number(s["exchangeMarkets"][1]["volume"]) == 125.5, "Fixed point volume takes precedence");
        auto date_only = snapshot();
        date_only["games"][0]["startsAt"] = "2026-10-14T02:00:00Z"; // Eastern Oct 13
        auto dated = kalshi;
        dated["markets"][0].erase("occurrence_datetime");
        parse_kalshi_markets(dated, date_only, now_iso());
        check(date_only["exchangeMarkets"].size() == 1,
              "Kalshi structured Eastern ticker date works without occurrence time");
        date_only["exchangeMarkets"] = json::array();
        auto duplicate = date_only["games"][0];
        duplicate["id"] = "ambiguous";
        date_only["games"].push_back(duplicate);
        parse_kalshi_markets(dated, date_only, now_iso());
        check(date_only["exchangeMarkets"].empty(), "Ambiguous same-day event rejected");
        auto a = activity_dashboard(s);
        a["markets"][0]["observedAt"] = iso_at(parse_time(now_iso()) - 301);
        a["trades"][0]["tradedAt"] = iso_at(parse_time(now_iso()) - 86401);
        expire_activity(a);
        check(a["markets"][0]["stale"] == true && a["trades"].size() == 1, "Request-time stale/24h expiry");
        check(activity_dashboard(empty_snapshot("live"))["trades"].empty(),
              "Live never substitutes synthetic activity");
        check(build_dashboard(demo_snapshot())["activity"]["trades"].size() == 4,
              "Synthetic demo activity delivered");
        const std::string poly = "https://gamma-api.polymarket.com/events?limit=100";
        int calls = 0;
        PublicFeeds feeds(dir / "cache", [&](const std::string &) {
            ++calls;
            return HttpResponse{200, "[]", ""};
        });
        auto first = feeds.get(poly, 60, 300);
        auto cached = feeds.get(poly, 60, 300);
        check(calls == 1 && cached.cached && first.fetched_at == cached.fetched_at,
              "Cache hit preserves original time");
        check(!allowed_data_url("https://sportsbook-api2.p.rapidapi.com/v0/competitions"),
              "Speculative provider removed");
        check(allowed_data_url("https://data-api.polymarket.com/trades?limit=500") &&
                  !allowed_data_url("https://data-api.polymarket.com.evil.test/trades"),
              "Trade host allowlist");
        feeds.get("https://sportsbook-api2.p.rapidapi.com/v0/competitions", 60);
        check(calls == 1, "Disallowed host blocked before transport");
        int denied = 0;
        PublicFeeds limited(dir / "limited", [&](const std::string &) {
            ++denied;
            return HttpResponse{429, "{}", "rate limited"};
        });
        limited.get(poly, 60);
        limited.get(poly + "&offset=100", 60);
        check(denied == 1, "429 opens host circuit across URLs");
        PublicFeeds restarted(dir / "limited", [&](const std::string &) {
            ++denied;
            return HttpResponse{200, "[]", ""};
        });
        restarted.get(poly + "&offset=200", 60);
        check(denied == 1, "Circuit survives restart");
        int malformed = 0;
        PublicFeeds invalid(dir / "invalid", [&](const std::string &) {
            ++malformed;
            return HttpResponse{200, "{\"error\":\"bad API\"}", ""};
        });
        check(invalid.get(poly, 60).data.is_null(), "HTTP 200 error envelope is not success");
        invalid.get(poly, 60);
        check(malformed == 1, "Schema failure retry deferred");
        check(!valid_feed_payload(poly, json::object()) && valid_feed_payload(poly, json::array()),
              "Empty real collection differs from wrong schema");
        int offline = 0;
        PublicFeeds unavailable(dir / "offline", [&](const std::string &) {
            ++offline;
            return HttpResponse{0, "", "network blocked"};
        });
        unavailable.get(poly, 60);
        unavailable.get(poly + "&offset=100", 60);
        unavailable.get(poly + "&offset=200", 60);
        check(offline == 2, "Repeated transport failures stop same-host fanout");
        // Retry failures compound across attempts rather than being reset by in-flight records.
        const auto retry_file = dir / "invalid" / (stable_id(poly) + ".retry.json");
        json retry;
        {
            std::ifstream file(retry_file);
            file >> retry;
        }
        retry["attemptedAt"] = iso_at(parse_time(now_iso()) - 61);
        {
            std::ofstream file(retry_file);
            file << retry;
        }
        invalid.get(poly, 60);
        {
            std::ifstream file(retry_file);
            file >> retry;
        }
        check(malformed == 2 && retry["retrySeconds"] == 120,
              "Exponential URL backoff survives attempt journaling");
        const std::string straight_url = "https://guest.api.arcadia.pinnacle.com/0.1/leagues/487/markets/straight?brandId=0";
        int empty_calls = 0;
        PublicFeeds no_markets(dir / "no-markets", [&](const std::string &) {
            ++empty_calls;
            return HttpResponse{204, "", ""};
        });
        auto no_prices = no_markets.get(straight_url, 45, 0);
        check(no_prices.status == 204 && no_prices.error.empty() && no_prices.data.is_array() && no_prices.data.empty(),
              "Pinnacle 204 is an empty successful collection, not a JSON or access error");
        check(!std::filesystem::exists(dir / "no-markets" / (stable_id(straight_url) + ".retry.json")),
              "A 204 clears the in-flight failure journal");
        PublicFeeds empty_restarted(dir / "no-markets", [&](const std::string &) {
            ++empty_calls;
            return HttpResponse{200, "[]", ""};
        });
        auto empty_cached = empty_restarted.get(straight_url, 45, 0);
        check(empty_calls == 1 && empty_cached.cached && empty_cached.status == 204 && empty_cached.data.empty(),
              "Empty market response is cached with its status across restart");
        check(no_markets.get(poly, 60).data.is_null(), "204 is not accepted for unrelated JSON contracts");
        for (const int status : {403, 404, 429, 500}) {
            PublicFeeds access_failure(dir / ("pinnacle-failure-" + std::to_string(status)), [&](const std::string &) {
                return HttpResponse{status, "", "fixture failure"};
            });
            const auto failure = access_failure.get(straight_url, 45, 0);
            check(failure.data.is_null() && failure.status == status && !failure.error.empty(),
                  "Actual Pinnacle access/server failures are never disguised as empty coverage");
        }
        // Successful old prices must be replaced, not served stale on a real 204.
        {
            std::ofstream file(dir / "no-markets" / (stable_id(straight_url) + ".json"));
            file << json({{"url", straight_url}, {"fetchedAt", iso_at(parse_time(now_iso()) - 61)},
                          {"body", json::array({{{"key", "old-quote"}}})}});
        }
        check(no_markets.get(straight_url, 45, 0).data.empty(), "204 replaces expired quotes without stale fallback");
        std::string lineup_url;
        int lineup_calls = 0;
        PublicFeeds reported(dir / "reported-errors", [&](const std::string &url) {
            if (url.find("/leaguedashlineups?") != std::string::npos) {
                ++lineup_calls;
                lineup_url = url;
                for (const auto *parameter : {"Month=0", "OpponentTeamID=0", "PaceAdjust=N", "Period=0",
                                              "PlusMinus=N", "Rank=N", "DateFrom=&", "DateTo=&", "TeamID=&",
                                              "ShotClockRange=&", "VsConference=&", "VsDivision=&"})
                    check(url.find(parameter) != std::string::npos, "Lineup request includes NBA endpoint defaults");
                return HttpResponse{500, "", "Public source returned HTTP 500"};
            }
            if (url.find("/sports/4/leagues") != std::string::npos)
                return HttpResponse{200, "[{\"id\":487,\"name\":\"NBA\"}]", ""};
            if (url == straight_url)
                return HttpResponse{204, "", ""};
            if (url.find("/matchups?brandId=0") != std::string::npos)
                return HttpResponse{200, "[]", ""};
            return HttpResponse{200, "{\"resultSets\":[]}", ""};
        });
        auto report = reported.refresh(empty_snapshot("live"));
        check(lineup_calls == 1 && !lineup_url.empty(), "Refresh exercises the lineup endpoint once");
        bool saw_empty = false;
        for (const auto &entry : report["sources"])
            if (entry["id"] == "pinnacle")
                saw_empty = entry["state"] == "empty" && entry["count"] == 0 &&
                            entry.value("message", "").find("HTTP 204") != std::string::npos;
        check(saw_empty && report["quotes"].empty(), "Pinnacle source card labels no-content accurately and creates no prices");
        const auto lineup_retry = dir / "reported-errors" / (stable_id(lineup_url) + ".retry.json");
        json lineup_failure;
        {
            std::ifstream file(lineup_retry);
            file >> lineup_failure;
        }
        check(lineup_failure["retrySeconds"] == 21600, "Lineup 500 defers retries for six hours");
        // Simulate five minutes passing without sleeping, then restart the client.
        lineup_failure["attemptedAt"] = iso_at(parse_time(now_iso()) - 300);
        {
            std::ofstream file(lineup_retry);
            file << lineup_failure;
        }
        PublicFeeds lineup_restarted(dir / "reported-errors", [&](const std::string &) {
            ++lineup_calls;
            return HttpResponse{200, "{\"resultSets\":[]}", ""};
        });
        const auto deferred = lineup_restarted.get(lineup_url, 21600, 172800, 21600);
        check(lineup_calls == 1 && deferred.cached && deferred.status == 500 && deferred.data.is_null(),
              "Lineup failure backoff persists across restart without inventing data");
        int budget_calls = 0;
        PublicFeeds budget(dir / "budget", [&](const std::string &) {
            ++budget_calls;
            return HttpResponse{200, "[]", ""};
        });
        for (int page = 0; page < 80; ++page)
            budget.get(poly + "&offset=" + std::to_string(page), 60);
        check(budget_calls == 72, "Shared request ceiling is enforced even for a healthy provider");
        // Stale fallback retains the original observation, never stamps it as now.
        auto path = dir / "cache" / (stable_id(poly) + ".json");
        json old;
        {
            std::ifstream f(path);
            f >> old;
        }
        old["fetchedAt"] = iso_at(parse_time(now_iso()) - 120);
        {
            std::ofstream f(path);
            f << old;
        }
        PublicFeeds stale(dir / "cache",
                          [](const std::string &) { return HttpResponse{503, "", "unavailable"}; });
        auto fallback = stale.get(poly, 60, 300);
        check(fallback.stale && fallback.fetched_at == old["fetchedAt"], "Stale cache preserves time");
        auto next = stale.get(poly, 60, 300);
        check(next.stale && next.fetched_at == fallback.fetched_at, "Retry respects stale cache age");
        // Exercise the actual refresh pipeline with bounded, deterministic provider replies.
        int gamma_pages = 0, data_pages = 0, kalshi_pages = 0, total_calls = 0;
        auto joined = snapshot();
        auto espn =
            json{{"events",
                  json::array(
                      {{{"id", "game"},
                        {"date", joined["games"][0]["startsAt"]},
                        {"competitions",
                         json::array(
                             {{{"competitors",
                                json::array(
                                    {{{"homeAway", "home"}, {"team", {{"abbreviation", "BOS"}, {"id", "2"}}}},
                                     {{"homeAway", "away"},
                                      {"team", {{"abbreviation", "NYK"}, {"id", "18"}}}}})}}})}}})}};
        PublicFeeds pipeline(dir / "pipeline", [&](const std::string &url) {
            ++total_calls;
            if (url.find("sportsbook-api2") != std::string::npos ||
                url.find("sportscore.com") != std::string::npos)
                throw std::runtime_error("Removed provider was requested");
            json body;
            if (url.find("site.api.espn.com") != std::string::npos) {
                if (url.find("scoreboard") != std::string::npos)
                    body = espn;
                else
                    body = {{"athletes", json::array()}};
            } else if (url.find("gamma-api") != std::string::npos) {
                ++gamma_pages;
                body = json::array();
                if (url.find("offset=0") != std::string::npos) {
                    body.push_back(event(joined));
                    while (body.size() < 100)
                        body.push_back(json::object());
                } else
                    body.push_back(event(joined));
            } else if (url.find("data-api.polymarket") != std::string::npos) {
                ++data_pages;
                body = json::array({trade(), trade()});
            } else if (url.find("external-api.kalshi") != std::string::npos) {
                ++kalshi_pages;
                body = {{"markets", json::array()}, {"cursor", "repeat"}};
            } else
                return HttpResponse{403, "{}", "test unavailable"};
            return HttpResponse{200, body.dump(), ""};
        });
        auto refreshed = pipeline.refresh(empty_snapshot("live"));
        check(gamma_pages == 2 && data_pages == 1 && kalshi_pages == 2,
              "Bounded pagination and repeated cursor stop");
        check(refreshed["exchangeMarkets"].size() == 1 && refreshed["whaleTrades"].size() == 1,
              "End-to-end joins and deduplication");
        check(refreshed["whaleCoverage"]["state"] == "sampled", "Successful bounded trade sample");
        const auto count_before = total_calls;
        refreshed = pipeline.refresh(refreshed);
        check(total_calls == count_before, "Immediate refresh is entirely cached/backed off");
        check(refreshed["whaleTrades"].size() == 1, "Cross-refresh trade retention does not double-count");
        auto malformed_scoreboard = espn;
        auto broken_event = espn["events"][0];
        broken_event["status"] = nullptr;
        malformed_scoreboard["events"].push_back(broken_event);
        PublicFeeds atomic(dir / "atomic", [&](const std::string &url) {
            if (url.find("site.api.espn.com") != std::string::npos)
                return HttpResponse{200, malformed_scoreboard.dump(), ""};
            return HttpResponse{403, "{}", "fixture denial"};
        });
        auto rejected = atomic.refresh(empty_snapshot("live"));
        check(rejected["games"].empty(), "Parse exception rolls back earlier mutations from the same page");
        auto synthetic = demo_snapshot();
        refreshed = pipeline.refresh(synthetic);
        for (const auto &fill : refreshed["whaleTrades"])
            check(!fill.value("demo", false), "No demo whales enter live pipeline");
        auto model_before = demo_snapshot(), model_after = model_before;
        parse_polymarket_trades(json::array(), model_after, now_iso());
        check(evaluate_quote(model_before["quotes"][0], model_before) ==
                  evaluate_quote(model_before["quotes"][0], model_after),
              "Activity never changes model probability or EV");
        std::cout << "PASS activity: units, no double counting, malformed fills, identity, 24h expiry, "
                     "isolation, cache, schema, host circuits and credential removal\n";
    } catch (const std::exception &e) {
        std::cerr << "FAIL activity: " << e.what() << "\n";
        std::filesystem::remove_all(dir);
        return 1;
    }
    std::filesystem::remove_all(dir);
    return 0;
}
