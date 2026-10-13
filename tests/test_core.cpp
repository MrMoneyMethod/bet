#include "core.hpp"
#include "feeds.hpp"
#include <cmath>
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
        rows["observations"][0]["predictedAt"] = "2026-10-01T02:00:00Z";
        throws([&] { score_backtest(rows); });
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
    std::cout << "\n" << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
