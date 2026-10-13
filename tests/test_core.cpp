#include "core.hpp"
#include "feeds.hpp"
#include "history.hpp"
#include "history_download.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <set>
#include <stdexcept>

using namespace edgecourt;
namespace {
int passed = 0, failed = 0;
void check(bool condition, const std::string &message) {
    if (!condition)
        throw std::runtime_error(message);
}
void near(double a, double b, double epsilon = 1e-8) {
    check(std::abs(a - b) < epsilon, "Expected " + std::to_string(b) + ", got " + std::to_string(a));
}
void throws(const std::function<void()> &function) {
    bool did_throw = false;
    try {
        function();
    } catch (const std::exception &) {
        did_throw = true;
    }
    check(did_throw, "Expected an exception");
}
void test(const std::string &name, const std::function<void()> &function) {
    try {
        function();
        ++passed;
        std::cout << "PASS  " << name << "\n";
    } catch (const std::exception &error) {
        ++failed;
        std::cerr << "FAIL  " << name << ": " << error.what() << "\n";
    }
}
json find_quote(const json &s, const std::string &market, const std::string &player = "",
                const std::string &side = "over") {
    for (const auto &q : s["quotes"])
        if (q.value("market", "") == market && (player.empty() || q.value("player", "") == player) &&
            q.value("side", "") == side)
            return q;
    throw std::runtime_error("Test quote missing");
}
json espn_fixture() {
    return json::parse(
        R"({"events":[{"id":"401-test","date":"2026-11-05T01:00Z","season":{"type":2},"status":{"type":{"state":"pre","name":"STATUS_SCHEDULED"},"period":0,"displayClock":"0:00"},"competitions":[{"competitors":[{"homeAway":"home","score":"0","team":{"abbreviation":"GS","displayName":"Golden State Warriors","id":"9"}},{"homeAway":"away","score":"0","team":{"abbreviation":"NY","displayName":"New York Knicks","id":"18"}}],"odds":[{"provider":{"name":"Fixture Book"},"homeTeamOdds":{"moneyLine":-125},"awayTeamOdds":{"moneyLine":110}}]}]}]})");
}
} // namespace
int main() {
    const auto demo = demo_snapshot();
    test("American odds conversion", [] {
        near(decimal_odds(-110), 1 + 100.0 / 110);
        near(decimal_odds(150), 2.5);
        near(implied_probability(-200), 2.0 / 3);
        throws([] { decimal_odds(0); });
        throws([] { decimal_odds(-99); });
    });
    test("EV handles pushes and losses", [] {
        near(expected_value(.55, 0, -110), 5);
        near(expected_value(.5, .1, 100), 10);
        near(expected_value(0, 1, 100), 0);
        throws([] { expected_value(.9, .2, 100); });
    });
    test("Fractional Kelly uses conditional resolved outcomes and risk cap", [] {
        near(fractional_kelly(.55, 0, -110), .01375);
        near(fractional_kelly(.5, .05, -110), .00131578947368);
        near(fractional_kelly(.99, 0, 200), .02);
        near(fractional_kelly(.1, 0, -110), 0);
    });
    test("Sampling interval uses effective history, not Monte Carlo draws", [] {
        auto band = wilson_interval(.6, 20);
        check(number(band["lower"]) < .4 && number(band["upper"]) > .75,
              "Interval is unrealistically narrow");
    });
    test("UTC timestamp validation rejects normalized invalid dates", [] {
        check(parse_time("2026-10-12T20:00:00Z") > 0, "UTC parse");
        check(parse_time("2026-02-31T20:00:00Z") == 0, "Invalid February date");
        check(parse_time("2026-10-32") == 0, "Invalid day");
        check(parse_time("2026-10-12T20:00:00-06:00") == 0, "Offsets intentionally disallowed");
        check(parse_time(now_iso()) > 0, "Current UTC");
    });
    test("Unicode name matching", [] {
        check(normalize_name("Nikola Jokić") == normalize_name("Nikola Jokic"), "Jokic crosswalk");
        check(normalize_name("Luka Dončić") == "lukadoncic", "Doncic crosswalk");
        check(normalize_name("Jonas Valančiūnas") == "jonasvalanciunas", "Lithuanian crosswalk");
    });
    test("Season boundary", [] {
        check(nba_season(parse_time("2026-09-30T12:00:00Z")) == "2025-26", "September");
        check(nba_season(parse_time("2026-10-01T12:00:00Z")) == "2026-27", "October");
    });
    test("Demo covers every requested market and is labeled synthetic", [&] {
        std::set<std::string> covered;
        for (const auto &q : demo["quotes"]) {
            covered.insert(q["market"]);
            check(q["demo"] == true && q["source"] == "synthetic", "Demo provenance");
        }
        check(covered.size() == 10, "Market coverage");
        check(demo["mode"] == "demo", "Demo mode");
    });
    test("Bootstrap produces reproducible probabilities and effective sample sizes", [&] {
        auto q = find_quote(demo, "points", "Stephen Curry");
        auto r = evaluate_quote(q, demo), again = evaluate_quote(q, demo);
        check(r["availability"] == "ready", "Projection missing");
        near(number(r["probability"]), number(again["probability"]));
        check(number(r["effectiveSampleSize"]) < 45, "Effective N should be smaller than raw N");
        check(r["sampleSize"] == 45, "Historical count");
    });
    test("Integer prop lines partition win/loss/push", [&] {
        auto q = find_quote(demo, "points", "Stephen Curry");
        q["line"] = 26;
        auto over = evaluate_quote(q, demo);
        q["side"] = "under";
        auto under = evaluate_quote(q, demo);
        near(number(over["probability"]) + number(under["probability"]) + number(over["pushProbability"]), 1);
        check(number(over["pushProbability"]) > 0, "Push mass absent");
    });
    test("Half-point lines cannot push", [&] {
        auto r = evaluate_quote(find_quote(demo, "rebounds", "Karl-Anthony Towns"), demo);
        near(number(r["pushProbability"]), 0);
    });
    test("Double-double uses all five settlement categories", [&] {
        auto s = demo;
        auto q = find_quote(s, "double_double", "Stephen Curry", "yes");
        s["players"][normalize_name("Stephen Curry")]["logs"][0].erase("blocks");
        auto r = evaluate_quote(q, s);
        check(r["probability"].is_null() && r.value("reason", "").find("steals") != std::string::npos,
              "Missing fifth category must block");
    });
    test("First-basket field is normalized across ten confirmed starters", [&] {
        double total = 0;
        for (const auto &item : demo["players"].items()) {
            const auto &p = item.value();
            if (p.value("team", "") != "LAL" && p.value("team", "") != "GSW")
                continue;
            auto q = find_quote(demo, "first_basket", "Stephen Curry", "yes");
            q["player"] = p["name"];
            q["team"] = p["team"];
            total += number(evaluate_quote(q, demo)["probability"]);
        }
        near(total, 1);
        auto s = demo;
        s["players"][normalize_name("Moses Moody")]["starterConfirmed"] = false;
        check(
            evaluate_quote(find_quote(s, "first_basket", "Stephen Curry", "yes"), s)["probability"].is_null(),
            "Incomplete lineup should block");
    });
    test("First-basket contradictory out starter is rejected", [&] {
        auto s = demo;
        s["players"][normalize_name("Moses Moody")]["injury"] = "Out";
        check(
            evaluate_quote(find_quote(s, "first_basket", "Stephen Curry", "yes"), s)["probability"].is_null(),
            "Out starter field");
    });
    test("Live player projections require fresh accrued stats", [&] {
        auto s = demo;
        auto q = find_quote(s, "points", "Nikola Jokić");
        check(!evaluate_quote(q, s)["probability"].is_null(), "Fresh live quote");
        s["players"][normalize_name("Nikola Jokić")].erase("live");
        check(evaluate_quote(q, s)["probability"].is_null(), "No pregame extrapolation in play");
    });
    test("Stale quotes disable EV, but retain explicitly stale projections", [&] {
        auto q = find_quote(demo, "points", "Stephen Curry");
        q["observedAt"] = iso_at(parse_time(now_iso()) - 901);
        auto r = evaluate_quote(q, demo);
        check(r["ev"].is_null() && !r["probability"].is_null() && r["eligible"] == false,
              "Stale price guard");
    });
    test("Future and suspended quotes cannot claim EV", [&] {
        auto q = find_quote(demo, "points", "Stephen Curry");
        q["observedAt"] = iso_at(parse_time(now_iso()) + 300);
        check(evaluate_quote(q, demo)["ev"].is_null(), "Future date");
        q["observedAt"] = now_iso();
        q["status"] = "suspended";
        check(evaluate_quote(q, demo)["probability"].is_null(), "Suspended market");
    });
    test("Preseason and reported-out players suppress baseline", [&] {
        auto s = demo;
        auto q = find_quote(s, "points", "Stephen Curry");
        s["games"][1]["seasonType"] = "preseason";
        check(evaluate_quote(q, s)["probability"].is_null(), "Preseason guard");
        s = demo;
        s["players"][normalize_name("Stephen Curry")]["injury"] = "Out";
        check(evaluate_quote(q, s)["probability"].is_null(), "Injury guard");
    });
    test("Future player logs are excluded (leakage guard)", [&] {
        auto s = demo;
        auto q = find_quote(s, "points", "Stephen Curry");
        for (auto &log : s["players"][normalize_name("Stephen Curry")]["logs"])
            log["date"] = iso_at(parse_time(now_iso()) + 20 * 86400).substr(0, 10);
        check(evaluate_quote(q, s)["probability"].is_null(), "Future logs");
    });
    test("Discrete team totals have push mass", [&] {
        auto q = find_quote(demo, "team_total");
        q["line"] = 114;
        auto over = evaluate_quote(q, demo);
        q["side"] = "under";
        auto under = evaluate_quote(q, demo);
        near(number(over["probability"]) + number(under["probability"]) + number(over["pushProbability"]), 1);
    });
    test("Moneyline probabilities partition both teams", [&] {
        auto home = find_quote(demo, "moneyline", "", "home"),
             away = find_quote(demo, "moneyline", "", "away");
        near(number(evaluate_quote(home, demo)["probability"]) +
                 number(evaluate_quote(away, demo)["probability"]),
             1);
    });
    test("Dashboard no-vig uses matching book and opposite side", [&] {
        auto s = demo;
        s["quotes"] = json::array({find_quote(s, "points", "Stephen Curry", "over"),
                                   find_quote(s, "points", "Stephen Curry", "under")});
        auto d = build_dashboard(s);
        near(number(d["markets"][0]["fairProbability"]), .5);
        s["quotes"][1]["book"] = "Different book";
        d = build_dashboard(s);
        check(d["markets"][0]["fairProbability"].is_null(), "Must not cross books");
    });
    test("Request-time expiry removes cached stale EV without rerunning the model", [&] {
        auto s = demo;
        s["quotes"] = json::array({find_quote(s, "points", "Stephen Curry")});
        auto d = build_dashboard(s);
        d["markets"][0]["observedAt"] = iso_at(parse_time(now_iso()) - 1000);
        d = expire_dashboard(d);
        check(d["markets"][0]["ev"].is_null() && d["summary"]["priced"] == 0, "Cache expiry guard");
    });
    test("API-Basketball daily pregame odds keep a 24-hour freshness window", [&] {
        auto s = demo;
        auto quote = find_quote(s, "moneyline", "", "home");
        quote["source"] = "api_basketball";
        quote["observedAt"] = iso_at(parse_time(now_iso()) - 16 * 60);
        check(evaluate_quote(quote, s)["freshness"] == "fresh",
              "Daily provider odds remain fresh after the generic 15-minute window");
        quote["observedAt"] = iso_at(parse_time(now_iso()) - 24 * 60 * 60 - 1);
        check(evaluate_quote(quote, s)["freshness"] == "stale", "Daily snapshot expires after 24 hours");
    });
    test("Request-time expiry blocks stale live scores and missing game-state transitions", [&] {
        auto s = demo;
        s["quotes"] = json::array({find_quote(s, "points", "Nikola Jokić")});
        auto d = build_dashboard(s);
        d["markets"][0]["game"]["dataAsOf"] = iso_at(parse_time(now_iso()) - 100);
        d = expire_dashboard(d);
        check(d["markets"][0]["probability"].is_null(), "Live cached score expiry");
    });
    test("No odds means no fabricated EV", [&] {
        auto s = empty_snapshot("live");
        s["games"] = demo["games"];
        auto d = build_dashboard(s);
        check(d["summary"]["positiveEv"] == 0 && d["summary"]["quotes"] == 0, "Missing odds summary");
        for (const auto &m : d["markets"])
            check(m["ev"].is_null() && m["americanOdds"].is_null(), "Missing price placeholder");
    });
    test("JSON import validates every quote before merge", [&] {
        auto q = find_quote(demo, "points", "Stephen Curry");
        auto good = validate_import({{"quotes", json::array({q})}});
        check(good["quotes"][0]["source"] == "user_import", "Import provenance");
        q["americanOdds"] = 0;
        throws([&] { validate_import({{"quotes", json::array({q})}}); });
        q = find_quote(demo, "points", "Stephen Curry");
        q["side"] = "yes";
        throws([&] { validate_import({{"quotes", json::array({q})}}); });
    });
    test("CSV RFC4180 quoted commas, escaped quotes and malformed numbers", [] {
        auto csv = "gameId,market,player,team,side,line,americanOdds,book,observedAt\r\ng,points,Stephen "
                   "Curry,GSW,over,24.5,-110,\"Book, \"\"A\"\"\"," +
                   now_iso() + "\r\n";
        auto data = parse_csv_import(csv);
        check(data["quotes"][0]["book"] == "Book, \"A\"", "CSV escaping");
        throws([] { parse_csv_import("gameId,market\nx,points"); });
        throws([] { parse_csv_import("\"unclosed"); });
    });
    test("Duplicate player log dates and future quote dates are rejected", [&] {
        auto p = demo["players"][normalize_name("Stephen Curry")];
        p["logs"][1]["date"] = p["logs"][0]["date"];
        throws([&] { validate_import({{"players", json::array({p})}}); });
        auto q = find_quote(demo, "points", "Stephen Curry");
        q["observedAt"] = iso_at(parse_time(now_iso()) + 600);
        throws([&] { validate_import({{"quotes", json::array({q})}}); });
    });
    test("Quote deduplication is idempotent", [&] {
        auto s = demo;
        auto q = validate_import({{"quotes", json::array({find_quote(s, "points", "Stephen Curry")})}});
        merge_import(s, q);
        auto count = s["quotes"].size();
        merge_import(s, q);
        check(s["quotes"].size() == count, "Duplicate import");
    });
    test("Data URL allowlist rejects local hosts, credentials, redirects and ports", [] {
        check(allowed_data_url("https://stats.nba.com/stats/x?Season=2026-27"), "NBA allowlist");
        check(!allowed_data_url("http://127.0.0.1"), "Local HTTP");
        check(!allowed_data_url("https://stats.nba.com@evil.test/x"), "Credentials trick");
        check(!allowed_data_url("https://stats.nba.com:443/x"), "Explicit port");
        check(!allowed_data_url("https://stats.nba.com/x\nInjected"), "Header injection");
    });
    test("NBA tabular schema mapping skips malformed rows", [] {
        auto rows = nba_rows(
            {{"resultSets", json::array({{{"headers", {"PTS", "MIN"}}, {"rowSet", {{24, 32}, {1}}}}})}});
        check(rows.size() == 1 && rows[0]["PTS"] == 24, "Tabular parser");
    });
    test("ESPN schedule, team alias and explicit prices", [] {
        auto s = empty_snapshot("live");
        parse_espn_scoreboard(espn_fixture(), s, now_iso());
        check(s["games"].size() == 1 && s["games"][0]["home"] == "GSW" && s["games"][0]["away"] == "NYK",
              "ESPN schedule");
        check(s["quotes"].size() == 2 && s["quotes"][0]["americanOdds"] == -125, "Explicit moneyline");
        check(s["games"][0]["startsAt"] == "2026-11-05T01:00:00Z", "Minute precision UTC");
    });
    test("ESPN new moneyline schema and absent odds", [] {
        auto f = espn_fixture();
        auto &entry = f["events"][0]["competitions"][0]["odds"][0];
        entry.erase("homeTeamOdds");
        entry.erase("awayTeamOdds");
        entry["moneyline"] = {{"home", {{"close", {{"odds", "-130"}}}}},
                              {"away", {{"close", {{"odds", "+115"}}}}}};
        auto s = empty_snapshot("live");
        parse_espn_scoreboard(f, s, now_iso());
        check(s["quotes"].size() == 2 && s["quotes"][1]["americanOdds"] == 115, "New schema");
        f["events"][0]["competitions"][0].erase("odds");
        s = empty_snapshot("live");
        parse_espn_scoreboard(f, s, now_iso());
        check(s["quotes"].empty(), "No inferred odds");
    });
    test("NBA scoreboard merges into ESPN event without duplication", [] {
        auto s = empty_snapshot("live");
        parse_espn_scoreboard(espn_fixture(), s, now_iso());
        json fixture = {
            {"scoreboard",
             {{"games", json::array({{{"gameId", "0022600012"},
                                      {"gameTimeUTC", "2026-11-05T01:00:00Z"},
                                      {"gameStatus", 2},
                                      {"period", 3},
                                      {"gameClock", "PT08M42.00S"},
                                      {"homeTeam", {{"teamTricode", "GSW"}, {"score", 65}}},
                                      {"awayTeam", {{"teamTricode", "NYK"}, {"score", 62}}}}})}}}};
        parse_nba_scoreboard(fixture, s, now_iso());
        check(s["games"].size() == 1 && s["games"][0]["id"] == "401-test", "Dedup event");
        near(number(s["games"][0]["elapsedMinutes"]), 27.3);
    });
    test("ESPN live player box-score mapping includes made threes and fractional minutes", [] {
        auto s = empty_snapshot("live");
        auto g = json{{"id", "g"}, {"home", "GSW"}, {"away", "NYK"}};
        json doc = {
            {"boxscore",
             {{"players",
               json::array(
                   {{{"team", {{"abbreviation", "GS"}}},
                     {"statistics",
                      json::array(
                          {{{"labels", {"MIN", "PTS", "REB", "AST", "STL", "BLK", "3PT"}},
                            {"athletes",
                             json::array(
                                 {{{"athlete", {{"displayName", "Stephen Curry"}}},
                                   {"starter", true},
                                   {"stats", {"21:30", "19", "3", "5", "1", "0", "4-8"}}}})}}})}}})}}}};
        parse_espn_summary(doc, s, g, now_iso());
        auto live = s["players"][normalize_name("Stephen Curry")]["live"];
        near(number(live["minutes"]), 21.5);
        check(live["threes"] == 4, "Made threes");
    });
    test("Saved HTML extracts normalized JSON without executing scripts", [&] {
        auto q = find_quote(demo, "points", "Stephen Curry");
        std::string html = "<script type=\"application/json\" id=\"edgecourt-data\">" +
                           json({{"quotes", json::array({q})}}).dump() +
                           "</script><script>throw Error('not executed')</script>";
        auto out = scrape_saved_html(html);
        check(out["quotes"].size() == 1, "Embedded data");
    });
    test("Saved Basketball Reference game-log HTML uses data-stat fields", [] {
        std::string html =
            "<table><tr><th data-stat=\"date_game\"><a>2026-10-01</a></th><td data-stat=\"mp\">34:30</td><td "
            "data-stat=\"pts\">24</td><td data-stat=\"trb\">5</td><td data-stat=\"ast\">6</td><td "
            "data-stat=\"fg3\">4</td><td data-stat=\"stl\">1</td><td data-stat=\"blk\">0</td></tr></table>";
        auto out = scrape_saved_html(html, "Stephen Curry", "GSW");
        near(number(out["players"]["stephencurry"]["logs"][0]["minutes"]), 34.5);
        throws([&] { scrape_saved_html(html); });
    });
    test("Backtest metrics and anti-leakage timestamp guard", [] {
        json rows = {{"observations", json::array({{{"probability", .6},
                                                    {"americanOdds", 100},
                                                    {"predictedAt", "2026-10-01T00:00:00Z"},
                                                    {"startsAt", "2026-10-01T01:00:00Z"},
                                                    {"result", "win"}},
                                                   {{"probability", .4},
                                                    {"americanOdds", 100},
                                                    {"predictedAt", "2026-10-01T00:00:00Z"},
                                                    {"startsAt", "2026-10-01T01:00:00Z"},
                                                    {"result", "loss"}},
                                                   {{"probability", .6},
                                                    {"americanOdds", 100},
                                                    {"predictedAt", "2026-10-01T00:00:00Z"},
                                                    {"startsAt", "2026-10-01T01:00:00Z"},
                                                    {"result", "push"}}})}};
        auto score = score_backtest(rows);
        near(number(score["brier"]), .16);
        check(score["selectedBets"] == 2 && score["pushes"] == 1, "Backtest selection");
        near(number(score["roiPercent"]), 50);
        check(score["marketComparisonStatus"] == "not_provided" && score["marketComparison"].is_null(),
              "Unbenchmarked report must not imply market outperformance");
        rows["observations"][0]["predictedAt"] = "2026-10-01T02:00:00Z";
        throws([&] { score_backtest(rows); });
    });
    test("Backtest scores all predictions against closing no-vig market", [] {
        auto observation = [](const std::string &game, double model, double market, const std::string &result,
                              double push = 0) {
            return json{{"gameId", game},
                        {"market", "points"},
                        {"probability", model},
                        {"pushProbability", push},
                        {"oddsPercent", 50},
                        {"predictedAt", "2026-10-01T00:00:00Z"},
                        {"closingAt", "2026-10-01T01:30:00Z"},
                        {"closingNoVigProbability", market},
                        {"startsAt", "2026-10-01T02:00:00Z"},
                        {"result", result}};
        };
        json doc = {{"observations", json::array({observation("g1", .8, .7, "win"),
                                                   observation("g2", .4, .6, "loss"),
                                                   observation("g3", .45, .5, "push", .1)})}};
        auto report = score_backtest(doc);
        const auto comparison = report["marketComparison"]["overall"];
        check(report["marketComparisonStatus"] == "scored", "Market comparison missing");
        check(report["marketComparison"]["matchedObservations"] == 3, "Matched sample size");
        check(comparison["resolved"] == 2 && comparison["pushesExcluded"] == 1, "Push scoring");
        near(number(comparison["modelBrier"]), .1);
        near(number(comparison["closingMarketBrier"]), .225);
        near(number(comparison["brierDifferenceModelMinusMarket"]), -.125);
        near(number(comparison["brierSkillScore"]), .125 / .225);
        check(comparison["brierDifference95CI"].is_null() &&
                  comparison["brierIntervalResult"] == "not_estimable_fewer_than_30_resolved_game_clusters",
              "Small samples must not claim an uncertainty interval");
        check(comparison["minimumGameClustersForInterval"] == 30, "Minimum cluster guard missing");
        check(report["marketComparison"]["byMarket"]["points"]["resolved"] == 2,
              "Per-market score missing");
    });
    test("Market score uncertainty requires 30 resolved game clusters", [] {
        json observations = json::array();
        for (int i = 0; i < 30; ++i) {
            const bool win = i % 2 == 0;
            observations.push_back({{"gameId", "game-" + std::to_string(i)},
                                    {"market", "points"},
                                    {"probability", win ? .55 : .45},
                                    {"oddsPercent", 50},
                                    {"predictedAt", "2026-10-01T00:00:00Z"},
                                    {"closingAt", "2026-10-01T01:30:00Z"},
                                    {"closingNoVigProbability", .5},
                                    {"startsAt", "2026-10-01T02:00:00Z"},
                                    {"result", win ? "win" : "loss"}});
        }
        const auto report = score_backtest({{"observations", observations}});
        const auto comparison = report["marketComparison"]["overall"];
        check(comparison["gameClusters"] == 30 && !comparison["brierDifference95CI"].is_null(),
              "Thirty resolved games should enable the approximate interval");
    });
    test("Backtest derives no-vig closing probability from both quoted sides", [] {
        json row = {{"gameId", "g1"},
                    {"market", "points"},
                    {"probability", .6},
                    {"oddsPercent", 50},
                    {"predictedAt", "2026-10-01T00:00:00Z"},
                    {"closingAt", "2026-10-01T01:30:00Z"},
                    {"closingOddsPercent", 55},
                    {"closingOppositeOddsPercent", 50},
                    {"startsAt", "2026-10-01T02:00:00Z"},
                    {"result", "win"}};
        auto report = score_backtest({{"observations", json::array({row})}});
        const double fair = 55.0 / 105.0;
        near(number(report["marketComparison"]["overall"]["closingMarketBrier"]),
             (1 - fair) * (1 - fair));
        check(report["marketComparison"]["overall"]["brierDifference95CI"].is_null(),
              "A single game cannot estimate between-game uncertainty");
    });
    test("Market benchmark rejects partial samples, incomplete pairs and closing-time leakage", [] {
        json first = {{"gameId", "g1"},
                      {"probability", .6},
                      {"oddsPercent", 50},
                      {"predictedAt", "2026-10-01T00:00:00Z"},
                      {"closingAt", "2026-10-01T01:30:00Z"},
                      {"closingNoVigProbability", .55},
                      {"startsAt", "2026-10-01T02:00:00Z"},
                      {"result", "win"}};
        json second = first;
        second["gameId"] = "g2";
        json partial = {{"observations", json::array({first, second})}};
        partial["observations"][1].erase("closingNoVigProbability");
        throws([&] { score_backtest(partial); });

        auto late = first;
        late["closingAt"] = "2026-10-01T02:00:00Z";
        throws([&] { score_backtest({{"observations", json::array({late})}}); });

        auto incomplete_pair = first;
        incomplete_pair.erase("closingNoVigProbability");
        incomplete_pair["closingOddsPercent"] = 55;
        throws([&] { score_backtest({{"observations", json::array({incomplete_pair})}}); });

        auto no_game = first;
        no_game.erase("gameId");
        throws([&] { score_backtest({{"observations", json::array({no_game})}}); });
    });
    test("NBA player log parser preserves roster crosswalk and maps raw stats", [] {
        auto s = empty_snapshot("live");
        s["players"]["stephencurry"] = {{"name", "Stephen Curry"},
                                        {"team", "GSW"},
                                        {"injury", "Questionable"},
                                        {"rosterAsOf", now_iso()}};
        json doc = {
            {"resultSets", json::array({{{"headers",
                                          {"PLAYER_NAME", "PLAYER_ID", "TEAM_ABBREVIATION", "GAME_DATE",
                                           "MIN", "PTS", "REB", "AST", "FG3M", "STL", "BLK", "MATCHUP"}},
                                         {"rowSet",
                                          {{"Stephen Curry", 201939, "GSW", "2026-10-01T00:00:00", 34.5, 27,
                                            4, 6, 5, 1, 0, "GSW vs. LAL"}}}}})}};
        parse_player_logs(doc, s, now_iso());
        auto p = s["players"]["stephencurry"];
        check(p["injury"] == "Questionable" && p["logs"].size() == 1, "Roster context lost");
        check(p["logs"][0]["threes"] == 5 && p["logs"][0]["blocks"] == 0, "Stat mapping");
    });
    test("Paired team logs provide chronological Elo, margin and rest", [] {
        auto s = empty_snapshot("live");
        auto date = iso_at(parse_time(now_iso()) - 3 * 86400).substr(0, 10);
        json doc = {
            {"resultSets",
             json::array({{{"headers", {"TEAM_ABBREVIATION", "GAME_ID", "GAME_DATE", "PTS", "MATCHUP"}},
                           {"rowSet",
                            {{"GSW", "002-test", date, 115, "GSW vs. LAL"},
                             {"LAL", "002-test", date, 108, "LAL @ GSW"}}}}})}};
        parse_team_logs(doc, s, now_iso());
        check(number(s["teams"]["GSW"]["elo"]) > 1500 && number(s["teams"]["LAL"]["elo"]) < 1500,
              "Elo result ordering");
        check(s["teams"]["GSW"]["logs"][0]["margin"] == 7, "Margin mapping");
    });
    test("Stale team pace environments cannot produce live-data prop EV", [&] {
        auto s = demo;
        s["mode"] = "live";
        s["teams"]["GSW"]["dataAsOf"] = iso_at(parse_time(now_iso()) - 172801);
        check(evaluate_quote(find_quote(s, "points", "Stephen Curry"), s)["probability"].is_null(),
              "Stale pace guard");
    });
    test("Synthetic examples are explicitly identified at validation", [&] {
        auto q = find_quote(demo, "points", "Stephen Curry");
        auto out = validate_import({{"synthetic", true}, {"quotes", json::array({q})}});
        check(out.value("demoOnly", false), "Synthetic import marker lost");
    });
    test("Backtest optional push mass affects selection and conditional scoring", [] {
        json doc = {{"observations", json::array({{{"probability", .5},
                                                   {"pushProbability", .1},
                                                   {"americanOdds", 100},
                                                   {"predictedAt", "2026-10-01T00:00:00Z"},
                                                   {"startsAt", "2026-10-01T01:00:00Z"},
                                                   {"result", "win"}}})}};
        auto report = score_backtest(doc);
        check(report["selectedBets"] == 1, "Push-aware EV selection");
        near(number(report["brier"]), std::pow(.5 / .9 - 1, 2));
    });
    test("ESPN saved public-page embedded event scraping", [] {
        auto data = espn_fixture();
        auto html = std::string("<script>window['__espnfitt__']=") + data.dump() + ";</script>";
        throws([&] { scrape_saved_html(html); });
        auto extracted = scrape_saved_html(html, "", "", now_iso());
        check(extracted["games"].size() == 1 && extracted["quotes"].size() == 2, "ESPN embedded extraction");
        check(extracted["games"][0]["homeEspnId"] == "9", "Roster crosswalk preserved");
    });
    test("Canonical quote IDs agree across public feeds and manual imports", [] {
        auto s = empty_snapshot("live");
        parse_espn_scoreboard(espn_fixture(), s, now_iso());
        auto imported = validate_import({{"quotes", s["quotes"]}});
        check(imported["quotes"][0]["id"] == s["quotes"][0]["id"],
              "Same identity must not create duplicate quotes");
        merge_import(s, imported);
        check(s["quotes"].size() == 2, "Canonical replacement");
    });
    test("Incomplete live score or clock cannot default to zero", [&] {
        auto s = demo;
        auto q = find_quote(s, "moneyline", "", "home");
        q["gameId"] = "demo-3";
        q["team"] = "DEN";
        s["games"][2]["liveScoresComplete"] = false;
        check(evaluate_quote(q, s)["probability"].is_null(), "Live score completeness guard");
    });
    test("CSV templates cannot manufacture a live price", [&] {
        auto csv = csv_template(demo);
        check(csv.find("REPLACE_WITH_BOOK") != std::string::npos, "Template placeholder");
        throws([&] { parse_csv_import(csv); });
    });
    test("First-basket confirmation is scoped to its game", [&] {
        auto s = demo;
        s["players"][normalize_name("Moses Moody")]["starterGameId"] = "other-game";
        auto r = evaluate_quote(find_quote(s, "first_basket", "Stephen Curry", "yes"), s);
        check(r["probability"].is_null(), "Old lineup cannot confirm another event");
        auto p = s["players"][normalize_name("Moses Moody")];
        p.erase("starterGameId");
        throws([&] { validate_import({{"players", json::array({p})}}); });
    });
    test("Already-achieved live thresholds are not new edges", [&] {
        auto q = find_quote(demo, "points", "Nikola Jokić");
        q["line"] = 0.5;
        check(evaluate_quote(q, demo)["probability"].is_null(), "Points already won");
        auto s = demo;
        s["players"][normalize_name("Nikola Jokic")]["live"]["rebounds"] = 11;
        q = find_quote(s, "double_double", "Nikola Jokić", "yes");
        check(evaluate_quote(q, s)["probability"].is_null(), "Double-double already won");
        q = find_quote(s, "team_total");
        q["gameId"] = "demo-3";
        q["team"] = "DEN";
        q["line"] = 60.5;
        check(evaluate_quote(q, s)["probability"].is_null(), "Team total already won");
    });
    test("Overtime rotations and unconfirmed final transitions are suppressed", [&] {
        auto s = demo;
        for (auto &g : s["games"])
            if (g["id"] == "demo-3") {
                g["period"] = 5;
                g["elapsedMinutes"] = 50;
            }
        check(evaluate_quote(find_quote(s, "points", "Nikola Jokić"), s)["probability"].is_null(),
              "OT outside player baseline");
        for (auto &g : s["games"])
            if (g["id"] == "demo-3") {
                g["period"] = 4;
                g["elapsedMinutes"] = 48;
            }
        auto q = find_quote(s, "moneyline", "", "home");
        q["gameId"] = "demo-3";
        q["team"] = "DEN";
        q["side"] = "home";
        check(evaluate_quote(q, s)["probability"].is_null(), "Await explicit final or OT");
    });
    test("Expired forecasts clear all sizing outputs", [&] {
        auto d = build_dashboard(demo);
        for (auto &r : d["markets"])
            if (r["game"].value("status", "") == "live") {
                r["game"]["dataAsOf"] = iso_at(parse_time(now_iso()) - 200);
                r["kellyFraction"] = 0.02;
            }
        d = expire_dashboard(d);
        for (auto &r : d["markets"])
            if (r["game"].value("status", "") == "live")
                check(r["kellyFraction"].is_null() && r["conservativeEv"].is_null() &&
                          !r["eligible"].get<bool>(),
                      "No stake for expired model");
    });
    test("Percentage quotes preserve exact payouts, pushes and Kelly without American rounding", [&] {
        auto q = find_quote(demo, "points", "Stephen Curry");
        q.erase("americanOdds");
        q["oddsPercent"] = 52.1234567;
        auto imported = validate_import({{"quotes", json::array({q})}})["quotes"][0];
        check(imported["americanOdds"].is_null(), "No lossy integer American conversion");
        near(quote_implied_probability(imported), .521234567);
        near(quote_decimal_odds(imported), 1 / .521234567);
        near(expected_value(.55, .05, imported), 100 * (.55 / .521234567 + .05 - 1));
        auto equivalent = json{{"oddsPercent", 100 * implied_probability(-110)}};
        near(expected_value(.55, .05, equivalent), expected_value(.55, .05, -110));
        near(fractional_kelly(.55, .05, equivalent), fractional_kelly(.55, .05, -110));
        auto result = evaluate_quote(imported, demo);
        check(result["eligible"].get<bool>(), "Percentage-only quote must evaluate");
        near(number(result["oddsPercent"]), 52.1234567);
        for (double invalid : {0., 100., -1., 101.}) {
            q["oddsPercent"] = invalid;
            throws([&] { validate_import({{"quotes", json::array({q})}}); });
        }
        q["oddsPercent"] = 52.38;
        q["americanOdds"] = 120;
        throws([&] { validate_import({{"quotes", json::array({q})}}); });
    });
    test("All prices expose percentages even when model inputs are blocked", [&] {
        auto s = empty_snapshot("live");
        s["games"] = demo["games"];
        auto q = find_quote(demo, "points", "Stephen Curry");
        auto r = evaluate_quote(q, s);
        check(r["probability"].is_null(), "Missing player data stays missing");
        near(number(r["oddsPercent"]), 100 * implied_probability(q["americanOdds"]));
        near(number(r["impliedProbability"]), implied_probability(q["americanOdds"]));
        q["americanOdds"] = nullptr;
        check(evaluate_quote(q, s)["oddsPercent"].is_null(), "Unpriced is not zero percent");
    });
    test("Percentage CSV import and templates work alongside legacy American CSV", [&] {
        auto csv = std::string("gameId,market,player,team,side,line,oddsPercent,book,observedAt\n") +
                   "demo-2,points,Stephen Curry,GSW,over,26.5,52.38,Percent book," + now_iso() + "\n";
        auto parsed = parse_csv_import(csv);
        near(number(parsed["quotes"][0]["oddsPercent"]), 52.38);
        check(parsed["quotes"][0]["americanOdds"].is_null(), "Exact percentage price");
        check(csv_template(demo).find("oddsPercent") != std::string::npos, "Percentage template");
        throws([&] { parse_csv_import(csv_template(demo)); });
    });
    test("Matched no-vig pairs work with percentage-only quotes", [&] {
        auto s = demo;
        auto q = find_quote(s, "points", "Stephen Curry");
        q.erase("americanOdds");
        q["book"] = "Percent pair";
        q["oddsPercent"] = 52.38;
        auto opposite = q;
        opposite["side"] = "under";
        opposite["oddsPercent"] = 51.11;
        auto quotes = validate_import({{"quotes", json::array({q, opposite})}});
        merge_import(s, quotes);
        auto board = build_dashboard(s);
        int paired = 0;
        for (const auto &row : board["markets"])
            if (row.value("book", "") == "Percent pair") {
                ++paired;
                check(!row["fairProbability"].is_null(), "Percentage quote pair missing fair probability");
                if (row["side"] == "over")
                    near(number(row["fairProbability"]), 52.38 / (52.38 + 51.11));
            }
        check(paired == 2, "Both percentage selections are present");
    });
    test("Dated first-FG counts exclude same-day and future observations", [&] {
        const auto today = now_iso().substr(0, 10);
        const auto yesterday = iso_at(parse_time(now_iso()) - 86400).substr(0, 10);
        const auto tomorrow = iso_at(parse_time(now_iso()) + 86400).substr(0, 10);
        auto p = json{
            {"name", "Dated Fixture"},
            {"team", "GSW"},
            {"logs", json::array()},
            {"firstBasketGames", 999},
            {"firstBasketWins", 999},
            {"firstBasketHistory", json::array({{{"gameId", "before"}, {"date", yesterday}, {"win", false}},
                                                {{"gameId", "target-day"}, {"date", today}, {"win", true}},
                                                {{"gameId", "after"}, {"date", tomorrow}, {"win", true}}})}};
        p = validate_import({{"players", json::array({p})}})["players"][normalize_name("Dated Fixture")];
        auto counts = first_basket_counts(p, today + "T23:00:00Z");
        check(counts["games"] == 1 && counts["wins"] == 0, "No leakage through aggregate counts");
        check(!p.contains("firstBasketGames"), "Dated data replaces aggregate totals");
        p["firstBasketHistory"].push_back(p["firstBasketHistory"][0]);
        throws([&] { validate_import({{"players", json::array({p})}}); });
    });
    test("Lineup-only imports retain history and are scoped to the current game", [&] {
        auto s = demo;
        auto key = normalize_name("Stephen Curry");
        auto logs = s["players"][key]["logs"];
        auto validated = validate_import({{"lineups", json::array({{{"player", "Stephen Curry"},
                                                                    {"team", "GSW"},
                                                                    {"gameId", "demo-2"},
                                                                    {"confirmed", false}}})}});
        merge_import(s, validated);
        check(s["players"][key]["logs"] == logs, "Lineup update must not replace historical features");
        check(!s["players"][key]["starterConfirmed"].get<bool>(), "Explicit deconfirmation applied");
        auto q = find_quote(s, "first_basket", "Stephen Curry", "yes");
        check(evaluate_quote(q, s)["probability"].is_null(), "Incomplete field remains blocked");
        validated["lineups"][0]["confirmed"] = true;
        merge_import(s, validated);
        check(!evaluate_quote(q, s)["probability"].is_null(),
              "Current starter confirmation restores full field");
        check(s["lineups"].size() == 1, "Lineup updates deduplicate");
    });
    test("Streaming historical bundles validate atomically and never confirm current starters", [&] {
        const auto directory = std::filesystem::temp_directory_path() /
                               ("edgecourt-history-unit-" +
                                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(directory);
        struct Cleanup {
            std::filesystem::path directory;
            ~Cleanup() { std::filesystem::remove_all(directory); }
        } cleanup{directory};
        check(load_history_bundle(directory).status["state"] == "not_installed",
              "Missing bundle is explicit");
        const auto date = iso_at(parse_time(now_iso()) - 86400).substr(0, 10);
        auto p = demo["players"][normalize_name("Stephen Curry")];
        p["dataAsOf"] = date + "T23:59:59Z";
        p["firstBasketHistory"] =
            json::array({{{"gameId", "fixture-history"}, {"date", date}, {"win", true}}});
        json manifest = {{"recordType", "manifest"},
                         {"source", "basketball_reference"},
                         {"schemaVersion", 1},
                         {"metadata",
                          {{"games", 1},
                           {"events", 20},
                           {"missingGames", 0},
                           {"firstDate", date},
                           {"lastDate", date},
                           {"asOf", now_iso().substr(0, 10)},
                           {"exportedAt", now_iso()}}}};
        auto write = [&](const std::string &extra = "") {
            std::ofstream file(directory / "features.jsonl");
            file << manifest.dump() << "\n"
                 << json{{"recordType", "player"}, {"feature", p}}.dump() << "\n"
                 << extra;
        };
        write();
        auto history = load_history_bundle(directory);
        const auto key = normalize_name("Stephen Curry");
        check(history.status["players"] == 1, "Streaming player feature count");
        check(!history.features["players"][key]["starterConfirmed"].get<bool>(),
              "Past starters are not current starters");
        check(!history.features["players"][key].contains("live"),
              "Past boxes cannot create fresh live stats");
        auto live = empty_snapshot("live");
        live["players"][key] = {{"name", "Stephen Curry"},  {"team", "LAL"},
                                {"injury", "Out"},          {"logs", json::array()},
                                {"starterConfirmed", true}, {"starterGameId", "current-event"}};
        apply_history(live, history);
        check(live["players"][key]["team"] == "LAL" && live["players"][key]["injury"] == "Out",
              "Fresh roster status wins");
        check(live["players"][key]["starterGameId"] == "current-event", "Preserve explicit current lineup");
        check(live["players"][key]["logs"].size() == p["logs"].size(),
              "Historical vectors actually enter snapshot");
        auto isolated = demo;
        apply_history(isolated, history);
        check(isolated["players"] == demo["players"], "Demo never mixes real history into synthetic data");
        write("{bad-json}\n");
        throws([&] { load_history_bundle(directory); });
        manifest["synthetic"] = true;
        write();
        throws([&] { load_history_bundle(directory); });
    });
    test("NBA CDN odds file converts named-book decimal moneylines", [&] {
        auto s = demo;
        for (auto &g : s["games"])
            if (g.value("id", "") == "demo-2")
                g["officialGameId"] = "0022600002";
        auto doc = json::parse(
            R"({"games":[{"gameId":"0022600002","homeTeamId":"1610612747","awayTeamId":"1610612744","markets":[{"name":"2way","odds_type_id":1,"books":[{"id":"sr:book:1","name":"FixtureKings","outcomes":[{"type":"home","odds":"1.91"},{"type":"away","odds":"1.91"}]},{"id":"sr:book:2","name":"BetFixture","outcomes":[{"type":"home","odds":"2.10"},{"type":"away","odds":"1.80"}]}]},{"name":"spread","odds_type_id":4,"books":[{"name":"FixtureKings","outcomes":[{"type":"home","odds":"1.91","spread":"-3.5"}]}]}]},{"gameId":"0022600999","homeTeamId":"1610612738","awayTeamId":"1610612752","markets":[{"name":"2way","books":[{"name":"FixtureKings","outcomes":[{"type":"home","odds":"1.50"},{"type":"away","odds":"2.60"}]}]}]}]})");
        parse_nba_cdn_odds(doc, s, now_iso());
        int count = 0;
        bool saw_plus_110 = false, saw_minus_125 = false, saw_books = false;
        for (const auto &q : s["quotes"]) {
            if (q.value("source", "") != "nba_cdn_odds")
                continue;
            ++count;
            if (q.value("americanOdds", json()).is_number() && q["americanOdds"] == 110)
                saw_plus_110 = true;
            if (q.value("americanOdds", json()).is_number() && q["americanOdds"] == -125)
                saw_minus_125 = true;
            if (q.value("book", "") == "BetFixture")
                saw_books = true;
        }
        check(count == 6, "Four moneylines for the matched game plus two via team ids; spread ignored");
        check(saw_plus_110 && saw_minus_125, "Decimal odds converted to American (2.10 -> +110, 1.80 -> -125)");
        check(saw_books, "Sportsbook names are preserved for book filtering");
    });
    test("History loader accepts SportsDataverse-stamped manifests", [&] {
        const auto directory = std::filesystem::temp_directory_path() / "edgecourt-sdv-manifest-test";
        std::filesystem::create_directories(directory);
        struct Cleanup {
            std::filesystem::path directory;
            ~Cleanup() { std::filesystem::remove_all(directory); }
        } cleanup{directory};
        const auto date = iso_at(parse_time(now_iso()) - 86400).substr(0, 10);
        auto p = demo["players"][normalize_name("Stephen Curry")];
        p["dataAsOf"] = date + "T23:59:59Z";
        json manifest = {{"recordType", "manifest"},
                         {"source", "sportsdataverse hoopR ESPN play-by-play + player box scores (local CSV ingest)"},
                         {"schemaVersion", 1},
                         {"metadata",
                          {{"games", 1},
                           {"events", 20},
                           {"missingGames", 0},
                           {"firstDate", date},
                           {"lastDate", date},
                           {"asOf", now_iso().substr(0, 10)},
                           {"exportedAt", now_iso()}}}};
        std::ofstream file(directory / "features.jsonl");
        file << manifest.dump() << "\n"
             << json{{"recordType", "player"}, {"feature", p}}.dump() << "\n";
        file.close();
        auto history = load_history_bundle(directory);
        check(history.status["historySource"] == "sportsdataverse", "Provenance tag propagated");
        check(history.features["players"][normalize_name("Stephen Curry")]["historySource"] ==
                  "sportsdataverse",
              "Player records carry the SportsDataverse tag");
    });
    test("Native SportsDataverse builder emits a loadable bundle with first-FG history", [] {
        const auto directory = std::filesystem::temp_directory_path() / "edgecourt-sdv-builder-test";
        std::filesystem::create_directories(directory / "downloads");
        struct Cleanup {
            std::filesystem::path directory;
            ~Cleanup() { std::filesystem::remove_all(directory); }
        } cleanup{directory};
        const std::string box_header =
            "game_id,athlete_id,athlete_display_name,team_abbreviation,starter,did_not_play,minutes,points,"
            "field_goals_made,field_goals_attempted,three_point_field_goals_made,"
            "three_point_field_goals_attempted,free_throws_made,free_throws_attempted,offensive_rebounds,"
            "total_rebounds,assists,steals,blocks,turnovers,personal_fouls,game_date,home_team_abbreviation,"
            "away_team_abbreviation,season_type\n";
        const std::string pbp_header =
            "game_id,text,type_text,period_number,clock_display_value,scoring_play,score_value,away_score,"
            "home_score,athlete_id_1\n";
        auto game_rows = [](const std::string &game_id, const std::string &date) {
            std::string box, pbp;
            for (int side = 0; side < 2; ++side) {
                const std::string team = side == 0 ? "LAL" : "GSW", prefix = side == 0 ? "Laker" : "Warrior";
                const int base = side == 0 ? 100 : 200;
                for (int i = 1; i <= 5; ++i) {
                    box += game_id + "," + std::to_string(base + i) + "," + prefix + " " + std::to_string(i) + "," +
                           team + ",true,false,30," + std::to_string(8 + i) + ",4,8,1,3,2,2,1,6,5,1,1,2,3," + date +
                           ",LAL,GSW,2\n";
                }
            }
            return std::pair<std::string, std::string>(box, pbp);
        };
        std::string box = box_header, pbp = pbp_header;
        auto g1 = game_rows("401000001", "2024-01-05");
        auto g2 = game_rows("401000002", "2024-01-06");
        box += g1.first + g2.first;
        // Game 1: a made free throw first (must not count), then Laker 1 scores the first field goal.
        pbp += "401000001,\"Laker 1 makes free throw\",Free Throw,1,11:32,true,1,0,1,101\n";
        pbp += "401000001,\"Laker 1 makes 2-pt jump shot, assisted by Laker 2\",Jump Shot,1,11:10,true,2,0,3,101\n";
        // Game 2: Warrior 1 scores the first field goal.
        pbp += "401000002,\"Warrior 1 makes 2-pt dunk\",Dunk,1,11:45,true,2,2,0,201\n";
        pbp += "401000002,\"Laker 1 makes 3-pt jump shot\",Jump Shot,1,11:20,true,3,2,3,101\n";
        {
            std::ofstream(directory / "downloads" / "player_box_2024.csv") << box;
            std::ofstream(directory / "downloads" / "play_by_play_2024.csv") << pbp;
        }
        const auto metadata = build_history_bundle(directory, {2024}, "2024-02-01");
        check(metadata["games"] == 2 && metadata["missingGames"] == 0, "Both fixture games completed");
        auto bundle = load_history_bundle(directory);
        check(bundle.status["state"] == "local_history" && bundle.status["historySource"] == "sportsdataverse",
              "Native bundle loads with SportsDataverse provenance");
        const auto &laker = bundle.features["players"][normalize_name("Laker 1")];
        check(laker["logs"].size() == 2, "Player logs exported for both games");
        check(laker["firstBasketHistory"].size() == 2 && laker["firstBasketHistory"][0]["win"] == false &&
                  laker["firstBasketHistory"][1]["win"] == true,
              "First-FG opportunities are dated, newest-first, free throw ignored");
        const auto &warrior = bundle.features["players"][normalize_name("Warrior 1")];
        check(warrior["firstBasketHistory"].size() == 2 && warrior["firstBasketHistory"][0]["win"] == true,
              "Away scorer credited in game 2");
    });
    test("Polymarket game moneylines become percentage quotes; futures/spreads ignored", [&] {
        auto s = empty_snapshot("live");
        const auto start = iso_at(parse_time(now_iso()) + 86400);
        s["games"].push_back({{"id", "g1"}, {"home", "LAL"}, {"away", "GSW"}, {"startsAt", start}});
        json doc = json::array({
            {{"slug", "nba-future"}, {"markets", json::array({{{"active", true}, {"closed", false},
                {"outcomes", "[\"Yes\", \"No\"]"}, {"outcomePrices", "[\"0.4\", \"0.6\"]"}}})}},
            {{"slug", "nba-gsw-lal"}, {"markets", json::array({
                {{"active", true}, {"closed", false}, {"sportsMarketType", "spreads"},
                 {"outcomes", "[\"Golden State Warriors\", \"Los Angeles Lakers\"]"},
                 {"outcomePrices", "[\"0.5\", \"0.5\"]"}, {"endDate", start}},
                {{"active", true}, {"closed", false}, {"sportsMarketType", "moneyline"},
                 {"outcomes", "[\"Golden State Warriors\", \"Los Angeles Lakers\"]"},
                 {"outcomePrices", "[\"0.42\", \"0.58\"]"}, {"endDate", start}}})}}});
        parse_polymarket_events(doc, s, now_iso());
        check(s["quotes"].size() == 2, "Only the moneyline pair is imported");
        check(s["quotes"][0]["book"] == "Polymarket" && s["quotes"][0]["americanOdds"].is_null(), "Native % price");
        near(number(s["quotes"][0]["oddsPercent"]), 42.0);
        near(number(s["quotes"][1]["oddsPercent"]), 58.0);
        check(s["quotes"][1]["side"] == "home", "Outcome mapped to the scheduled home team");
    });
    test("Kalshi NBA game markets use ticker teams and dollar midpoints", [&] {
        auto s = empty_snapshot("live");
        s["games"].push_back({{"id", "g2"}, {"home", "LAL"}, {"away", "DEN"}, {"startsAt", "2026-10-17T02:00:00Z"}});
        json doc = {{"markets", json::array({
            {{"status", "active"}, {"event_ticker", "KXNBAGAME-26OCT16DENLAL"}, {"ticker", "KXNBAGAME-26OCT16DENLAL-LAL"},
             {"occurrence_datetime", "2026-10-17T05:00:00Z"}, {"yes_bid_dollars", "0.5000"}, {"yes_ask_dollars", "0.5900"}},
            {{"status", "active"}, {"event_ticker", "KXNBAGAME-26OCT16DENLAL"}, {"ticker", "KXNBAGAME-26OCT16DENLAL-DEN"},
             {"occurrence_datetime", "2026-10-17T05:00:00Z"}, {"yes_bid_dollars", "0.3900"}, {"yes_ask_dollars", "0.5000"}},
            {{"status", "closed"}, {"event_ticker", "KXNBAGAME-26OCT16DENLAL"}, {"ticker", "KXNBAGAME-26OCT16DENLAL-LAL"},
             {"occurrence_datetime", "2026-10-17T05:00:00Z"}, {"yes_bid_dollars", "0.9000"}, {"yes_ask_dollars", "0.9100"}}})}};
        parse_kalshi_markets(doc, s, now_iso());
        check(s["quotes"].size() == 2, "Closed markets are skipped");
        near(number(s["quotes"][0]["oddsPercent"]), 54.5);
        near(number(s["quotes"][1]["oddsPercent"]), 44.5);
        check(s["quotes"][0]["side"] == "home" && s["quotes"][1]["side"] == "away", "Kalshi sides");
    });
    test("SharpAPI pregame NBA moneylines match the schedule and skip unrelated markets", [&] {
        auto s = empty_snapshot("live");
        const auto starts = parse_time(now_iso()) + 3 * 60 * 60;
        const auto start = iso_at(starts);
        s["games"].push_back({{"id", "espn-sharp-1"}, {"home", "PHI"}, {"away", "PHX"},
                              {"startsAt", start}, {"status", "upcoming"}});
        const auto fetched = now_iso();
        json doc = {{"data", json::array({
            {{"id", "dk-1"}, {"sportsbook", "draftkings"}, {"league", "nba"},
             {"market_type", "moneyline"}, {"selection", "PHI 76ers"}, {"selection_type", "home"},
             {"home_team", "PHI 76ers"}, {"away_team", "PHO Suns"}, {"event_start_time", start},
             {"odds_american", -110}, {"timestamp", fetched}, {"is_live", false}},
            {{"id", "fd-1"}, {"sportsbook", "fanduel"}, {"league", "nba"},
             {"market_type", "moneyline"}, {"selection", "PHO Suns"}, {"selection_type", "away"},
             {"home_team", "PHI 76ers"}, {"away_team", "PHO Suns"}, {"event_start_time", start},
             {"odds_american", "+130"}, {"timestamp", fetched}, {"is_live", false}},
            {{"id", "prop-1"}, {"sportsbook", "draftkings"}, {"league", "nba"},
             {"market_type", "player_points"}, {"selection", "Over"}, {"selection_type", "over"},
             {"home_team", "PHI 76ers"}, {"away_team", "PHO Suns"}, {"event_start_time", start},
             {"odds_american", -110}, {"timestamp", fetched}, {"is_live", false}},
            {{"id", "live-1"}, {"sportsbook", "draftkings"}, {"league", "nba"},
             {"market_type", "moneyline"}, {"selection", "PHI 76ers"}, {"selection_type", "home"},
             {"home_team", "PHI 76ers"}, {"away_team", "PHO Suns"}, {"event_start_time", start},
             {"odds_american", -110}, {"timestamp", fetched}, {"is_live", true}},
            {{"id", "pm-1"}, {"sportsbook", "polymarket"}, {"league", "nba"},
             {"market_type", "moneyline"}, {"selection", "PHI 76ers"}, {"selection_type", "home"},
             {"home_team", "PHI 76ers"}, {"away_team", "PHO Suns"}, {"event_start_time", start},
             {"odds_american", -110}, {"timestamp", fetched}, {"is_live", false}}
        })}};
        parse_sharpapi_odds(doc, s, s, fetched);
        check(s["quotes"].size() == 2, "Only prematch sportsbook moneylines are imported");
        check(s["quotes"][0]["book"] == "DraftKings" && s["quotes"][1]["book"] == "FanDuel",
              "Book IDs are rendered as sportsbook names");
        check(s["quotes"][0]["source"] == "sharpapi" && s["quotes"][0]["americanOdds"] == -110 &&
                  s["quotes"][1]["americanOdds"] == 130,
              "American prices and SharpAPI provenance are retained");
        check(s["quotes"][0]["side"] == "home" && s["quotes"][1]["side"] == "away" &&
                  s["quotes"][0]["observedAt"] == fetched,
              "Team selection and provider timestamp are matched to the scheduled game");
    });
    test("NBA_Go and TheSportsDB attach context without changing model data", [&] {
        auto s = empty_snapshot("live");
        const auto starts = parse_time(now_iso()) + 3 * 60 * 60;
        const auto start = iso_at(starts);
        const auto token = daily_lineup_date_token(starts);
        const auto game_date = token.substr(0, 4) + "-" + token.substr(4, 2) + "-" + token.substr(6, 2);
        s["games"].push_back({{"id", "context-game"}, {"home", "LAL"}, {"away", "GSW"},
                              {"startsAt", start}, {"status", "upcoming"},
                              {"homeScore", 0}, {"awayScore", 0}});
        json nba_go = {{"data", json::array({
            {{"gameId", "go-123"}, {"date", game_date}, {"homeTeam", "LAL"}, {"visitorTeam", "GSW"},
             {"homePts", 113}, {"visitorPts", 107}, {"arena", "Crypto.com Arena"},
             {"playerGameBasicStats", json::array({
                 {{"playerName", "LeBron James"}, {"team", "LAL"}, {"pts", 25}, {"trb", 8}, {"ast", 7}},
                 {{"playerName", "Stephen Curry"}, {"team", "GSW"}, {"pts", 23}, {"trb", 4}, {"ast", 6}}})},
             {"teamGameAdvStats", json::array({{{"team", "LAL"}, {"oRtg", 115}, {"dRtg", 108},
                                                   {"efgPercent", 0.55}, {"tsPercent", 0.58}}})},
             {"lineScores", json::array({{{"team", "LAL"}, {"q1", 28}, {"q2", 29}, {"q3", 30},
                                             {"q4", 26}, {"total", 113}}})}}
        })}};
        check(parse_nba_go_games(nba_go, s, now_iso()) == 1,
              "NBA_Go matches the game by teams and Eastern game date");
        check(s["games"][0]["externalContext"]["nbaGo"]["homeScore"] == 113 &&
                  s["games"][0]["externalContext"]["nbaGo"]["topPerformers"][0]["name"] == "LeBron James",
              "NBA_Go contributes compact score/stat context");
        json sportsdb = {{"schedule", json::array({
            {{"idEvent", "tsdb-1"}, {"idLeague", "4387"}, {"strSport", "Basketball"},
             {"strHomeTeam", "Los Angeles Lakers"}, {"strAwayTeam", "Golden State Warriors"},
             {"dateEvent", game_date}, {"strTimestamp", start}, {"strStatus", "Not Started"},
             {"strVenue", "Crypto.com Arena"}, {"strCity", "Los Angeles"},
             {"intHomeScore", nullptr}, {"intAwayScore", nullptr}}
        })}};
        check(parse_thesportsdb_schedule(sportsdb, s, now_iso()) == 1 &&
                  s["games"][0]["externalContext"]["theSportsDB"]["venue"] == "Crypto.com Arena",
              "TheSportsDB schedule metadata attaches to the same NBA game");
        check(s["games"][0]["homeScore"] == 0 && s["games"][0]["awayScore"] == 0 &&
                  s["quotes"].empty() && s["teams"].empty(),
              "Context-only providers do not overwrite scores, add odds or modify model team inputs");
        auto baseline = demo;
        auto contextual = demo;
        const auto model_quote = find_quote(baseline, "moneyline", "", "home");
        for (auto &game : contextual["games"])
            if (game.value("id", "") == model_quote.value("gameId", "")) {
                game["externalContext"] = s["games"][0]["externalContext"];
                break;
            }
        near(number(evaluate_quote(model_quote, contextual)["probability"]),
             number(evaluate_quote(model_quote, baseline)["probability"]));
    });
    test("API-Basketball NBA moneylines map by game ID and convert decimal odds", [&] {
        auto s = empty_snapshot("live");
        const auto starts = parse_time(now_iso()) + 3 * 60 * 60;
        const auto start = iso_at(starts);
        s["games"].push_back({{"id", "espn-1"}, {"home", "BOS"}, {"away", "NYK"},
                              {"startsAt", start}, {"status", "upcoming"}});
        s["games"].push_back({{"id", "espn-2"}, {"home", "BOS"}, {"away", "NYK"},
                              {"startsAt", iso_at(starts + 24 * 60 * 60)}, {"status", "upcoming"}});
        const json team_pair = {{"home", json{{"name", "Boston Celtics"}}},
                                 {"away", json{{"name", "New York Knicks"}}}};
        const json schedule = {{"response", json::array({
            json{{"id", 12001}, {"timestamp", starts}, {"teams", team_pair}},
            json{{"id", 12002}, {"timestamp", starts + 12 * 60 * 60}, {"teams", team_pair}},
            json{{"id", 99999}, {"timestamp", starts},
                 {"teams", {{"home", {{"name", "Unknown Club"}}, {"away", {{"name", "Other Club"}}}}}}}
        })}};
        const auto matches = match_api_basketball_games(schedule, s);
        check(matches.size() == 1 && matches.count("12001"),
              "Only a same-team, six-hour tip-off match joins; nearby duplicate fixtures are excluded");
        const json odds = {{"response", json::array({
            {{"game", {{"id", 12001}}}, {"bookmakers", json::array({
                {{"id", 4}, {"name", "Example Sportsbook"}, {"bets", json::array({
                    {{"id", 2}, {"name", "Home/Away"}, {"values", json::array({
                        {{"value", "Home"}, {"odd", "1.91"}}, {{"value", "Away"}, {"odd", "2.50"}}})}},
                    {{"id", 5}, {"name", "Game Total"}, {"values", json::array({
                        {{"value", "Over"}, {"odd", "1.90"}}})}}
                })}}
            })}},
            {{"game", {{"id", 99999}}}, {"bookmakers", json::array()}}
        })}};
        const auto fetched = now_iso();
        parse_api_basketball_odds(odds, matches, s, fetched);
        check(s["quotes"].size() == 2, "Only the matched game's two-way moneyline is kept");
        check(s["quotes"][0]["source"] == "api_basketball" &&
                  s["quotes"][0]["book"] == "Example Sportsbook" &&
                  s["quotes"][0]["observedAt"] == fetched,
              "Quote provenance, bookmaker and retrieval timestamp are retained");
        check(s["quotes"][0]["americanOdds"] == -110 && s["quotes"][1]["americanOdds"] == 150,
              "Decimal odds convert to American format");
        check(s["quotes"][0]["side"] == "home" && s["quotes"][1]["side"] == "away",
              "Home/Away outcomes map to scheduled team sides");
    });
    test("API-Basketball quota guard reserves daily requests and seasons use full years", [] {
        check(api_basketball_season(parse_time("2026-10-13T00:00:00Z")) == "2026-2027",
              "NBA fall season format");
        check(api_basketball_season(parse_time("2026-09-30T23:59:59Z")) == "2025-2026",
              "NBA summer season format");
        check(api_basketball_quota_allows(0, 100, 20, 2), "Two daily adapter calls fit with the reserve");
        check(api_basketball_quota_allows(79, 100, 20, 1), "One call may use remaining capacity above reserve");
        check(!api_basketball_quota_allows(79, 100, 20, 2), "Insufficient capacity for both adapter calls");
        check(!api_basketball_quota_allows(80, 100, 20, 1), "Never consume the protected reserve");
        check(!api_basketball_quota_allows(0, 0, 20, 1), "Unknown daily limit fails closed");
    });
    test("Pinnacle guest moneylines keep exact American prices and ignore totals", [&] {
        auto s = empty_snapshot("live");
        const auto start = iso_at(parse_time(now_iso()) + 3600);
        s["games"].push_back({{"id", "g3"}, {"home", "LAL"}, {"away", "GSW"}, {"startsAt", start}});
        json matchups = json::array({{{"id", 1}, {"type", "matchup"}, {"startTime", start},
            {"participants", json::array({{{"id", 11}, {"name", "Golden State Warriors"}, {"alignment", "away"}},
                                           {{"id", 12}, {"name", "Los Angeles Lakers"}, {"alignment", "home"}}})}}});
        json markets = json::array({
            {{"matchupId", 1}, {"key", "s;0;m"}, {"prices", json::array({{{"participantId", 11}, {"price", 120}}, {{"participantId", 12}, {"price", -140}}})}},
            {{"matchupId", 1}, {"key", "s;0;ou"}, {"prices", json::array({{{"participantId", 11}, {"price", -110}, {"points", 224.5}}})}}});
        parse_pinnacle_markets(matchups, markets, s, now_iso());
        check(s["quotes"].size() == 2, "Game totals are not team totals and must be skipped");
        check(s["quotes"][0]["americanOdds"] == 120 && s["quotes"][1]["americanOdds"] == -140, "Exact prices");
        auto r = evaluate_quote(s["quotes"][1], s);
        near(number(r["oddsPercent"]), 100 * implied_probability(-140));
    });
    test("ESPN scoreboard URLs are single-day, never a rejected date range", [] {
        const auto urls = espn_scoreboard_day_urls(parse_time("2026-10-12T15:00:00Z"),
                                                   parse_time("2026-10-20T02:00:00Z"));
        check(urls.size() == 9, "Yesterday through next seven days, inclusive");
        check(urls.front().find("dates=20261012&") != std::string::npos, "First day");
        check(urls.back().find("dates=20261020&") != std::string::npos, "Last day");
        for (const auto &url : urls) {
            const auto dates = url.substr(url.find("dates=") + 6);
            check(dates.size() == 8 + std::string("&limit=100").size() &&
                      dates.find('-') == std::string::npos,
                  "Hyphenated ranges return HTTP 400 and must never be built");
            check(allowed_data_url(url), "Day URL stays on the allowlist");
        }
        check(espn_scoreboard_day_urls(parse_time("2026-10-20T00:00:00Z"),
                                       parse_time("2026-10-12T00:00:00Z"))
                  .empty(),
              "Reversed window is empty");
        check(espn_scoreboard_day_urls(parse_time("2026-10-01T00:00:00Z"),
                                       parse_time("2026-11-30T00:00:00Z"))
                      .size() <= 14,
              "Window is hard-capped");
    });
    test("ESPN preseason and postseason games stay in the data with their prices", [&] {
        auto fixture = espn_fixture();
        fixture["events"][0]["season"]["type"] = 1;
        auto s = empty_snapshot("live");
        parse_espn_scoreboard(fixture, s, now_iso());
        check(s["games"].size() == 1 && s["games"][0]["seasonType"] == "preseason", "Preseason typing");
        check(s["quotes"].size() == 2, "Preseason moneylines are kept");
        auto dashboard = build_dashboard(s);
        check(dashboard["games"].size() == 1 && dashboard["summary"]["quotes"] == 2,
              "Preseason game reaches the dashboard");
        const auto &row = dashboard["markets"][0];
        check(!row["oddsPercent"].is_null(), "Preseason price stays visible as Odds %");
        check(row["probability"].is_null() &&
                  row.value("reason", "").find("Preseason") != std::string::npos,
              "Preseason projections remain guarded, not hidden");
        fixture["events"][0]["season"] = {{"type", 3}};
        s = empty_snapshot("live");
        parse_espn_scoreboard(fixture, s, now_iso());
        check(s["games"][0]["seasonType"] == "playoffs", "Postseason is not mislabeled regular");
        fixture["events"][0]["season"] = {{"slug", "preseason"}};
        s = empty_snapshot("live");
        parse_espn_scoreboard(fixture, s, now_iso());
        check(s["games"][0]["seasonType"] == "preseason", "Season slug fallback");
    });
    test("NBA CDN game-id prefixes type preseason and playoff games", [] {
        for (const auto &item : std::vector<std::pair<std::string, std::string>>{
                 {"0012600001", "preseason"}, {"0022600001", "regular"}, {"0042600001", "playoffs"}}) {
            auto s = empty_snapshot("live");
            json fixture = {
                {"scoreboard",
                 {{"games", json::array({{{"gameId", item.first},
                                          {"gameTimeUTC", iso_at(parse_time(now_iso()) + 86400)},
                                          {"gameStatus", 1},
                                          {"period", 0},
                                          {"gameClock", ""},
                                          {"homeTeam", {{"teamTricode", "GSW"}, {"score", 0}}},
                                          {"awayTeam", {{"teamTricode", "NYK"}, {"score", 0}}}}})}}}};
            parse_nba_scoreboard(fixture, s, now_iso());
            check(s["games"].size() == 1 && s["games"][0]["seasonType"] == item.second,
                  "NBA CDN season type for " + item.first);
        }
    });
    test("Public moneylines match preseason schedule games", [&] {
        auto s = empty_snapshot("live");
        const auto start = iso_at(parse_time(now_iso()) + 86400);
        s["games"].push_back({{"id", "g-pre"},
                              {"home", "LAL"},
                              {"away", "GSW"},
                              {"startsAt", start},
                              {"seasonType", "preseason"}});
        json doc = json::array({{{"slug", "nba-gsw-lal-pre"},
                                 {"markets", json::array({{{"active", true},
                                                           {"closed", false},
                                                           {"sportsMarketType", "moneyline"},
                                                           {"outcomes", "[\"Golden State Warriors\", \"Los Angeles Lakers\"]"},
                                                           {"outcomePrices", "[\"0.42\", \"0.58\"]"},
                                                           {"endDate", start}}})}}});
        parse_polymarket_events(doc, s, now_iso());
        check(s["quotes"].size() == 2, "Preseason moneyline pair matched to the schedule");
        auto row = evaluate_quote(s["quotes"][0], s);
        check(!row["oddsPercent"].is_null() && row["probability"].is_null(),
              "Preseason price is displayed while the projection stays guarded");
    });
    test("Imported games accept preseason/playoffs and reject unknown season types", [] {
        json game = {{"id", "g-import-pre"},
                     {"home", "GSW"},
                     {"away", "NYK"},
                     {"startsAt", now_iso()},
                     {"seasonType", "preseason"}};
        auto validated = validate_import({{"games", json::array({game})}});
        check(validated["games"][0]["seasonType"] == "preseason", "Preseason import kept");
        game["seasonType"] = "exhibition";
        throws([&] { validate_import({{"games", json::array({game})}}); });
    });
    test("nba_api standings map cities to teams and stay context-only", [&] {
        auto doc = json::parse(
            R"({"resultSets":[{"headers":["TeamCity","TeamName","WINS","LOSSES","WinPCT","Record","HOME","ROAD","L10","strCurrentStreak","Conference","PlayoffRank","ConferenceGamesBack","PointsPG","OppPointsPG","DiffPointsPG"],"rowSet":[["Boston","Celtics",40,10,0.8,"40-10","22-3","18-7","8-2","W 4","East",1,0.0,118.5,109.0,9.5],["LA","Clippers",30,20,0.6,"30-20","17-8","13-12","5-5","L 1","West",5,6.5,112.0,111.0,1.0]]}]})");
        auto s = empty_snapshot("live");
        parse_standings(doc, s, now_iso());
        check(s["teams"]["BOS"]["standings"]["wins"] == 40, "Standings wins");
        near(number(s["teams"]["BOS"]["standings"]["winPct"]), 0.8);
        check(s["teams"]["LAC"]["standings"]["record"] == "30-20", "LA Clippers city mapping");
        // Context presence must not move a probability.
        auto with_context = demo;
        auto q = find_quote(with_context, "moneyline", "", "home");
        const double before = number(evaluate_quote(q, with_context)["probability"]);
        parse_standings(doc, with_context, now_iso());
        with_context["teams"]["BOS"]["fourFactors"] = {{"efgPct", 0.57}};
        auto row = evaluate_quote(q, with_context);
        near(number(row["probability"]), before);
        bool seen = false;
        for (const auto &f : row["features"])
            if (f.value("label", "") == "Home standings win %" && f.value("role", "") == "context")
                seen = true;
        check(seen, "Standings shown as context, not a model input");
        auto dashboard = build_dashboard(with_context);
        check(dashboard["teams"].size() >= 1 && dashboard["teams"][0].contains("standings"),
              "Dashboard carries compact standings for schedule cards");
    });
    test("nba_api player season stats enrich existing players only, as context", [&] {
        auto doc = json::parse(
            R"({"resultSets":[{"headers":["PLAYER_NAME","TEAM_ABBREVIATION","GP","MIN","PTS","REB","AST","FG3M","STL","BLK","FG_PCT","FG3_PCT","FT_PCT","PLUS_MINUS","DD2","TD3"],"rowSet":[["Stephen Curry","GSW",60,34.1,27.6,4.4,6.5,4.7,1.1,0.4,0.47,0.41,0.92,3.2,2,0],["Ghost Player","GSW",10,10.0,5.0,2.0,1.0,0.5,0.2,0.1,0.4,0.3,0.8,0.5,0,0]]}]})");
        auto s = demo;
        const auto players_before = s["players"].size();
        parse_player_season_stats(doc, s, now_iso());
        check(s["players"].size() == players_before, "A season line never creates a player");
        check(s["players"][normalize_name("Stephen Curry")]["seasonStats"]["pts"] == 27.6, "Season PTS");
        auto q = find_quote(s, "points", "Stephen Curry");
        const double before = number(evaluate_quote(q, demo)["probability"]);
        auto row = evaluate_quote(q, s);
        near(number(row["probability"]), before);
        bool seen = false;
        for (const auto &f : row["features"])
            if (f.value("label", "") == "Season points" && f.value("role", "") == "context")
                seen = true;
        check(seen, "Season average shown as context beside the log-based projection");
    });
    test("nba_api Four Factors parse team and opponent rates", [] {
        auto doc = json::parse(
            R"({"resultSets":[{"headers":["TEAM_NAME","TEAM_ABBREVIATION","EFG_PCT","FTA_RATE","TM_TOV_PCT","OREB_PCT","OPP_EFG_PCT","OPP_FTA_RATE","OPP_TOV_PCT","OPP_OREB_PCT"],"rowSet":[["Golden State Warriors","GSW",0.57,0.24,0.13,0.25,0.54,0.22,0.14,0.27]]}]})");
        auto s = empty_snapshot("live");
        parse_team_four_factors(doc, s, now_iso());
        near(number(s["teams"]["GSW"]["fourFactors"]["efgPct"]), 0.57);
        near(number(s["teams"]["GSW"]["fourFactors"]["oppOrebPct"]), 0.27);
        check(feature_catalog().size() == 53, "Feature catalog includes the nba_api + official/league context feeds");
    });
    test("NBA official schedule enriches existing games only, as context", [&] {
        auto s = demo;
        json game;
        for (const auto &g : s["games"])
            if (g.value("id", "") == "demo-2")
                game = g;
        const auto starts = game.value("startsAt", "");
        auto doc = json::parse(
            R"({"leagueSchedule":{"seasonYear":"2026-27","leagueId":"00","gameDates":[{"gameDate":"10/21/2026","games":[{"gameId":"0022600002","gameCode":"20261021/GSWLAL","gameDateTimeUTC":"2026-10-21T02:30:00Z","gameStatus":1,"arenaName":"Crypto.com Arena","arenaCity":"Los Angeles","arenaState":"CA","homeTeam":{"teamTricode":"LAL"},"awayTeam":{"teamTricode":"GSW"},"broadcasters":{"nationalBroadcasters":[{"broadcasterDisplay":"ESPN"}],"homeTvBroadcasters":[{"broadcasterDisplay":"Spectrum SportsNet"}]},"pointsLeaders":[{"firstName":"Stephen","lastName":"Curry","teamTricode":"GSW","points":31.2}]}]}]}})");
        // Fixture start time must sit within the 36h match window of demo-2.
        doc["leagueSchedule"]["gameDates"][0]["games"][0]["gameDateTimeUTC"] = starts;
        json q;
        for (const auto &candidate : s["quotes"])
            if (candidate.value("market", "") == "moneyline" && candidate.value("gameId", "") == "demo-2" &&
                candidate.value("side", "") == "home")
                q = candidate;
        check(!q.empty(), "Demo moneyline quote for the schedule game");
        const double before = number(evaluate_quote(q, s)["probability"]);
        check(before > 0 && before < 1, "Baseline moneyline probability exists before schedule enrichment");
        parse_official_schedule(doc, s, now_iso());
        const auto &g2 = s["games"][1];
        check(g2.value("officialGameId", "") == "0022600002", "Official game id attached");
        check(g2.value("arenaName", "") == "Crypto.com Arena", "Arena attached");
        check(g2["broadcasts"]["national"].size() == 1 && g2["broadcasts"]["national"][0] == "ESPN",
              "National broadcast attached");
        check(g2["pointsLeaders"].size() == 1 && g2["pointsLeaders"][0]["name"] == "Stephen Curry",
              "Points leader attached");
        check(s["leagueContext"]["schedule"]["matchedGames"] == 1 && s["leagueContext"]["schedule"]["games"] == 1,
              "Schedule coverage counted");
        near(number(evaluate_quote(q, s)["probability"]), before);
        auto empty = empty_snapshot("live");
        parse_official_schedule(doc, empty, now_iso());
        check(empty["games"].empty(), "Official schedule never creates slate games");
    });
    test("NBA daily lineups confirm only explicit starters for the first-basket guard", [&] {
        check(daily_lineup_date_token(parse_time("2026-10-13T02:00:00Z")) == "20261012",
              "Daily lineup token uses the Eastern game date");
        json game;
        for (const auto &g : demo["games"])
            if (g.value("id", "") == "demo-2")
                game = g;
        const auto token = daily_lineup_date_token(parse_time(game.value("startsAt", "")));
        auto make_doc = [](const std::string &status) {
            std::string text_doc =
                R"({"games":[{"gameId":"0022600002","gameStatus":1,"homeTeam":{"teamAbbreviation":"LAL","players":[{"personId":2544,"playerName":"LeBron James","lineupStatus":"LINEUP_STATUS","position":"F","rosterStatus":"Active"}]},"awayTeam":{"teamAbbreviation":"GSW","players":[{"personId":201939,"playerName":"Stephen Curry","lineupStatus":"LINEUP_STATUS","position":"G","rosterStatus":"Active"}]}}]})";
            const auto marker = std::string("LINEUP_STATUS");
            size_t at = 0;
            while ((at = text_doc.find(marker, at)) != std::string::npos) {
                text_doc.replace(at, marker.size(), status);
                at += status.size();
            }
            return json::parse(text_doc);
        };
        auto projected = demo;
        parse_daily_lineups(make_doc("Expected"), projected, now_iso(), token);
        check(projected["players"][normalize_name("Stephen Curry")].value("starterConfirmed", true) == false,
              "Expected starter never confirms and clears stale confirmation");
        check(evaluate_quote(find_quote(projected, "first_basket", "Stephen Curry", "yes"), projected)["probability"]
                  .is_null(),
              "Projected lineup cannot satisfy the first-basket guard");
        auto confirmed = demo;
        parse_daily_lineups(make_doc("Confirmed"), confirmed, now_iso(), token);
        check(confirmed["players"][normalize_name("Stephen Curry")].value("starterConfirmed", false),
              "Confirmed starter satisfies the guard input");
        check(confirmed["games"][1]["lineupSummary"]["homeConfirmed"] == 1 &&
                  confirmed["games"][1]["lineupSummary"]["awayConfirmed"] == 1,
              "Lineup summary counts confirmed starters");
        auto row = evaluate_quote(find_quote(confirmed, "first_basket", "Stephen Curry", "yes"), confirmed);
        check(!row["probability"].is_null(), "Confirmed lineup leaves the first-basket model available");
        check(row.contains("lineupContext") && row["lineupContext"].value("confirmed", false),
              "Panel carries same-game lineup context");
        parse_daily_lineups(make_doc("Expected"), confirmed, now_iso(), token);
        check(confirmed["players"][normalize_name("Stephen Curry")].value("starterConfirmed", true) == false,
              "A later non-confirmed listing clears the confirmation");
    });
    test("NBA player movement stays dated context and never moves a probability", [&] {
        auto s = demo;
        auto doc = json::parse(
            R"({"NBA_Player_Movement":{"rows":[{"TRANSACTION_DATE":"2026-07-15","Transaction_Type":"Signing","TRANSACTION_DESCRIPTION":"Lakers signed LeBron James.","TEAM_SLUG":"lakers","TEAM_ID":"1610612747","PLAYER_SLUG":"lebron-james","PLAYER_ID":"2544"},{"TRANSACTION_DATE":"2026-02-01","Transaction_Type":"Trade","TRANSACTION_DESCRIPTION":"Old-season row that must be dropped.","TEAM_SLUG":"lakers","TEAM_ID":"1610612747","PLAYER_SLUG":"lebron-james","PLAYER_ID":"2544"}]}})");
        auto q = find_quote(s, "points", "LeBron James");
        const double before = number(evaluate_quote(q, s)["probability"]);
        parse_player_movement(doc, s, now_iso(), "2026-27");
        check(s["transactions"].size() == 1, "Only current-season transactions retained");
        check(s["transactions"][0]["teamAbbr"] == "LAL", "Transaction team slug mapped");
        check(s["players"][normalize_name("LeBron James")]["latestTransaction"]["type"] == "Signing",
              "Latest transaction attached as context");
        auto row = evaluate_quote(q, s);
        near(number(row["probability"]), before);
        check(row.contains("latestTransaction"), "Panel carries transaction context");
        auto dashboard = build_dashboard(s);
        check(dashboard["transactions"].size() == 1 && dashboard.contains("leagueContext"),
              "Dashboard carries the compact league desk");
    });
    test("Official season datasets enrich players and teams as context only", [&] {
        auto s = demo;
        auto result_set = [](const std::string &name, const std::string &headers, const std::string &row) {
            return json::parse("{\"resultSets\":[{\"name\":\"" + name + "\",\"headers\":[" + headers +
                               "],\"rowSet\":[[" + row + "]]}]}");
        };
        parse_player_index(result_set("PlayerIndex",
                                      "\"PERSON_ID\",\"PLAYER_FIRST_NAME\",\"PLAYER_LAST_NAME\",\"PLAYER_SLUG\",\"TEAM_ABBREVIATION\",\"TEAM_ID\",\"TEAM_SLUG\",\"JERSEY_NUMBER\",\"POSITION\",\"HEIGHT\",\"WEIGHT\",\"COLLEGE\",\"COUNTRY\",\"DRAFT_YEAR\",\"DRAFT_ROUND\",\"DRAFT_NUMBER\",\"ROSTER_STATUS\",\"PTS\",\"REB\",\"AST\",\"IS_DEFUNCT\"",
                                      "\"201939\",\"Stephen\",\"Curry\",\"stephen_curry\",\"GSW\",\"1610612744\",\"warriors\",\"30\",\"G\",\"6-2\",\"185\",\"Davidson\",\"USA\",\"2009\",\"1\",\"7\",1,27.6,4.4,6.5,0"),
                           s, now_iso());
        parse_player_clutch(result_set("PlayerClutch",
                                       "\"PLAYER_NAME\",\"GP\",\"W\",\"L\",\"W_PCT\",\"MIN\",\"FG_PCT\",\"FG3_PCT\",\"FT_PCT\",\"REB\",\"AST\",\"TOV\",\"STL\",\"BLK\",\"PTS\",\"PLUS_MINUS\",\"NBA_FANTASY_PTS\",\"DD2\",\"TD3\"",
                                       "\"Stephen Curry\",20,12,8,0.6,95.0,0.48,0.42,0.91,15,22,9,4,1,210,18,260.5,0,0"),
                            s, now_iso());
        parse_player_hustle(result_set("HustleStatsPlayer",
                                       "\"PLAYER_NAME\",\"G\",\"MIN\",\"CONTESTED_SHOTS\",\"CONTESTED_SHOTS_2PT\",\"CONTESTED_SHOTS_3PT\",\"DEFLECTIONS\",\"CHARGES_DRAWN\",\"SCREEN_ASSISTS\",\"SCREEN_AST_PTS\",\"LOOSE_BALLS_RECOVERED\",\"OFF_LOOSE_BALLS_RECOVERED\",\"DEF_LOOSE_BALLS_RECOVERED\",\"BOX_OUTS\",\"OFF_BOXOUTS\",\"DEF_BOXOUTS\"",
                                       "\"Stephen Curry\",55,1870,320,210,110,140,3,18,41,38,15,23,12,4,8"),
                            s, now_iso());
        parse_player_estimated_metrics(result_set("PlayerEstimatedMetrics",
                                                  "\"PLAYER_NAME\",\"GP\",\"W\",\"L\",\"W_PCT\",\"MIN\",\"E_OFF_RATING\",\"E_DEF_RATING\",\"E_NET_RATING\",\"E_AST_RATIO\",\"E_OREB_PCT\",\"E_DREB_PCT\",\"E_REB_PCT\",\"E_TOV_PCT\",\"E_USG_PCT\",\"E_PACE\"",
                                                  "\"Stephen Curry\",55,35,20,0.636,34.1,118.2,110.4,7.8,24.5,3.1,12.4,7.8,11.2,0.31,99.5"),
                                       s, now_iso());
        parse_player_tracking_shots(result_set("PlayerTrackingShots",
                                               "\"PLAYER_NAME\",\"GP\",\"G\",\"PLAYER_LAST_TEAM_ABBREVIATION\",\"FGA_FREQUENCY\",\"FGM\",\"FGA\",\"FG_PCT\",\"EFG_PCT\",\"FG2A_FREQUENCY\",\"FG2M\",\"FG2A\",\"FG2_PCT\",\"FG3A_FREQUENCY\",\"FG3M\",\"FG3A\",\"FG3_PCT\"",
                                               "\"Stephen Curry\",55,55,\"GSW\",1.0,9.8,20.1,0.487,0.582,0.52,5.1,9.9,0.515,0.48,4.7,10.2,0.408"),
                                    s, now_iso());
        parse_player_tracking_defense(result_set("PlayerTrackingDefense",
                                                 "\"PLAYER_NAME\",\"GP\",\"G\",\"PLAYER_LAST_TEAM_ABBREVIATION\",\"PLAYER_POSITION\",\"FREQ\",\"D_FGM\",\"D_FGA\",\"D_FG_PCT\",\"NORMAL_FG_PCT\",\"PCT_PLUSMINUS\"",
                                                 "\"Stephen Curry\",55,55,\"GSW\",\"G\",0.18,6.1,13.9,0.439,0.462,-0.023"),
                                      s, now_iso());
        parse_team_clutch(result_set("TeamClutch",
                                     "\"TEAM_NAME\",\"TEAM_ABBREVIATION\",\"GP\",\"W\",\"L\",\"W_PCT\",\"MIN\",\"FG_PCT\",\"FG3_PCT\",\"FT_PCT\",\"REB\",\"AST\",\"TOV\",\"STL\",\"BLK\",\"PTS\",\"PLUS_MINUS\"",
                                     "\"Golden State Warriors\",\"GSW\",25,15,10,0.6,125.0,0.47,0.4,0.8,110,80,30,15,10,560,30"),
                          s, now_iso());
        parse_team_hustle(result_set("HustleStatsTeam",
                                     "\"TEAM_NAME\",\"TEAM_ABBREVIATION\",\"MIN\",\"CONTESTED_SHOTS\",\"CONTESTED_SHOTS_2PT\",\"CONTESTED_SHOTS_3PT\",\"DEFLECTIONS\",\"CHARGES_DRAWN\",\"SCREEN_ASSISTS\",\"SCREEN_AST_PTS\",\"LOOSE_BALLS_RECOVERED\",\"OFF_LOOSE_BALLS_RECOVERED\",\"DEF_LOOSE_BALLS_RECOVERED\",\"BOX_OUTS\",\"OFF_BOXOUTS\",\"DEF_BOXOUTS\"",
                                     "\"Golden State Warriors\",\"GSW\",12000,2600,1700,900,1250,25,480,1050,300,120,180,900,350,550"),
                          s, now_iso());
        parse_team_estimated_metrics(result_set("TeamEstimatedMetrics",
                                                "\"TEAM_NAME\",\"GP\",\"W\",\"L\",\"W_PCT\",\"MIN\",\"E_OFF_RATING\",\"E_DEF_RATING\",\"E_NET_RATING\",\"E_PACE\",\"E_AST_RATIO\",\"E_OREB_PCT\",\"E_DREB_PCT\",\"E_REB_PCT\",\"E_TM_TOV_PCT\"",
                                                "\"Golden State Warriors\",55,35,20,0.636,2640,117.1,111.3,5.8,99.8,26.1,24.5,72.1,48.3,0.125"),
                                     s, now_iso());
        parse_team_tracking_defense(result_set("TeamTrackingDefense",
                                               "\"TEAM_NAME\",\"TEAM_ABBREVIATION\",\"GP\",\"G\",\"FREQ\",\"D_FGM\",\"D_FGA\",\"D_FG_PCT\",\"NORMAL_FG_PCT\",\"PCT_PLUSMINUS\"",
                                               "\"Golden State Warriors\",\"GSW\",55,55,1.0,41.2,88.5,0.466,0.471,-0.005"),
                                    s, now_iso());
        parse_team_opponent_shots(result_set("TeamOpponentShots",
                                             "\"TEAM_NAME\",\"TEAM_ABBREVIATION\",\"GP\",\"G\",\"FGA_FREQUENCY\",\"FGM\",\"FGA\",\"FG_PCT\",\"EFG_PCT\",\"FG2_PCT\",\"FG3_PCT\"",
                                             "\"Golden State Warriors\",\"GSW\",55,55,1.0,40.9,87.9,0.465,0.531,0.512,0.358"),
                                  s, now_iso());
        auto lineups = json::parse(
            R"({"resultSets":[{"name":"Lineups","headers":["TEAM_NAME","TEAM_ABBREVIATION","GROUP_NAME","GROUP_ID","GP","W","L","W_PCT","MIN","FG_PCT","FG3_PCT","FT_PCT","REB","AST","TOV","STL","BLK","PTS","PLUS_MINUS"],"rowSet":[["Golden State Warriors","GSW","Unit A","111",40,25,15,0.625,300.0,0.49,0.4,0.8,180,120,50,20,10,480,45],["Golden State Warriors","GSW","Unit B","222",50,30,20,0.6,512.5,0.47,0.38,0.79,210,140,60,22,12,560,38]]}]})");
        parse_team_top_lineups(lineups, s, now_iso());
        check(s["players"][normalize_name("Stephen Curry")]["playerIndex"]["personId"] == "201939",
              "PlayerIndex crosswalk attached");
        check(s["players"][normalize_name("Stephen Curry")]["clutch"]["pts"] == 210, "Player clutch totals");
        check(s["players"][normalize_name("Stephen Curry")]["hustle"]["deflections"] == 140,
              "Player hustle season totals");
        near(number(s["players"][normalize_name("Stephen Curry")]["estimatedMetrics"]["eNetRating"]), 7.8);
        near(number(s["teams"]["GSW"]["topLineup"]["minutesPerGame"]), 10.25);
        check(s["teams"]["GSW"]["topLineup"]["groupName"] == "Unit B", "Most-used lineup kept");
        auto q = find_quote(s, "points", "Stephen Curry");
        const double before = number(evaluate_quote(q, demo)["probability"]);
        auto row = evaluate_quote(q, s);
        near(number(row["probability"]), before);
        check(row.contains("playerProfile") && row["playerProfile"]["height"] == "6-2",
              "Panel carries the PlayerIndex profile");
        for (const auto &label : {"Height", "Clutch points", "Season deflections", "Estimated net rating",
                                  "Tracking eFG %", "Defended FG differential"}) {
            bool seen = false;
            for (const auto &f : row["features"])
                if (f.value("label", "") == label && f.value("role", "") == "context")
                    seen = true;
            check(seen, std::string("Context feature shown: ") + label);
        }
        json mlq;
        for (const auto &candidate : s["quotes"])
            if (candidate.value("market", "") == "moneyline" && candidate.value("gameId", "") == "demo-2" &&
                candidate.value("side", "") == "home")
                mlq = candidate;
        auto ml = evaluate_quote(mlq, s);
        bool team_seen = false;
        for (const auto &f : ml["features"])
            if (f.value("label", "").find("estimated net rating") != std::string::npos &&
                f.value("role", "") == "context")
                team_seen = true;
        check(team_seen, "Team estimated metrics shown as context on moneyline");
        auto dashboard = build_dashboard(s);
        bool carried = false;
        for (const auto &t : dashboard["teams"])
            if (t.value("abbr", "") == "GSW" && t.contains("estimatedMetrics") && t.contains("topLineup"))
                carried = true;
        check(carried, "Dashboard carries compact team league context");
    });
    test("NBA CDN odds parser skips null-named books and null outcome fields", [&] {
        // Real Sportradar payloads carry explicit nulls; value(key, "") used to
        // throw type_error.302 on them and abort the whole refresh.
        auto s = empty_snapshot("live");
        const auto start = iso_at(parse_time(now_iso()) + 3600);
        s["games"].push_back({{"id", "g9"}, {"home", "LAL"}, {"away", "GSW"}, {"startsAt", start},
                              {"officialGameId", "0022600010"}});
        auto doc = json::parse(
            R"({"games":[{"gameId":"0022600010","markets":[{"name":"2way","books":[{"name":null,"outcomes":[{"type":"home","odds":"1.91"}]},{"name":"NullSafe Books","outcomes":[{"type":null,"odds":"1.91"},{"type":"home","odds":null},{"type":"home","odds":"2.00"},{"type":"away","odds":"1.80"}]}]},{"name":null,"books":[{"name":"Ignored","outcomes":[{"type":"home","odds":"9.99"}]}]}]}]})");
        parse_nba_cdn_odds(doc, s, now_iso());
        check(s["quotes"].size() == 2, "Only well-formed outcomes become quotes; nulls never throw");
        check(s["quotes"][0].value("book", "") == "NullSafe Books" &&
                  s["quotes"][1].value("book", "") == "NullSafe Books",
              "Quoted book keeps its name");
        check(s["quotes"][0]["americanOdds"] == 100 && s["quotes"][1]["americanOdds"] == -125,
              "Surviving prices still convert (2.00 -> +100, 1.80 -> -125)");
    });
    test("Pinnacle parsers skip participants and markets with null fields", [&] {
        auto s = empty_snapshot("live");
        const auto start = iso_at(parse_time(now_iso()) + 3600);
        s["games"].push_back({{"id", "g10"}, {"home", "LAL"}, {"away", "GSW"}, {"startsAt", start}});
        json matchups = json::parse(
            R"([{"id":7,"type":"matchup","startTime":")" + start +
            R"(","participants":[{"id":71,"name":null,"alignment":"home"},{"id":72,"name":"Los Angeles Lakers","alignment":"home"},{"id":73,"name":"Golden State Warriors","alignment":"away"},{"id":74,"name":"Mystery Team","alignment":null}]}])");
        json markets = json::parse(
            R"([{"matchupId":7,"key":"s;0;m","status":null,"prices":[{"participantId":72,"price":-120},{"participantId":73,"price":100}]},{"matchupId":7,"key":null,"prices":[{"participantId":72,"price":-110}]}])");
        parse_pinnacle_markets(matchups, markets, s, now_iso());
        check(s["quotes"].size() == 2, "Null names/alignments/keys are skipped; valid prices still import");
        check(s["quotes"][0]["americanOdds"] == -120 && s["quotes"][1]["americanOdds"] == 100,
              "Exact prices survive null neighbours");
    });
    test("Pinnacle guest requests carry the published site key, others do not", [] {
        auto has = [](const std::vector<std::pair<std::string, std::string>> &headers,
                      const std::string &name, const std::string &value) {
            for (const auto &h : headers)
                if (h.first == name && h.second == value)
                    return true;
            return false;
        };
        const auto pinnacle = public_request_headers(
            "https://guest.api.arcadia.pinnacle.com/0.1/leagues/487/matchups?brandId=0");
        check(has(pinnacle, "X-API-Key", "CmX2KcMrXuFmNg6YFbmTxE0y9CIrOi0R"),
              "Pinnacle guest key header sent (keyless calls now return 404)");
        check(has(pinnacle, "Referer", "https://www.pinnacle.com/"), "Pinnacle referer sent");
        std::string device_uuid;
        for (const auto &h : pinnacle)
            if (h.first == "X-Device-UUID")
                device_uuid = h.second;
        check(device_uuid.size() == 35 && device_uuid[8] == '-' && device_uuid[17] == '-' &&
                  device_uuid[26] == '-',
              "Pinnacle requests carry an anonymous device UUID like the public site's client");
        std::string device_uuid_again;
        for (const auto &h :
             public_request_headers("https://guest.api.arcadia.pinnacle.com/0.1/sports/4/leagues"))
            if (h.first == "X-Device-UUID")
                device_uuid_again = h.second;
        check(!device_uuid_again.empty() && device_uuid_again == device_uuid,
              "Device UUID is stable within the process");
        const auto stats = public_request_headers("https://stats.nba.com/stats/leaguestandingsv3?LeagueID=00");
        check(!has(stats, "X-API-Key", "CmX2KcMrXuFmNg6YFbmTxE0y9CIrOi0R"),
              "Guest key is never sent to other hosts");
        bool stats_have_uuid = false;
        for (const auto &h : stats)
            if (h.first == "X-Device-UUID")
                stats_have_uuid = true;
        check(!stats_have_uuid, "Device UUID is never sent to other hosts");
        check(allowed_data_url(
                  "https://guest.api.arcadia.pinnacle.com/0.1/leagues/487/matchups?brandId=0"),
              "Pinnacle brandId URL stays allowlisted");
    });
    test("API-Basketball key is sent only to its fixed host from the environment", [] {
        const char *existing = std::getenv("API_BASKETBALL_KEY");
        const bool had_existing = existing != nullptr;
        const std::string saved = existing ? existing : "";
#ifdef _WIN32
        _putenv_s("API_BASKETBALL_KEY", "fixture-key-not-a-credential");
#else
        setenv("API_BASKETBALL_KEY", "fixture-key-not-a-credential", 1);
#endif
        const auto basketball =
            public_request_headers("https://v1.basketball.api-sports.io/odds?league=12");
        bool sent_to_basketball = false;
        for (const auto &header : basketball)
            if (header.first == "x-apisports-key" && header.second == "fixture-key-not-a-credential")
                sent_to_basketball = true;
        check(sent_to_basketball, "API key is added from the environment");
        const auto other = public_request_headers("https://site.api.espn.com/apis/site/v2/sports/basketball/nba");
        check(std::none_of(other.begin(), other.end(), [](const auto &header) {
                  return header.first == "x-apisports-key";
              }),
              "API-Sports credential is never sent to another provider");
        const auto lookalike =
            public_request_headers("https://v1.basketball.api-sports.io.evil.test/odds");
        check(std::none_of(lookalike.begin(), lookalike.end(), [](const auto &header) {
                  return header.first == "x-apisports-key";
              }),
              "Credential is never attached to a lookalike API-Sports host");
#ifdef _WIN32
        _putenv_s("API_BASKETBALL_KEY", "fixture-key\r\nx-injected: yes");
#else
        setenv("API_BASKETBALL_KEY", "fixture-key\r\nx-injected: yes", 1);
#endif
        const auto malformed =
            public_request_headers("https://v1.basketball.api-sports.io/odds?league=12");
        check(std::none_of(malformed.begin(), malformed.end(), [](const auto &header) {
                  return header.first == "x-apisports-key";
              }),
              "Header-injected environment values are rejected");
#ifdef _WIN32
        _putenv_s("API_BASKETBALL_KEY", had_existing ? saved.c_str() : "");
#else
        if (had_existing)
            setenv("API_BASKETBALL_KEY", saved.c_str(), 1);
        else
            unsetenv("API_BASKETBALL_KEY");
#endif
    });
    test("SharpAPI key is sent only to its fixed host from the environment", [] {
        const char *existing = std::getenv("SHARPAPI_KEY");
        const bool had_existing = existing != nullptr;
        const std::string saved = existing ? existing : "";
#ifdef _WIN32
        _putenv_s("SHARPAPI_KEY", "sharpapi-fixture-not-a-credential");
#else
        setenv("SHARPAPI_KEY", "sharpapi-fixture-not-a-credential", 1);
#endif
        const auto sharp = public_request_headers("https://api.sharpapi.io/api/v1/odds?league=nba");
        check(std::any_of(sharp.begin(), sharp.end(), [](const auto &header) {
                  return header.first == "X-API-Key" && header.second == "sharpapi-fixture-not-a-credential";
              }),
              "SharpAPI key is added as the documented header");
        const auto other = public_request_headers("https://site.api.espn.com/apis/site/v2/sports/basketball/nba");
        check(std::none_of(other.begin(), other.end(), [](const auto &header) {
                  return header.first == "X-API-Key";
              }),
              "SharpAPI key is never sent to another provider");
        const auto lookalike = public_request_headers("https://api.sharpapi.io.evil.test/api/v1/odds");
        check(std::none_of(lookalike.begin(), lookalike.end(), [](const auto &header) {
                  return header.first == "X-API-Key";
              }),
              "SharpAPI key is never attached to a lookalike host");
#ifdef _WIN32
        _putenv_s("SHARPAPI_KEY", "fixture-key\r\nx-injected: yes");
#else
        setenv("SHARPAPI_KEY", "fixture-key\r\nx-injected: yes", 1);
#endif
        const auto malformed = public_request_headers("https://api.sharpapi.io/api/v1/odds");
        check(std::none_of(malformed.begin(), malformed.end(), [](const auto &header) {
                  return header.first == "X-API-Key";
              }),
              "Header-injected environment values are rejected");
#ifdef _WIN32
        _putenv_s("SHARPAPI_KEY", had_existing ? saved.c_str() : "");
#else
        if (had_existing)
            setenv("SHARPAPI_KEY", saved.c_str(), 1);
        else
            unsetenv("SHARPAPI_KEY");
#endif
    });
    test("Optional provider keys stay in exact documented host headers", [] {
        auto set_env = [](const char *name, const char *value) {
#ifdef _WIN32
            _putenv_s(name, value ? value : "");
#else
            if (value)
                setenv(name, value, 1);
            else
                unsetenv(name);
#endif
        };
        const std::vector<std::string> names = {"RAPIDAPI_KEY", "RAPIDAPI_HOST", "THESPORTSDB_KEY", "SPORTSCORE_KEY"};
        std::vector<std::pair<bool, std::string>> saved;
        for (const auto &name : names) {
            const char *value = std::getenv(name.c_str());
            saved.emplace_back(value != nullptr, value ? value : "");
        }
        set_env("RAPIDAPI_KEY", "rapidapi-fixture-not-a-credential");
        set_env("RAPIDAPI_HOST", nullptr);
        set_env("THESPORTSDB_KEY", "sportsdb-fixture-not-a-credential");
        set_env("SPORTSCORE_KEY", "sportscore-fixture-not-a-credential");
        auto has_header = [](const std::vector<std::pair<std::string, std::string>> &headers,
                             const std::string &name, const std::string &value) {
            return std::any_of(headers.begin(), headers.end(), [&](const auto &header) {
                return header.first == name && header.second == value;
            });
        };
        const auto rapid_unconfirmed = public_request_headers("https://sportsbook-api2.p.rapidapi.com/v0/competitions");
        check(std::none_of(rapid_unconfirmed.begin(), rapid_unconfirmed.end(),
                           [](const auto &h) { return h.first == "X-RapidAPI-Key"; }),
              "The RapidAPI key is withheld until its product host is confirmed");
        set_env("RAPIDAPI_HOST", "sportsbook-api2.p.rapidapi.com");
        const auto rapid = public_request_headers("https://sportsbook-api2.p.rapidapi.com/v0/competitions");
        check(!has_header(rapid, "X-RapidAPI-Key", "rapidapi-fixture-not-a-credential") &&
                  !has_header(rapid, "X-RapidAPI-Host", "sportsbook-api2.p.rapidapi.com"),
              "Removed provider receives no credentials even with host confirmation");
        set_env("RAPIDAPI_HOST", "wrong-product.p.rapidapi.com");
        const auto rapid_mismatch = public_request_headers("https://sportsbook-api2.p.rapidapi.com/v0/competitions");
        check(std::none_of(rapid_mismatch.begin(), rapid_mismatch.end(),
                           [](const auto &h) { return h.first == "X-RapidAPI-Key"; }),
              "A different RapidAPI host cannot receive the product key");
        set_env("RAPIDAPI_HOST", "sportsbook-api2.p.rapidapi.com");
        const auto sportsdb = public_request_headers("https://www.thesportsdb.com/api/v2/json/schedule/next/league/4387");
        check(has_header(sportsdb, "X-API-KEY", "sportsdb-fixture-not-a-credential"),
              "TheSportsDB V2 key uses its documented header");
        const auto score = public_request_headers("https://sportscore.com/api/v1/fixtures/?sport=basketball");
        check(!has_header(score, "X-Api-Key", "sportscore-fixture-not-a-credential"),
              "Removed speculative SportScore adapter sends no credential");
        for (const auto &url : {
                 "https://api.server.nbaapi.com/api/games?date=2026-10-13",
                 "https://site.api.espn.com/apis/site/v2/sports/basketball/nba/scoreboard",
                 "https://sportsbook-api2.p.rapidapi.com.evil.test/v0/competitions",
                 "https://www.thesportsdb.com.evil.test/api/v2/json/schedule/next/league/4387",
                 "https://sportscore.com.evil.test/api/v1/fixtures"}) {
            const auto headers = public_request_headers(url);
            check(!has_header(headers, "X-RapidAPI-Key", "rapidapi-fixture-not-a-credential") &&
                      !has_header(headers, "X-API-KEY", "sportsdb-fixture-not-a-credential") &&
                      !has_header(headers, "X-Api-Key", "sportscore-fixture-not-a-credential"),
                  "Optional keys are never sent to keyless, unrelated or lookalike hosts");
        }
        set_env("RAPIDAPI_KEY", "rapidapi-fixture-not-a-credential");
        set_env("RAPIDAPI_HOST", "sportsbook-api2.p.rapidapi.com\r\nx-injected: yes");
        const auto rapid_host_bad = public_request_headers("https://sportsbook-api2.p.rapidapi.com/v0/competitions");
        check(std::none_of(rapid_host_bad.begin(), rapid_host_bad.end(),
                           [](const auto &h) { return h.first == "X-RapidAPI-Key"; }),
              "CR/LF-injected host confirmations are rejected");
        set_env("RAPIDAPI_KEY", "fixture-key\r\nx-injected: yes");
        set_env("RAPIDAPI_HOST", "sportsbook-api2.p.rapidapi.com");
        set_env("THESPORTSDB_KEY", "fixture-key\r\nx-injected: yes");
        set_env("SPORTSCORE_KEY", "fixture-key\r\nx-injected: yes");
        const auto rapid_bad = public_request_headers("https://sportsbook-api2.p.rapidapi.com/v0/competitions");
        const auto sportsdb_bad = public_request_headers("https://www.thesportsdb.com/api/v2/json/schedule/next/league/4387");
        const auto score_bad = public_request_headers("https://sportscore.com/api/v1/fixtures");
        check(std::none_of(rapid_bad.begin(), rapid_bad.end(), [](const auto &h) { return h.first == "X-RapidAPI-Key"; }) &&
                  std::none_of(sportsdb_bad.begin(), sportsdb_bad.end(), [](const auto &h) { return h.first == "X-API-KEY"; }) &&
                  std::none_of(score_bad.begin(), score_bad.end(), [](const auto &h) { return h.first == "X-Api-Key"; }),
              "CR/LF-injected environment values are rejected");
        for (std::size_t i = 0; i < names.size(); ++i)
            set_env(names[i].c_str(), saved[i].first ? saved[i].second.c_str() : nullptr);
    });
    test("New price hosts are allowlisted and lookalikes are not", [] {
        check(allowed_data_url("https://gamma-api.polymarket.com/events?tag_slug=nba"), "Polymarket");
        check(allowed_data_url("https://external-api.kalshi.com/trade-api/v2/markets"), "Kalshi");
        check(allowed_data_url("https://guest.api.arcadia.pinnacle.com/0.1/sports"), "Pinnacle");
        check(allowed_data_url("https://v1.basketball.api-sports.io/odds?league=12"), "API-Sports exact host");
        check(!allowed_data_url("https://v1.basketball.api-sports.io.evil.test/odds"),
              "API-Sports lookalike host");
        check(allowed_data_url("https://api.sharpapi.io/api/v1/odds?league=nba"), "SharpAPI exact host");
        check(!allowed_data_url("https://api.sharpapi.io.evil.test/api/v1/odds"),
              "SharpAPI lookalike host");
        check(!allowed_data_url("https://sportsbook-api2.p.rapidapi.com/v0/competitions"), "Removed unverified API2 host");
        check(!allowed_data_url("https://sportsbook-api2.p.rapidapi.com.evil.test/v0/competitions"),
              "Sportsbook API2 lookalike host");
        check(allowed_data_url("https://www.thesportsdb.com/api/v2/json/schedule/next/league/4387"),
              "TheSportsDB exact host");
        check(!allowed_data_url("https://www.thesportsdb.com.evil.test/api/v2/json/schedule/next/league/4387"),
              "TheSportsDB lookalike host");
        check(!allowed_data_url("https://sportscore.com/api/v1/fixtures/?sport=basketball"), "Removed SportScore route");
        check(!allowed_data_url("https://sportscore.com.evil.test/api/v1/fixtures"), "SportScore lookalike host");
        check(allowed_data_url("https://api.server.nbaapi.com/api/games?date=2026-10-13"), "NBA_Go exact host");
        check(!allowed_data_url("https://api.server.nbaapi.com.evil.test/api/games"), "NBA_Go lookalike host");
        check(!allowed_data_url("https://gamma-api.polymarket.com.evil.test/events"), "Lookalike host");
        check(!allowed_data_url("https://kalshi.com/trade-api/v2/markets"), "Wrong Kalshi host");
    });
    std::cout << "\n" << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
