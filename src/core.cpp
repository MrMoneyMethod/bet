#include "core.hpp"
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <map>
#include <numeric>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>

namespace edgecourt {
namespace {
const std::set<std::string> markets = {"moneyline",    "points",    "rebounds",      "assists",
                                       "threes",       "pra",       "double_double", "triple_double",
                                       "first_basket", "team_total"};
const std::set<std::string> teams = {"ATL", "BOS", "BKN", "CHA", "CHI", "CLE", "DAL", "DEN", "DET", "GSW",
                                     "HOU", "IND", "LAC", "LAL", "MEM", "MIA", "MIL", "MIN", "NOP", "NYK",
                                     "OKC", "ORL", "PHI", "PHX", "POR", "SAC", "SAS", "TOR", "UTA", "WAS"};
const std::map<std::string, std::string> labels = {
    {"moneyline", "Moneyline"},         {"points", "Points"},
    {"rebounds", "Rebounds"},           {"assists", "Assists"},
    {"threes", "3-pointers made"},      {"pra", "Points + rebounds + assists"},
    {"double_double", "Double-double"}, {"triple_double", "Triple-double"},
    {"first_basket", "First basket"},   {"team_total", "Team total"}};
long long epoch_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
double clamp(double v, double lo, double hi) {
    return std::max(lo, std::min(hi, v));
}
double phi(double v) {
    return 0.5 * std::erfc(-v / std::sqrt(2.0));
}
void feature(json &result, const std::string &label, double value, const std::string &unit = "",
             const std::string &role = "used") {
    result["features"].push_back(
        {{"label", label}, {"value", std::round(value * 1000) / 1000}, {"unit", unit}, {"role", role}});
}
json blocked(json result, const std::string &reason) {
    result["availability"] = "unavailable";
    result["reason"] = reason;
    result["probability"] = nullptr;
    result["pushProbability"] = nullptr;
    result["ev"] = nullptr;
    result["conservativeEv"] = nullptr;
    result["kellyFraction"] = nullptr;
    result["edge"] = nullptr;
    result["projected"] = nullptr;
    result["samplingInterval"] = nullptr;
    result["eligible"] = false;
    return result;
}
bool is_integer(double v) {
    return std::abs(v - std::round(v)) < 1e-8;
}
json normal_outcomes(double mean, double sd, double line, const std::string &side) {
    const double below = phi((std::ceil(line) - 0.5 - mean) / sd);
    const double above = 1.0 - phi((std::floor(line) + 0.5 - mean) / sd);
    const double push = is_integer(line) ? std::max(0.0, 1.0 - below - above) : 0.0;
    return {{"win", side == "over" ? above : below}, {"push", push}};
}
void require_string(const json &obj, const char *key, size_t limit = 120) {
    if (!obj.contains(key) || !obj[key].is_string() || obj[key].get<std::string>().empty() ||
        obj[key].get<std::string>().size() > limit)
        throw std::invalid_argument(std::string("Required text field: ") + key);
}
void require_identifier(const json &obj, const char *key) {
    require_string(obj, key);
    if (!std::isalnum(static_cast<unsigned char>(obj[key].get<std::string>()[0])))
        throw std::invalid_argument(std::string("Identifier must start with a letter or digit: ") + key);
    for (unsigned char c : obj[key].get<std::string>())
        if (!(c < 128 && (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == ':')))
            throw std::invalid_argument(
                std::string(
                    "Identifier may contain only letters, digits, dots, dashes, underscores or colons: ") +
                key);
}
void require_number(const json &obj, const char *key, double lo, double hi) {
    if (!obj.contains(key) || !obj[key].is_number() || !std::isfinite(obj[key].get<double>()) ||
        obj[key].get<double>() < lo || obj[key].get<double>() > hi)
        throw std::invalid_argument(std::string("Out-of-range numeric field: ") + key);
}
void normalize_price(json &q) {
    const bool percent = q.contains("oddsPercent") && !q["oddsPercent"].is_null();
    const bool american = q.contains("americanOdds") && !q["americanOdds"].is_null();
    if (!percent && !american)
        throw std::invalid_argument("Provide oddsPercent (0.01..99.99) or legacy americanOdds");
    if (percent) {
        require_number(q, "oddsPercent", 0.01, 99.99);
        if (number(q["oddsPercent"]) <= 0 || number(q["oddsPercent"]) >= 100)
            throw std::invalid_argument("oddsPercent must be strictly between 0 and 100");
    }
    if (american) {
        require_number(q, "americanOdds", -100000, 100000);
        if (!q["americanOdds"].is_number_integer())
            throw std::invalid_argument("americanOdds must be an integer");
        const double percent_value = 100 * implied_probability(q["americanOdds"]);
        if (percent && std::abs(number(q["oddsPercent"]) - percent_value) > 1e-7)
            throw std::invalid_argument(
                "oddsPercent and americanOdds disagree; provide just one price format");
        q["oddsPercent"] = percent_value;
    } else {
        q["americanOdds"] = nullptr;
    }
}
double ev_at_decimal(double win, double push, double decimal) {
    if (!std::isfinite(win) || !std::isfinite(push) || win < 0 || push < 0 || win + push > 1.0 + 1e-8)
        throw std::invalid_argument("Invalid outcome probabilities");
    return 100.0 * (win * decimal + push - 1.0);
}
double kelly_at_decimal(double win, double push, double decimal, double fraction) {
    const double b = decimal - 1, loss = std::max(0.0, 1.0 - win - push);
    if (win + loss < 1e-9)
        return 0;
    return clamp(fraction * (b * win - loss) / (b * (win + loss)), 0.0, 0.02);
}
std::vector<std::vector<std::string>> csv_rows(const std::string &input) {
    std::vector<std::vector<std::string>> rows;
    std::vector<std::string> row;
    std::string field;
    bool quoted = false;
    for (size_t i = 0; i < input.size(); ++i) {
        char c = input[i];
        if (c == '"') {
            if (quoted && i + 1 < input.size() && input[i + 1] == '"') {
                field += '"';
                ++i;
            } else if (field.empty() || quoted)
                quoted = !quoted;
            else
                throw std::invalid_argument("Malformed CSV quote");
        } else if (c == ',' && !quoted) {
            row.push_back(field);
            field.clear();
        } else if ((c == '\n' || c == '\r') && !quoted) {
            if (c == '\r' && i + 1 < input.size() && input[i + 1] == '\n')
                ++i;
            row.push_back(field);
            field.clear();
            if (!(row.size() == 1 && row[0].empty()))
                rows.push_back(row);
            row.clear();
        } else
            field += c;
    }
    if (quoted)
        throw std::invalid_argument("Unclosed CSV quote");
    if (!field.empty() || !row.empty()) {
        row.push_back(field);
        rows.push_back(row);
    }
    return rows;
}
} // namespace

std::string iso_at(long long epoch) {
    std::time_t time = static_cast<std::time_t>(epoch);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &time);
#else
    gmtime_r(&time, &tm);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof buffer, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buffer;
}
std::string now_iso() {
    return iso_at(epoch_now());
}
long long parse_time(const std::string &value) {
    // Import timestamps are intentionally UTC, without ambiguous local offsets.
    std::tm tm{};
    std::istringstream in(value);
    if (value.size() == 10)
        in >> std::get_time(&tm, "%Y-%m-%d");
    else if (value.size() == 20 && value.back() == 'Z')
        in >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    else
        return 0;
    if (in.fail())
        return 0;
    const std::tm original = tm;
#ifdef _WIN32
    auto result = static_cast<long long>(_mkgmtime(&tm));
#else
    auto result = static_cast<long long>(timegm(&tm));
#endif
    if (result <= 0 || tm.tm_year != original.tm_year || tm.tm_mon != original.tm_mon ||
        tm.tm_mday != original.tm_mday || tm.tm_hour != original.tm_hour || tm.tm_min != original.tm_min ||
        tm.tm_sec != original.tm_sec)
        return 0;
    return result;
}
std::string normalize_name(std::string value) {
    for (const auto &pair : std::vector<std::pair<std::string, std::string>>{
             {"á", "a"}, {"à", "a"}, {"â", "a"}, {"ä", "a"}, {"ã", "a"}, {"å", "a"}, {"ā", "a"}, {"ą", "a"},
             {"Á", "a"}, {"À", "a"}, {"Ä", "a"}, {"ć", "c"}, {"č", "c"}, {"ç", "c"}, {"Ć", "c"}, {"Č", "c"},
             {"Ç", "c"}, {"é", "e"}, {"è", "e"}, {"ê", "e"}, {"ë", "e"}, {"ē", "e"}, {"ę", "e"}, {"É", "e"},
             {"È", "e"}, {"Ê", "e"}, {"í", "i"}, {"ì", "i"}, {"î", "i"}, {"ï", "i"}, {"ī", "i"}, {"İ", "i"},
             {"ó", "o"}, {"ò", "o"}, {"ô", "o"}, {"ö", "o"}, {"ō", "o"}, {"ø", "o"}, {"Ó", "o"}, {"Ö", "o"},
             {"ú", "u"}, {"ù", "u"}, {"û", "u"}, {"ü", "u"}, {"ū", "u"}, {"ů", "u"}, {"Ú", "u"}, {"Ü", "u"},
             {"Ū", "u"}, {"ñ", "n"}, {"ń", "n"}, {"ň", "n"}, {"Ñ", "n"}, {"ś", "s"}, {"š", "s"}, {"ş", "s"},
             {"Š", "s"}, {"Ş", "s"}, {"ž", "z"}, {"ź", "z"}, {"ż", "z"}, {"Ž", "z"}, {"ł", "l"}, {"Ł", "l"},
             {"đ", "d"}, {"Đ", "d"}, {"ý", "y"}, {"ÿ", "y"}, {"ß", "ss"}}) {
        size_t pos = 0;
        while ((pos = value.find(pair.first, pos)) != std::string::npos) {
            value.replace(pos, pair.first.size(), pair.second);
            pos += pair.second.size();
        }
    }
    std::string out;
    for (unsigned char c : value)
        if (c < 128 && std::isalnum(c))
            out += static_cast<char>(std::tolower(c));
    return out;
}
std::string stable_id(const std::string &text_value) {
    uint64_t hash = 14695981039346656037ULL;
    for (unsigned char c : text_value) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    std::ostringstream out;
    out << std::hex << hash;
    return out.str();
}
std::string quote_id(const json &q) {
    return stable_id(json::array({q.value("gameId", ""), q.value("market", ""),
                                  normalize_name(q.value("player", "")), q.value("team", ""),
                                  q.value("side", ""), q.value("line", json(nullptr)), q.value("book", "")})
                         .dump());
}
double number(const json &value, double fallback) {
    if (value.is_number())
        return value.get<double>();
    if (value.is_string()) {
        try {
            size_t used = 0;
            auto str = value.get<std::string>();
            double n = std::stod(str, &used);
            if (used == str.size() && std::isfinite(n))
                return n;
        } catch (...) {
        }
    }
    return fallback;
}
std::string text(const json &value, const std::string &fallback) {
    if (value.is_string())
        return value.get<std::string>();
    if (value.is_number_integer())
        return std::to_string(value.get<long long>());
    return fallback;
}
double decimal_odds(int american) {
    if (american >= 100)
        return 1.0 + american / 100.0;
    if (american <= -100)
        return 1.0 + 100.0 / (-american);
    throw std::invalid_argument("American odds must be <= -100 or >= +100");
}
double implied_probability(int american) {
    return 1.0 / decimal_odds(american);
}
bool has_quote_price(const json &q) {
    return (q.contains("oddsPercent") && q["oddsPercent"].is_number() && number(q["oddsPercent"]) > 0 &&
            number(q["oddsPercent"]) < 100) ||
           (q.contains("americanOdds") && q["americanOdds"].is_number_integer() &&
            (number(q["americanOdds"]) <= -100 || number(q["americanOdds"]) >= 100));
}
double quote_implied_probability(const json &q) {
    if (q.contains("oddsPercent") && !q["oddsPercent"].is_null()) {
        const double p = number(q["oddsPercent"], -1) / 100;
        if (p > 0 && p < 1)
            return p;
        throw std::invalid_argument("oddsPercent must be strictly between 0 and 100");
    }
    if (q.contains("americanOdds") && q["americanOdds"].is_number_integer())
        return implied_probability(q["americanOdds"]);
    throw std::invalid_argument("Quote has no price");
}
double quote_decimal_odds(const json &q) {
    return 1 / quote_implied_probability(q);
}
double expected_value(double win, double push, int american) {
    return ev_at_decimal(win, push, decimal_odds(american));
}
double expected_value(double win, double push, const json &quote) {
    return ev_at_decimal(win, push, quote_decimal_odds(quote));
}
double fractional_kelly(double win, double push, int american, double fraction) {
    return kelly_at_decimal(win, push, decimal_odds(american), fraction);
}
double fractional_kelly(double win, double push, const json &quote, double fraction) {
    return kelly_at_decimal(win, push, quote_decimal_odds(quote), fraction);
}
json first_basket_counts(const json &player, const std::string &starts_at) {
    // Aggregate legacy counts are supported, but only dated observations can be leakage-guarded.
    if (!player.contains("firstBasketHistory"))
        return {{"games", player.value("firstBasketGames", json(0))},
                {"wins", player.value("firstBasketWins", json(0))}};
    int games = 0, wins = 0;
    for (const auto &observation : player["firstBasketHistory"])
        if (observation.value("date", "") < starts_at.substr(0, 10)) {
            ++games;
            if (observation.value("win", false))
                ++wins;
        }
    return {{"games", games}, {"wins", wins}};
}
json wilson_interval(double p, double n) {
    n = std::max(1.0, n);
    constexpr double z = 1.96;
    double d = 1 + z * z / n;
    double center = (p + z * z / (2 * n)) / d,
           spread = z * std::sqrt(p * (1 - p) / n + z * z / (4 * n * n)) / d;
    return {{"lower", clamp(center - spread, 0, 1)},
            {"upper", clamp(center + spread, 0, 1)},
            {"kind", "95% sampling interval; excludes model error"}};
}
std::string nba_season(long long epoch, bool previous) {
    const auto iso = iso_at(epoch);
    int year = std::stoi(iso.substr(0, 4));
    int month = std::stoi(iso.substr(5, 2));
    if (month < 10)
        --year;
    if (previous)
        --year;
    return std::to_string(year) + "-" + std::to_string((year + 1) % 100);
}
json empty_snapshot(const std::string &mode) {
    return {{"mode", mode},
            {"generatedAt", now_iso()},
            {"games", json::array()},
            {"players", json::object()},
            {"teams", json::object()},
            {"quotes", json::array()},
            {"lineups", json::array()},
            {"sources", json::array()},
            {"refreshing", false},
            {"revision", 0}};
}

json feature_catalog() {
    return json::array({{{"group", "Player history"},
                         {"label", "Minutes / recent rotation"},
                         {"role", "used"},
                         {"source", "NBA game logs / local historical box scores"}},
                        {{"group", "Player history"},
                         {"label", "Points per minute"},
                         {"role", "used"},
                         {"source", "NBA game logs / local historical box scores"}},
                        {{"group", "Player history"},
                         {"label", "Rebounds per minute"},
                         {"role", "used"},
                         {"source", "NBA game logs / local historical box scores"}},
                        {{"group", "Player history"},
                         {"label", "Assists per minute"},
                         {"role", "used"},
                         {"source", "NBA game logs / local historical box scores"}},
                        {{"group", "Player history"},
                         {"label", "3PM per minute"},
                         {"role", "used"},
                         {"source", "NBA game logs / local historical box scores"}},
                        {{"group", "Player history"},
                         {"label", "Steals & blocks"},
                         {"role", "used"},
                         {"source", "NBA game logs / local historical box scores"}},
                        {{"group", "Player history"},
                         {"label", "Joint P/R/A/STL/BLK covariance"},
                         {"role", "used"},
                         {"source", "Joint empirical resampling"}},
                        {{"group", "Player history"},
                         {"label", "Game recency & effective sample size"},
                         {"role", "used"},
                         {"source", "Exponential weighting"}},
                        {{"group", "Player history"},
                         {"label", "Home / away splits"},
                         {"role", "used"},
                         {"source", "NBA matchup field"}},
                        {{"group", "Player history"},
                         {"label", "Last 5 / 10 hit rates"},
                         {"role", "context"},
                         {"source", "Historical game logs"}},
                        {{"group", "Player history"},
                         {"label", "FGA / FTA / 3PA volume"},
                         {"role", "context"},
                         {"source", "NBA game logs / local historical box scores"}},
                        {{"group", "Player history"},
                         {"label", "Effective FG% / true shooting%"},
                         {"role", "context"},
                         {"source", "Derived from game logs"}},
                        {{"group", "Player history"},
                         {"label", "Turnovers / offensive rebounds / fouls"},
                         {"role", "context"},
                         {"source", "NBA game logs / local historical box scores"}},
                        {{"group", "Team environment"},
                         {"label", "Team & opponent pace"},
                         {"role", "used"},
                         {"source", "NBA advanced stats / approximate historical box-score fallback"}},
                        {{"group", "Team environment"},
                         {"label", "Offensive / defensive / net rating"},
                         {"role", "used"},
                         {"source", "NBA advanced stats / approximate historical box-score fallback"}},
                        {{"group", "Team environment"},
                         {"label", "Chronological team Elo"},
                         {"role", "used"},
                         {"source", "Paired historical team game logs"}},
                        {{"group", "Team environment"},
                         {"label", "Scoring & margin variance"},
                         {"role", "used"},
                         {"source", "Team game logs"}},
                        {{"group", "Team environment"},
                         {"label", "Rest days / back-to-back"},
                         {"role", "used"},
                         {"source", "Team game dates; moneyline only"}},
                        {{"group", "Team environment"},
                         {"label", "Home-court advantage"},
                         {"role", "used"},
                         {"source", "Fixed, documented baseline"}},
                        {{"group", "Team environment"},
                         {"label", "Team record / season type"},
                         {"role", "context"},
                         {"source", "ESPN scoreboard"}},
                        {{"group", "Availability"},
                         {"label", "Player injury / out status"},
                         {"role", "guard"},
                         {"source", "ESPN team rosters"}},
                        {{"group", "Availability"},
                         {"label", "Confirmed starter status"},
                         {"role", "guard"},
                         {"source", "Required for first basket"}},
                        {{"group", "Availability"},
                         {"label", "Historical first-FG winners"},
                         {"role", "used"},
                         {"source", "Dated play-by-play first-FG opportunities / supplied counts"}},
                        {{"group", "Availability"},
                         {"label", "Complete 10-starter first-basket field"},
                         {"role", "guard"},
                         {"source", "Explicit user-supplied lineup"}},
                        {{"group", "Live game"},
                         {"label", "Score differential"},
                         {"role", "used"},
                         {"source", "ESPN / NBA live scoreboard"}},
                        {{"group", "Live game"},
                         {"label", "Period / clock / remaining game time"},
                         {"role", "used"},
                         {"source", "ESPN / NBA live scoreboard"}},
                        {{"group", "Live game"},
                         {"label", "Live player minutes / accrued stats"},
                         {"role", "used"},
                         {"source", "ESPN summary box score"}},
                        {{"group", "Market quality"},
                         {"label", "Observed price age / event status"},
                         {"role", "guard"},
                         {"source", "Quote metadata"}},
                        {{"group", "Market quality"},
                         {"label", "Paired no-vig implied probability"},
                         {"role", "context"},
                         {"source", "Same-book matching quotes"}},
                        {{"group", "Market quality"},
                         {"label", "Push probability / settlement scope"},
                         {"role", "used"},
                         {"source", "Discrete simulated outcomes"}},
                        {{"group", "Future inputs"},
                         {"label", "Tracking touches / time of possession"},
                         {"role", "not implemented"},
                         {"source", "No stable keyless ingestion in v1"}},
                        {{"group", "Future inputs"},
                         {"label", "Referee assignments / foul tendencies"},
                         {"role", "not implemented"},
                         {"source", "No validated feed in v1"}},
                        {{"group", "Future inputs"},
                         {"label", "Travel distance / altitude / time zones"},
                         {"role", "not implemented"},
                         {"source", "Requires historical location features"}},
                        {{"group", "Future inputs"},
                         {"label", "Line movement / closing-line value"},
                         {"role", "not implemented"},
                         {"source", "Requires trustworthy historical odds"}},
                        {{"group", "Future inputs"},
                         {"label", "Teammate-on/off usage & matchup defense"},
                         {"role", "not implemented"},
                         {"source", "Requires calibrated contextual model"}}});
}

json demo_snapshot() {
    json s = empty_snapshot("demo");
    const auto now = epoch_now();
    std::mt19937 rng(20261012);
    const std::vector<std::string> order = {"BOS", "NYK", "LAL", "GSW", "DEN", "MIN"};
    const std::vector<std::string> names = {"Boston Celtics",     "New York Knicks",
                                            "Los Angeles Lakers", "Golden State Warriors",
                                            "Denver Nuggets",     "Minnesota Timberwolves"};
    const std::vector<double> offense = {118.5, 116.1, 117.4, 114.0, 120.2, 115.8},
                              defense = {111.0, 113.4, 115.1, 113.6, 115.9, 109.4},
                              pace = {97.8, 97.2, 99.1, 101.2, 98.7, 99.4};
    for (size_t t = 0; t < order.size(); ++t) {
        json logs = json::array();
        std::normal_distribution<double> noise(0, 11);
        for (int i = 0; i < 40; ++i)
            logs.push_back({{"date", iso_at(now - (i * 2 + 2) * 86400).substr(0, 10)},
                            {"points", std::round(pace[t] * offense[t] / 100 + noise(rng))},
                            {"margin", std::round((offense[t] - defense[t]) * 0.5 + noise(rng))},
                            {"home", i % 2 == 0}});
        s["teams"][order[t]] = {{"name", names[t]},
                                {"pace", pace[t]},
                                {"offRating", offense[t]},
                                {"defRating", defense[t]},
                                {"elo", 1500 + (offense[t] - defense[t]) * 14},
                                {"restDays", t % 3},
                                {"record", std::to_string(22 + static_cast<int>(t)) + "–" +
                                               std::to_string(13 + static_cast<int>(t))},
                                {"logs", logs},
                                {"dataAsOf", now_iso()},
                                {"source", "synthetic"}};
    }
    for (int i = 0; i < 3; ++i) {
        auto home = order[i * 2], away = order[i * 2 + 1];
        s["games"].push_back({{"id", "demo-" + std::to_string(i + 1)},
                              {"home", home},
                              {"away", away},
                              {"homeName", names[i * 2]},
                              {"awayName", names[i * 2 + 1]},
                              {"status", i == 2 ? "live" : "upcoming"},
                              {"startsAt", iso_at(now + (i == 2 ? -70 * 60 : (5 + i * 2) * 3600))},
                              {"period", i == 2 ? 3 : 0},
                              {"clock", i == 2 ? "8:42" : ""},
                              {"elapsedMinutes", i == 2 ? 27.3 : 0},
                              {"homeScore", i == 2 ? 61 : 0},
                              {"awayScore", i == 2 ? 57 : 0},
                              {"seasonType", "regular"},
                              {"venue", i == 0   ? "TD Garden"
                                        : i == 1 ? "Crypto.com Arena"
                                                 : "Ball Arena"},
                              {"source", "synthetic"},
                              {"dataAsOf", now_iso()}});
    }
    struct Seed {
        const char *name;
        const char *team;
        double pts, reb, ast, three, mins;
        int first;
    };
    const std::vector<Seed> seeds = {{"Jaylen Brown", "BOS", 26.2, 6.1, 4.6, 2.1, 35, 20},
                                     {"Derrick White", "BOS", 17.0, 4.5, 5.2, 3.0, 34, 8},
                                     {"Payton Pritchard", "BOS", 15.4, 3.5, 4.8, 3.1, 30, 7},
                                     {"Sam Hauser", "BOS", 10.5, 3.1, 1.2, 2.7, 26, 3},
                                     {"Neemias Queta", "BOS", 8.1, 8.4, 1.2, 0, 25, 6},
                                     {"Jalen Brunson", "NYK", 27.5, 3.0, 7.3, 2.4, 36, 14},
                                     {"Karl-Anthony Towns", "NYK", 24.7, 12.6, 3.1, 2.0, 34, 10},
                                     {"Mikal Bridges", "NYK", 18.1, 3.5, 3.4, 2.4, 36, 5},
                                     {"OG Anunoby", "NYK", 16.8, 4.6, 2.0, 2.2, 35, 4},
                                     {"Josh Hart", "NYK", 12.1, 9.2, 5.7, 1.1, 36, 3},
                                     {"Luka Dončić", "LAL", 30.4, 8.2, 8.8, 3.6, 36, 16},
                                     {"LeBron James", "LAL", 24.7, 7.9, 8.0, 2.1, 35, 14},
                                     {"Austin Reaves", "LAL", 21.1, 4.4, 5.8, 2.6, 35, 8},
                                     {"Rui Hachimura", "LAL", 13.0, 5.4, 1.4, 1.9, 31, 4},
                                     {"Deandre Ayton", "LAL", 14.5, 11.2, 1.8, 0, 30, 6},
                                     {"Stephen Curry", "GSW", 27.6, 4.4, 6.5, 4.7, 34, 15},
                                     {"Jimmy Butler", "GSW", 20.0, 5.9, 5.6, 0.8, 33, 8},
                                     {"Draymond Green", "GSW", 8.8, 6.8, 6.0, 0.9, 29, 3},
                                     {"Brandin Podziemski", "GSW", 13.4, 5.8, 3.8, 2.0, 30, 4},
                                     {"Moses Moody", "GSW", 12.4, 3.0, 1.5, 2.2, 28, 2},
                                     {"Nikola Jokić", "DEN", 29.8, 12.8, 10.1, 2.0, 36, 16},
                                     {"Jamal Murray", "DEN", 23.2, 4.0, 6.3, 2.7, 35, 11},
                                     {"Aaron Gordon", "DEN", 15.2, 6.3, 3.4, 1.5, 32, 9},
                                     {"Cameron Johnson", "DEN", 16.5, 4.3, 2.5, 2.9, 31, 5},
                                     {"Christian Braun", "DEN", 14.0, 5.2, 2.3, 1.1, 33, 3},
                                     {"Anthony Edwards", "MIN", 28.8, 5.7, 4.8, 4.1, 36, 15},
                                     {"Julius Randle", "MIN", 20.6, 7.6, 4.8, 1.5, 33, 8},
                                     {"Rudy Gobert", "MIN", 11.8, 11.5, 1.8, 0, 32, 6},
                                     {"Donte DiVincenzo", "MIN", 12.8, 3.8, 4.0, 2.8, 29, 4},
                                     {"Jaden McDaniels", "MIN", 12.4, 5.0, 2.0, 1.2, 31, 3}};
    for (size_t k = 0; k < seeds.size(); ++k) {
        const auto &p = seeds[k];
        json logs = json::array();
        std::normal_distribution<double> z(0, 1);
        for (int i = 0; i < 45; ++i) {
            double form = z(rng), minutes = clamp(p.mins + 2.5 * z(rng), 18, 42);
            auto stat = [&](double mean, double spread) {
                return std::max(
                    0.0, std::round((mean + spread * (0.35 * form + 0.94 * z(rng))) * minutes / p.mins));
            };
            double pts = stat(p.pts, 5.8), reb = stat(p.reb, 2.6), ast = stat(p.ast, 2.4),
                   th = stat(p.three, 1.6);
            logs.push_back({{"date", iso_at(now - (i * 2 + 2) * 86400).substr(0, 10)},
                            {"minutes", std::round(minutes * 10) / 10},
                            {"points", pts},
                            {"rebounds", reb},
                            {"assists", ast},
                            {"threes", th},
                            {"steals", stat(1.1, 0.9)},
                            {"blocks", stat(k % 5 == 4 ? 1.5 : 0.6, 0.8)},
                            {"fga", std::round(pts / 1.3)},
                            {"fgm", std::round(pts / 2.5)},
                            {"fta", std::round(pts / 5)},
                            {"ftm", std::round(pts / 6)},
                            {"threeAttempts", std::max(th, std::round(p.three * 2.7 + z(rng)))},
                            {"oreb", std::max(0.0, std::round(reb / 4))},
                            {"turnovers", std::max(0.0, std::round(p.ast / 3 + z(rng)))},
                            {"fouls", std::max(0.0, std::round(2 + z(rng)))},
                            {"home", i % 2 == 0}});
        }
        json pl = {{"name", p.name},
                   {"team", p.team},
                   {"logs", logs},
                   {"starterConfirmed", true},
                   {"starterGameId", "demo-" + std::to_string(k / 10 + 1)},
                   {"firstBasketWins", p.first},
                   {"firstBasketGames", 80},
                   {"injury", "Available (demo)"},
                   {"source", "synthetic"},
                   {"dataAsOf", now_iso()}};
        if (k >= 20) {
            const double part = 27.3 / 48;
            pl["live"] = {{"gameId", "demo-3"},
                          {"minutes", std::round(p.mins * part * 10) / 10},
                          {"points", std::round(p.pts * part)},
                          {"rebounds", std::round(p.reb * part)},
                          {"assists", std::round(p.ast * part)},
                          {"threes", std::round(p.three * part)},
                          {"steals", k % 2},
                          {"blocks", k % 3 == 0 ? 1 : 0},
                          {"dataAsOf", now_iso()}};
        }
        s["players"][normalize_name(p.name)] = pl;
    }
    auto quote = [&](std::string game, std::string market, std::string side, int odds, json line,
                     std::string player, std::string team, std::string book) {
        json q = {{"gameId", game},
                  {"market", market},
                  {"side", side},
                  {"americanOdds", odds},
                  {"line", line},
                  {"player", player},
                  {"team", team},
                  {"book", book},
                  {"observedAt", now_iso()},
                  {"timestampKind", "synthetic"},
                  {"source", "synthetic"},
                  {"status", "open"},
                  {"settlement", "full_game_including_ot"},
                  {"demo", true}};
        q["id"] = quote_id(q);
        s["quotes"].push_back(q);
    };
    for (int i = 0; i < 3; ++i) {
        std::string id = "demo-" + std::to_string(i + 1);
        quote(id, "moneyline", "home",
              i == 0   ? -140
              : i == 1 ? -120
                       : -180,
              nullptr, "", order[i * 2], "Demo book A");
        quote(id, "moneyline", "away",
              i == 0   ? 125
              : i == 1 ? 110
                       : 155,
              nullptr, "", order[i * 2 + 1], "Demo book A");
        for (int t = 0; t < 2; ++t)
            for (auto side : {"over", "under"})
                quote(id, "team_total", side, -110, 111.5 + i * 2.0, "", order[i * 2 + t], "Demo book A");
        for (int pindex : {0, 1, 5, 6}) {
            const auto &p = seeds[i * 10 + pindex];
            const std::vector<std::pair<std::string, double>> lines = {
                {"points", std::floor(p.pts) - 0.5},
                {"rebounds", std::floor(p.reb) + 0.5},
                {"assists", std::floor(p.ast) + 0.5},
                {"threes", std::max(0.5, std::floor(p.three) + 0.5)},
                {"pra", std::floor(p.pts + p.reb + p.ast) - 0.5}};
            for (const auto &item : lines) {
                auto book = pindex % 2 == 0 ? "Demo book A" : "Demo book B";
                quote(id, item.first, "over", -110, item.second, p.name, p.team, book);
                quote(id, item.first, "under", -110, item.second, p.name, p.team, book);
            }
            quote(id, "double_double", "yes",
                  p.reb > 10                   ? -450
                  : (p.reb >= 7 || p.ast >= 7) ? 100
                                               : 650,
                  nullptr, p.name, p.team, "Demo book B");
            quote(id, "triple_double", "yes", p.ast > 9 ? 260 : 900, nullptr, p.name, p.team, "Demo book B");
            if (i != 2)
                quote(id, "first_basket", "yes",
                      static_cast<int>(std::round(100 * (1 / (p.first / 80.0 * 1.03) - 1) / 10) * 10),
                      nullptr, p.name, p.team, "Demo book A");
        }
    }
    s["sources"] = {
        {{"id", "demo"},
         {"label", "Demo generator"},
         {"state", "demo"},
         {"updatedAt", now_iso()},
         {"count", static_cast<int>(s["quotes"].size())},
         {"message",
          "Seeded synthetic histories, fictional matchups and simulated prices. No live bookmaker data."}}};
    return s;
}

json evaluate_quote(const json &q, const json &s) {
    json r = q;
    r["marketLabel"] = labels.at(q.value("market", "moneyline"));
    r["oddsPercent"] = has_quote_price(q) ? json(100 * quote_implied_probability(q)) : json(nullptr);
    r["impliedProbability"] = has_quote_price(q) ? json(quote_implied_probability(q)) : json(nullptr);
    r["features"] = json::array();
    r["caveats"] = json::array();
    r["recent"] = json::array();
    r["availability"] = "ready";
    r["probability"] = nullptr;
    r["pushProbability"] = 0.0;
    r["projected"] = nullptr;
    r["ev"] = nullptr;
    r["eligible"] = false;
    r["samplingInterval"] = nullptr;
    r["fairProbability"] = nullptr;
    r["edge"] = nullptr;
    r["modelVersion"] = MODEL_VERSION;
    r["sampleSize"] = 0;
    r["effectiveSampleSize"] = 0;
    r["quality"] = "Insufficient";
    const json *game = nullptr;
    for (const auto &g : s["games"])
        if (g.value("id", "") == q.value("gameId", "")) {
            game = &g;
            break;
        }
    if (!game)
        return blocked(r, "No matching scheduled game. Import a valid game ID.");
    const auto &g = *game;
    const auto status = g.value("status", "upcoming");
    r["game"] = g;
    r["matchup"] = g.value("away", "") + " @ " + g.value("home", "");
    const bool live = status == "live";
    const auto age = epoch_now() - parse_time(q.value("observedAt", ""));
    const auto price_limit = live ? 90 : 900;
    const bool fresh = age >= -30 && age <= price_limit;
    r["priceAgeSeconds"] = age;
    r["freshness"] = fresh ? "fresh" : age < -30 ? "future" : "stale";
    r["caveats"].push_back(
        "Experimental baseline; no out-of-sample profitability or calibration has been established.");
    if (s.value("mode", "demo") == "demo")
        r["caveats"].push_back(
            "Synthetic demo data only. Do not use these prices or projections to place bets.");
    if (!fresh)
        r["caveats"].push_back("Quote is stale or future-dated. EV and stake sizing are disabled.");
    if (q.value("timestampKind", "") == "retrieved")
        r["caveats"].push_back("Price timestamp is retrieval time, not a verified bookmaker last-move "
                               "timestamp. Confirm the price at the book.");
    if (q.value("source", "") == "user_import")
        r["caveats"].push_back("User-supplied price/history; its accuracy is not independently verified.");
    if (status == "final" || status == "postponed" || status == "cancelled")
        return blocked(r, "Game is not open for betting.");
    if (g.value("seasonType", "regular") == "preseason")
        return blocked(r, "Preseason rotation uncertainty is outside this regular-season baseline.");
    if (!live && parse_time(g.value("startsAt", "")) <= epoch_now())
        return blocked(r, "Scheduled start has passed without a confirmed live game state.");
    if (live && (!g.value("liveScoresComplete", true) || !g.value("liveClockComplete", true) ||
                 g.value("period", 0) < 1))
        return blocked(r, "Live score/clock state is incomplete; missing values cannot be assumed zero.");
    if (live && epoch_now() - parse_time(g.value("dataAsOf", "")) > 90)
        return blocked(r, "Live scoreboard is stale; refresh before evaluating.");
    if (q.value("status", "open") != "open")
        return blocked(r, "Market is suspended or closed.");
    if (q.value("settlement", "full_game_including_ot") != "full_game_including_ot")
        return blocked(r, "Only full-game including-overtime settlement is supported.");
    if (!has_quote_price(q))
        return blocked(r, "No public bookmaker quote. Import a current price to evaluate EV.");
    const auto market = q.value("market", "");
    const auto side = q.value("side", "");
    const auto home = g.value("home", ""), away = g.value("away", "");
    const auto team = q.value("team", "");
    const bool is_home = team == home;
    const auto opponent = is_home ? away : home;
    if (!team.empty() && team != home && team != away)
        return blocked(r, "Selection team is not in this matchup.");
    const double elapsed = number(g.value("elapsedMinutes", json(0)));
    const int period = g.value("period", 0);
    const double total_minutes = period > 4 ? 48 + (period - 4) * 5 : 48;
    const double remaining = clamp(total_minutes - elapsed, 0, 48);
    if (live && remaining <= 0)
        return blocked(
            r, "Awaiting a confirmed next period or final game status; the current live horizon has ended.");
    if (live && market == "team_total" &&
        number(g[is_home ? "homeScore" : "awayScore"]) > number(q.value("line", json(0))))
        return blocked(r, "This full-game team-total threshold has already passed. Treat an offered line as "
                          "potentially settled/suspended, not a new edge.");
    double p = 0, push = 0, neff = 0, projected = 0;
    if (market == "moneyline" || market == "team_total") {
        if (!s["teams"].contains(home) || !s["teams"].contains(away))
            return blocked(r, "Missing historical team ratings.");
        const auto &h = s["teams"][home];
        const auto &a = s["teams"][away];
        if (h.contains("historySource") || a.contains("historySource")) {
            r["historySource"] = "basketball_reference";
            r["caveats"].push_back(
                "Local historical box scores supply team game vectors and approximate "
                "possession/rating fallback where official data is missing. Coverage may be "
                "partial; old environments are not made fresh by reloading.");
        }
        if (s.value("mode", "demo") == "live")
            for (const auto *t : {&h, &a})
                if (epoch_now() - parse_time(t->value("dataAsOf", "")) > 172800)
                    return blocked(r, "Team environment is more than 48 hours old. Refresh its data.");
        for (const auto *t : {&h, &a})
            if (!t->contains("offRating") || !t->contains("defRating") || !t->contains("pace") ||
                !t->contains("logs") || (*t)["logs"].size() < 12)
                return blocked(r, "At least 12 team games plus pace/offense/defense ratings are required.");
        const double pace = (number(h["pace"]) + number(a["pace"])) / 2;
        if (pace < 70 || pace > 130)
            return blocked(r, "Team pace is outside the supported range.");
        double hmean = pace * (number(h["offRating"]) + number(a["defRating"])) / 200.0 + 1.15;
        double amean = pace * (number(a["offRating"]) + number(h["defRating"])) / 200.0 - 1.15;
        const double rest =
            clamp(number(h.value("restDays", json(1))) - number(a.value("restDays", json(1))), -3, 3);
        double margin = hmean - amean + 0.5 * rest;
        if (h.contains("elo") && a.contains("elo")) {
            margin = 0.7 * (hmean - amean) + 0.3 * ((number(h["elo"]) - number(a["elo"])) / 28.0 + 2.3) +
                     0.5 * rest;
            feature(r, "Elo differential", number(h["elo"]) - number(a["elo"]), "rating");
        }
        double sq = 0, mean = 0;
        int n = 0;
        const auto &stats_team = market == "team_total" ? (is_home ? h : a) : h;
        const auto cutoff = parse_time(g.value("startsAt", ""));
        for (const auto &log : stats_team["logs"]) {
            if (parse_time(log.value("date", "")) >= cutoff)
                continue;
            double value = number(log.value(market == "team_total" ? "points" : "margin", json(0)));
            mean += value;
            sq += value * value;
            ++n;
        }
        if (n < 12)
            return blocked(r, "Insufficient pre-game team history (leakage guard).");
        double sd = clamp(std::sqrt(std::max(0.0, (sq - mean * mean / n) / (n - 1))), 9, 18);
        neff = std::min<size_t>(h["logs"].size(), a["logs"].size());
        r["sampleSize"] = n;
        if (market == "moneyline") {
            if ((side == "home" && team != home) || (side == "away" && team != away))
                return blocked(r, "Moneyline side and team do not match.");
            projected = margin;
            if (live) {
                projected = number(g["homeScore"]) - number(g["awayScore"]) + margin * remaining / 48;
                sd = std::max(1.5, sd * std::sqrt(remaining / 48));
            }
            p = phi(projected / sd);
            if (side == "away")
                p = 1 - p;
            feature(r, "Home net rating", number(h["offRating"]) - number(h["defRating"]), "pts/100");
            feature(r, "Away net rating", number(a["offRating"]) - number(a["defRating"]), "pts/100");
            feature(r, "Rest-day differential", rest, "days");
            feature(r, "Home court baseline", 2.3, "points");
            r["projectionLabel"] = "Projected home margin";
        } else {
            projected = is_home ? hmean : amean;
            if (live) {
                projected = number(g[is_home ? "homeScore" : "awayScore"]) + projected * remaining / 48;
                sd = std::max(1.5, sd * std::sqrt(remaining / 48));
            }
            auto outcomes = normal_outcomes(projected, sd, number(q["line"]), side);
            p = outcomes["win"];
            push = outcomes["push"];
            feature(r, "Team offensive rating", number(stats_team["offRating"]), "pts/100");
            feature(r, "Opponent defensive rating", number((is_home ? a : h)["defRating"]), "pts/100");
            r["projectionLabel"] = "Projected team points";
            int seen = 0;
            for (const auto &log : stats_team["logs"])
                if (seen++ < 10 && parse_time(log.value("date", "")) < cutoff)
                    r["recent"].push_back({{"date", log["date"]}, {"value", log["points"]}});
        }
        feature(r, "Matchup pace", pace, "possessions");
        feature(r, "Historical scoring spread", sd, "points");
    } else {
        const auto key = normalize_name(q.value("player", ""));
        if (!s["players"].contains(key))
            return blocked(r, "Player history is unavailable. Import game logs or refresh NBA stats.");
        const auto &pl = s["players"][key];
        r["player"] = pl.value("name", q.value("player", ""));
        r["injury"] = pl.value("injury", "Unknown");
        if (pl.contains("historySource")) {
            r["historySource"] = pl["historySource"];
            r["caveats"].push_back(
                "Local Basketball Reference history supplies game logs and/or dated "
                "first-FG opportunities. Coverage can be partial; data is not independently "
                "verified. Historical lineups never confirm current starters.");
        }
        if (pl.value("team", "") != team)
            return blocked(r, "Player team does not match the quoted selection.");
        const auto injury = normalize_name(pl.value("injury", ""));
        if (injury == "out" || injury.find("injuredreserve") != std::string::npos || injury == "inactive")
            return blocked(r, "Player is reported out or inactive.");
        if (injury.find("questionable") != std::string::npos || injury.find("doubtful") != std::string::npos)
            r["caveats"].push_back(
                "Uncertain player availability; projected minutes may not reflect injury restrictions.");
        if (injury == "unknown" || injury.empty())
            r["caveats"].push_back("Player injury status is not known.");
        if (market == "first_basket") {
            if (live)
                return blocked(r, "First-basket markets are disabled once a game has started.");
            double total = 0, target = 0;
            int field = 0, home_starters = 0, away_starters = 0;
            double smallest = 1e9;
            for (const auto &item : s["players"].items()) {
                const auto &candidate = item.value();
                auto ct = candidate.value("team", "");
                if ((ct != home && ct != away) || !candidate.value("starterConfirmed", false) ||
                    candidate.value("starterGameId", "") != g.value("id", ""))
                    continue;
                auto availability = normalize_name(candidate.value("injury", "Unknown"));
                if (availability == "out" || availability == "inactive")
                    return blocked(
                        r,
                        "A confirmed first-basket starter is marked out/inactive. Resolve the lineup first.");
                const auto counts = first_basket_counts(candidate, g.value("startsAt", ""));
                double n = number(counts["games"]), w = number(counts["wins"]);
                if (n < 20 || w > n)
                    return blocked(r,
                                   "All 10 confirmed starters need at least 20 first-basket history games.");
                double rate = (w + 0.5) / (n + 1);
                total += rate;
                smallest = std::min(smallest, n);
                ++field;
                if (ct == home)
                    ++home_starters;
                else
                    ++away_starters;
                if (item.key() == key)
                    target = rate;
            }
            if (field != 10 || home_starters != 5 || away_starters != 5 || target <= 0)
                return blocked(r, "First basket needs a complete field of 10 starters confirmed for this "
                                  "specific game, not "
                                  "scoring-rate guesses.");
            p = target / total;
            if (side == "no")
                p = 1 - p;
            neff = smallest;
            r["sampleSize"] = neff;
            r["projectionLabel"] = "First-basket probability";
            projected = p * 100;
            feature(r, "Confirmed starters", field, "players");
            const auto counts = first_basket_counts(pl, g.value("startsAt", ""));
            feature(r, "First-basket wins", number(counts["wins"]), "games");
            feature(r, "First-basket history", number(counts["games"]), "games");
            if (pl.contains("firstBasketHistory"))
                r["caveats"].push_back(
                    "Dated first-FG opportunities exclude the target day and future games. "
                    "Historical starters do not confirm today's lineup.");
            else
                r["caveats"].push_back(
                    "Legacy aggregate first-FG counts have no game dates; their historical "
                    "cutoff cannot be independently checked.");
            r["caveats"].push_back(
                "First basket is the first made field goal, not the first point/free throw. Verify identical "
                "bookmaker settlement rules. Historical frequencies are matchup-independent.");
        } else {
            if (!pl.contains("logs") || pl["logs"].size() < 12)
                return blocked(r, "At least 12 valid historical player games are required.");
            if (!s["teams"].contains(team) || !s["teams"].contains(opponent) ||
                !s["teams"][team].contains("pace") || !s["teams"][opponent].contains("pace"))
                return blocked(r, "Team/opponent pace is missing; no silent default projection is applied.");
            std::vector<json> logs;
            const auto cutoff = parse_time(g.value("startsAt", ""));
            for (const auto &log : pl["logs"])
                if (number(log.value("minutes", json(0))) >= 5 && parse_time(log.value("date", "")) > 0 &&
                    parse_time(log.value("date", "")) < cutoff)
                    logs.push_back(log);
            std::sort(logs.begin(), logs.end(),
                      [](const json &x, const json &y) { return x.value("date", "") > y.value("date", ""); });
            if (logs.size() > 60)
                logs.resize(60);
            if (logs.size() < 12)
                return blocked(
                    r, "Insufficient pre-game player history (leakage guard / low-minute games excluded).");
            if (epoch_now() - parse_time(logs.front().value("date", "")) > 210 * 86400LL)
                return blocked(r, "Player history is more than 210 days old.");
            if (epoch_now() - parse_time(logs.front().value("date", "")) > 45 * 86400LL)
                r["caveats"].push_back("Historical sample is more than 45 days old. Off-season roster and "
                                       "role changes are not modeled.");
            bool five = true;
            for (const auto &log : logs)
                if (!log.contains("steals") || !log.contains("blocks"))
                    five = false;
            if ((market == "double_double" || market == "triple_double") && !five)
                return blocked(r, "Double/triple-double settlement counts five categories; steals and blocks "
                                  "are required.");
            std::vector<double> weights;
            double sum = 0, sum2 = 0, minutes_sum = 0, recent_minutes = 0;
            size_t index = 0;
            for (const auto &log : logs) {
                double w = std::exp(-0.045 * index) * (log.value("home", false) == is_home ? 1.15 : 1.0);
                weights.push_back(w);
                sum += w;
                sum2 += w * w;
                minutes_sum += w * number(log["minutes"]);
                if (index < 5)
                    recent_minutes += number(log["minutes"]);
                ++index;
            }
            neff = sum * sum / sum2;
            double minutes = clamp(0.7 * recent_minutes / 5 + 0.3 * minutes_sum / sum, 6, 42);
            if (pl.contains("projectedMinutes")) {
                minutes = number(pl["projectedMinutes"]);
                r["caveats"].push_back("Projected minutes were explicitly overridden by the user.");
            }
            if (s.value("mode", "demo") == "live")
                for (const auto &abbr : {team, opponent})
                    if (epoch_now() - parse_time(s["teams"][abbr].value("dataAsOf", "")) > 172800)
                        return blocked(r, "Team pace environment is more than 48 hours old.");
            const double pace = (number(s["teams"][team]["pace"]) + number(s["teams"][opponent]["pace"])) / 2;
            // Relative adjustment needs the historical team pace. Never treat league-average pace as measured
            // data.
            const double history_pace = number(pl.value("historicalPace", s["teams"][team]["pace"]));
            if (history_pace < 70 || history_pace > 130 || pace < 70 || pace > 130)
                return blocked(r, "Historical or matchup pace is outside supported bounds.");
            const double factor = clamp(pace / history_pace, 0.85, 1.15);
            json accrued = {{"points", 0}, {"rebounds", 0}, {"assists", 0},
                            {"threes", 0}, {"steals", 0},   {"blocks", 0}};
            if (live) {
                if (!pl.contains("live") || pl["live"].value("gameId", "") != g.value("id", "") ||
                    epoch_now() - parse_time(pl["live"].value("dataAsOf", "")) > 90)
                    return blocked(r, "Fresh live player box score is required for in-play props.");
                accrued = pl["live"];
                r["liveDataAsOf"] = accrued.value("dataAsOf", "");
                for (auto stat : {"points", "rebounds", "assists", "threes", "minutes"})
                    if (!accrued.contains(stat))
                        return blocked(r, "Incomplete live player box score.");
                if ((market == "double_double" || market == "triple_double") &&
                    (!accrued.contains("steals") || !accrued.contains("blocks")))
                    return blocked(r, "Live steals/blocks are required for double/triple-double props.");
                if (period > 4)
                    return blocked(r, "Overtime player rotations are outside this experimental baseline; "
                                      "in-play player props are suppressed.");
                double current = market == "pra" ? number(accrued["points"]) + number(accrued["rebounds"]) +
                                                       number(accrued["assists"])
                                                 : number(accrued.value(market, json(0)));
                bool threshold_passed = current > number(q.value("line", json(0)));
                if (market == "double_double" || market == "triple_double") {
                    int categories = 0;
                    for (auto stat : {"points", "rebounds", "assists", "steals", "blocks"})
                        if (number(accrued[stat]) >= 10)
                            ++categories;
                    threshold_passed = categories >= (market == "double_double" ? 2 : 3);
                }
                if (threshold_passed)
                    return blocked(r,
                                   "This full-game player threshold has already been achieved. Verify "
                                   "settlement/suspension rather than treating an old line as a new edge.");
                feature(r, "Accrued player minutes", number(accrued["minutes"]), "minutes");
                minutes = clamp(std::min(minutes - number(accrued["minutes"]), remaining * minutes / 48), 0,
                                remaining);
            }
            std::mt19937 rng(
                static_cast<uint32_t>(std::stoull(stable_id(key + g.value("id", "")), nullptr, 16)));
            std::discrete_distribution<size_t> select(weights.begin(), weights.end());
            std::normal_distribution<double> jitter(0, 0.08);
            constexpr int draws = 8192;
            double wins = 0, pushes = 0, project_sum = 0;
            const double line = number(q.value("line", json(0)));
            for (int draw = 0; draw < draws; ++draw) {
                const auto &log = logs[select(rng)];
                const double scaling =
                    minutes / number(log["minutes"]) * factor * std::exp(jitter(rng) - 0.0032);
                std::array<double, 6> values{};
                int k = 0;
                for (auto stat : {"points", "rebounds", "assists", "steals", "blocks", "threes"})
                    values[k++] = std::max(0.0, std::round(number(log.value(stat, json(0))) * scaling)) +
                                  number(accrued.value(stat, json(0)));
                double value = 0;
                if (market == "points")
                    value = values[0];
                else if (market == "rebounds")
                    value = values[1];
                else if (market == "assists")
                    value = values[2];
                else if (market == "threes")
                    value = values[5];
                else if (market == "pra")
                    value = values[0] + values[1] + values[2];
                else {
                    int count = 0;
                    for (int c = 0; c < 5; ++c)
                        if (values[c] >= 10)
                            ++count;
                    value = count >= (market == "double_double" ? 2 : 3) ? 1 : 0;
                }
                project_sum += value;
                if (side == "yes" || side == "no") {
                    if ((value == 1) == (side == "yes"))
                        ++wins;
                } else if (std::abs(value - line) < 1e-8)
                    ++pushes;
                else if ((side == "over" && value > line) || (side == "under" && value < line))
                    ++wins;
            }
            p = wins / draws;
            push = pushes / draws;
            projected = project_sum / draws;
            r["sampleSize"] = logs.size();
            r["projectionLabel"] = (market == "double_double" || market == "triple_double")
                                       ? "Simulated occurrence rate"
                                       : "Projected final statistic";
            if (market == "double_double" || market == "triple_double")
                projected *= 100;
            feature(r, live ? "Projected remaining minutes" : "Projected minutes", minutes, "minutes");
            feature(r, "Matchup pace", pace, "possessions");
            feature(r, "Historical team pace", history_pace, "possessions");
            feature(r, "Pace multiplier", factor, "×");
            feature(r, "Recency decay", 0.045, "per game");
            feature(r, "Same-location weight", 1.15, "×");
            feature(r, "Effective historical sample", neff, "games");
            for (const auto &pair :
                 std::vector<std::pair<std::string, std::string>>{{"points", "Points / minute"},
                                                                  {"rebounds", "Rebounds / minute"},
                                                                  {"assists", "Assists / minute"},
                                                                  {"threes", "3PM / minute"}}) {
                double rate = 0;
                for (size_t j = 0; j < logs.size(); ++j)
                    rate += weights[j] * number(logs[j][pair.first]) / number(logs[j]["minutes"]);
                feature(r, pair.second, rate / sum, "per minute");
            }
            double fga = 0, fgm = 0, three = 0, fta = 0, pts = 0;
            for (const auto &log : logs) {
                fga += number(log.value("fga", json(0)));
                fgm += number(log.value("fgm", json(0)));
                three += number(log["threes"]);
                fta += number(log.value("fta", json(0)));
                pts += number(log["points"]);
            }
            if (fga > 0) {
                feature(r, "Effective field goal %", 100 * (fgm + 0.5 * three) / fga, "%", "context");
                feature(r, "True shooting %", 100 * pts / (2 * (fga + 0.44 * fta)), "%", "context");
                feature(r, "Shot attempts / game", fga / logs.size(), "attempts", "context");
            }
            int hits = 0, count = 0;
            for (const auto &log : logs) {
                if (count == 10)
                    break;
                double value = 0;
                if (market == "pra")
                    value = number(log["points"]) + number(log["rebounds"]) + number(log["assists"]);
                else if (market == "double_double" || market == "triple_double") {
                    int dd = 0;
                    for (auto stat : {"points", "rebounds", "assists", "steals", "blocks"})
                        if (number(log[stat]) >= 10)
                            ++dd;
                    value = dd >= (market == "double_double" ? 2 : 3) ? 1 : 0;
                } else
                    value = number(log[market]);
                bool hit = side == "yes"    ? value == 1
                           : side == "no"   ? value == 0
                           : side == "over" ? value > line
                                            : value < line;
                if (hit)
                    ++hits;
                ++count;
                r["recent"].push_back({{"date", log["date"]}, {"value", value}, {"hit", hit}});
            }
            r["recentHitRate"] = count ? 100.0 * hits / count : 0;
            r["caveats"].push_back("Bootstrap reuses complete game stat vectors (preserving empirical "
                                   "covariance). Tail probabilities and role changes remain uncertain; "
                                   "sampling interval is not calibrated model confidence.");
        }
    }
    if (live) {
        feature(r, "Game minutes remaining", remaining, "minutes");
        feature(r, "Current score differential", number(g["homeScore"]) - number(g["awayScore"]), "points");
        r["caveats"].push_back("In-play baseline does not model substitutions, foul trouble, possessions, or "
                               "bookmaker latency. Verify all live inputs.");
    }
    r["probability"] = clamp(p, 0, 1);
    r["pushProbability"] = push;
    r["projected"] = projected;
    r["effectiveSampleSize"] = neff;
    r["samplingInterval"] = wilson_interval(p, neff);
    r["quality"] = neff >= 30 ? "Larger sample" : neff >= 20 ? "Moderate sample" : "Small sample";
    r["decimalOdds"] = quote_decimal_odds(q);
    if (fresh) {
        r["ev"] = expected_value(p, push, q);
        r["conservativeEv"] = expected_value(number(r["samplingInterval"]["lower"]), push, q);
        r["kellyFraction"] = fractional_kelly(p, push, q);
        r["eligible"] = true;
    } else {
        r["conservativeEv"] = nullptr;
        r["kellyFraction"] = nullptr;
    }
    return r;
}

json build_dashboard(const json &snapshot) {
    json result = {{"version", VERSION},
                   {"modelVersion", MODEL_VERSION},
                   {"mode", snapshot.value("mode", "demo")},
                   {"generatedAt", snapshot.value("generatedAt", now_iso())},
                   {"revision", snapshot.value("revision", 0)},
                   {"refreshing", snapshot.value("refreshing", false)},
                   {"games", snapshot["games"]},
                   {"sources", snapshot["sources"]},
                   {"markets", json::array()},
                   {"featureCatalog", feature_catalog()},
                   {"calibration", "Uncalibrated experimental baseline"},
                   {"notice", "Research only. Estimated +EV is not a validated betting edge. No auto-betting "
                              "or guarantee of profit."}};
    int positive = 0, ready = 0, live = 0;
    double best = 0;
    for (const auto &game : snapshot["games"])
        if (game.value("status", "") == "live")
            ++live;
    for (const auto &q : snapshot["quotes"]) {
        json row = evaluate_quote(q, snapshot);
        if (row.value("eligible", false)) {
            ++ready;
            if (number(row["ev"]) > 0) {
                ++positive;
                best = std::max(best, number(row["ev"]));
            }
        }
        result["markets"].push_back(std::move(row));
    }
    // De-vig only matching, opposite, same-book quotes. Never mix lines, events, subjects or old timestamps.
    for (auto &r : result["markets"]) {
        if (!has_quote_price(r))
            continue;
        const auto market = r.value("market", "");
        const auto side = r.value("side", "");
        std::string opposite = side == "over"    ? "under"
                               : side == "under" ? "over"
                               : side == "home"  ? "away"
                               : side == "away"  ? "home"
                               : side == "yes"   ? "no"
                                                 : "yes";
        for (const auto &q : result["markets"]) {
            if (q.value("gameId", "") != r.value("gameId", "") || q.value("market", "") != market ||
                q.value("side", "") != opposite || q.value("book", "") != r.value("book", "") ||
                q.value("player", "") != r.value("player", "") ||
                (market != "moneyline" && q.value("team", "") != r.value("team", "")) ||
                q.value("line", json()) != r.value("line", json()) || !has_quote_price(q) ||
                q.value("freshness", "") != "fresh" || r.value("freshness", "") != "fresh" ||
                std::abs(parse_time(q.value("observedAt", "")) - parse_time(r.value("observedAt", ""))) > 90)
                continue;
            double p0 = quote_implied_probability(r), p1 = quote_implied_probability(q);
            r["fairProbability"] = p0 / (p0 + p1);
            r["overround"] = (p0 + p1 - 1) * 100;
            if (!r["probability"].is_null() && number(r["pushProbability"]) < 1)
                r["edge"] =
                    (number(r["probability"]) / (1 - number(r["pushProbability"])) - p0 / (p0 + p1)) * 100;
            break;
        }
    }
    // Missing feed coverage is visible. It is not filled with invented prices.
    std::set<std::string> covered;
    for (const auto &q : snapshot["quotes"])
        covered.insert(q.value("gameId", "") + q.value("market", "") + q.value("player", "") +
                       q.value("team", ""));
    for (const auto &game : snapshot["games"]) {
        if (game.value("status", "") == "final")
            continue;
        for (const auto &side : {std::string("home"), std::string("away")}) {
            const auto team = game.value(side, "");
            for (const auto &market : {std::string("moneyline"), std::string("team_total")}) {
                auto key = game.value("id", "") + market + "" + team;
                if (covered.count(key))
                    continue;
                json q = {{"id", "missing-" + stable_id(key)},
                          {"gameId", game["id"]},
                          {"market", market},
                          {"side", market == "moneyline" ? side : "over"},
                          {"team", team},
                          {"player", ""},
                          {"book", "No public quote"},
                          {"line", nullptr},
                          {"americanOdds", nullptr},
                          {"observedAt", now_iso()},
                          {"source", "missing"}};
                result["markets"].push_back(evaluate_quote(q, snapshot));
            }
        }
        for (const auto &item : snapshot["players"].items()) {
            const auto &pl = item.value();
            auto team = pl.value("team", "");
            if (team != game.value("home", "") && team != game.value("away", ""))
                continue;
            for (const auto &market : {"points", "rebounds", "assists", "threes", "pra", "double_double",
                                       "triple_double", "first_basket"}) {
                auto key = game.value("id", "") + market + pl.value("name", "") + team;
                if (covered.count(key))
                    continue;
                json q = {{"id", "missing-" + stable_id(key)},
                          {"gameId", game["id"]},
                          {"market", market},
                          {"side", std::string(market) == "double_double" ||
                                           std::string(market) == "triple_double" ||
                                           std::string(market) == "first_basket"
                                       ? "yes"
                                       : "over"},
                          {"team", team},
                          {"player", pl.value("name", "")},
                          {"book", "No public quote"},
                          {"line", nullptr},
                          {"americanOdds", nullptr},
                          {"observedAt", now_iso()},
                          {"source", "missing"}};
                result["markets"].push_back(evaluate_quote(q, snapshot));
            }
        }
    }
    result["summary"] = {{"positiveEv", positive},
                         {"priced", ready},
                         {"quotes", snapshot["quotes"].size()},
                         {"markets", result["markets"].size()},
                         {"liveGames", live},
                         {"games", snapshot["games"].size()},
                         {"bestEv", best},
                         {"players", snapshot["players"].size()}};
    result["players"] = json::array();
    for (const auto &item : snapshot["players"].items())
        result["players"].push_back(
            {{"name", item.value().value("name", "")}, {"team", item.value().value("team", "")}});
    return result;
}

json expire_dashboard(json dashboard) {
    int positive = 0, ready = 0;
    double best = 0;
    for (auto &row : dashboard["markets"]) {
        const bool live = row.value("game", json::object()).value("status", "") == "live";
        const auto age = epoch_now() - parse_time(row.value("observedAt", ""));
        row["priceAgeSeconds"] = age;
        const bool fresh = age >= -30 && age <= (live ? 90 : 900);
        row["freshness"] = fresh ? "fresh" : age < -30 ? "future" : "stale";
        if (!fresh) {
            row["ev"] = nullptr;
            row["conservativeEv"] = nullptr;
            row["kellyFraction"] = nullptr;
            row["edge"] = nullptr;
            row["fairProbability"] = nullptr;
            row["eligible"] = false;
        }
        const auto game = row.value("game", json::object());
        const bool score_stale = live && epoch_now() - parse_time(game.value("dataAsOf", "")) > 90;
        const bool player_stale = live && row.contains("liveDataAsOf") &&
                                  epoch_now() - parse_time(row.value("liveDataAsOf", "")) > 90;
        const bool start_passed =
            game.value("status", "") == "upcoming" && parse_time(game.value("startsAt", "")) <= epoch_now();
        if (score_stale || player_stale || start_passed)
            row = blocked(row, score_stale ? "Live scoreboard is stale; refresh before evaluating."
                               : player_stale
                                   ? "Live player box score is stale."
                                   : "Scheduled start has passed without a confirmed live game state.");
        if (row.value("eligible", false) && !row["ev"].is_null()) {
            ++ready;
            if (number(row["ev"]) > 0) {
                ++positive;
                best = std::max(best, number(row["ev"]));
            }
        }
    }
    dashboard["summary"]["positiveEv"] = positive;
    dashboard["summary"]["priced"] = ready;
    dashboard["summary"]["bestEv"] = best;
    return dashboard;
}

json validate_import(const json &document) {
    if (!document.is_object())
        throw std::invalid_argument(
            "Import must be a JSON object containing quotes, players, teams, or games.");
    json out = {{"quotes", json::array()},  {"players", json::object()},
                {"teams", json::object()},  {"games", json::array()},
                {"lineups", json::array()}, {"demoOnly", document.value("synthetic", false)}};
    if (document.contains("quotes")) {
        if (!document["quotes"].is_array() || document["quotes"].size() > 2000)
            throw std::invalid_argument("quotes must be an array with at most 2,000 entries");
        for (auto q : document["quotes"]) {
            require_identifier(q, "gameId");
            require_string(q, "market");
            require_string(q, "side");
            require_string(q, "team", 3);
            require_string(q, "book", 80);
            require_string(q, "observedAt", 20);
            const auto market = q["market"].get<std::string>(), side = q["side"].get<std::string>();
            if (!markets.count(market))
                throw std::invalid_argument("Unsupported market: " + market);
            if (!teams.count(q["team"].get<std::string>()))
                throw std::invalid_argument("Use a standard NBA team abbreviation, e.g. GSW, LAL, NYK.");
            normalize_price(q);
            auto timestamp = parse_time(q["observedAt"]);
            if (!timestamp || q["observedAt"].get<std::string>().size() != 20)
                throw std::invalid_argument("observedAt must be a real UTC timestamp: YYYY-MM-DDTHH:MM:SSZ");
            if (timestamp > epoch_now() + 30)
                throw std::invalid_argument("Quote timestamp is in the future");
            bool binary = market == "double_double" || market == "triple_double" || market == "first_basket";
            if ((market == "moneyline" && side != "home" && side != "away") ||
                (binary && side != "yes" && side != "no") ||
                (!binary && market != "moneyline" && side != "over" && side != "under"))
                throw std::invalid_argument("Side is incompatible with market " + market);
            if (market != "moneyline" && !binary) {
                require_number(q, "line", 0.5, market == "team_total" ? 300 : 150);
                if (std::abs(number(q["line"]) * 2 - std::round(number(q["line"]) * 2)) > 1e-8)
                    throw std::invalid_argument("Lines must be integers or half-points");
            } else
                q["line"] = nullptr;
            if (market != "moneyline" && market != "team_total") {
                require_string(q, "player", 100);
                if (normalize_name(q["player"]).empty())
                    throw std::invalid_argument("Player name has no matchable letters");
            } else
                q["player"] = "";
            if (q.value("settlement", "full_game_including_ot") != "full_game_including_ot")
                throw std::invalid_argument("Only full_game_including_ot settlement is supported");
            if (q.value("status", "open") != "open" && q.value("status", "") != "suspended" &&
                q.value("status", "") != "closed")
                throw std::invalid_argument("Invalid quote status");
            q["status"] = q.value("status", "open");
            q["settlement"] = "full_game_including_ot";
            q["source"] = "user_import";
            q["timestampKind"] = "user_observed";
            q["demo"] = false;
            q["id"] = quote_id(q);
            out["quotes"].push_back(q);
        }
    }
    if (document.contains("games")) {
        if (!document["games"].is_array() || document["games"].size() > 100)
            throw std::invalid_argument("games must be an array of at most 100 entries");
        for (auto g : document["games"]) {
            require_identifier(g, "id");
            require_string(g, "home", 3);
            require_string(g, "away", 3);
            require_string(g, "startsAt", 20);
            if (!parse_time(g["startsAt"]))
                throw std::invalid_argument("Invalid game start time");
            if (!teams.count(g["home"]) || !teams.count(g["away"]) || g["home"] == g["away"])
                throw std::invalid_argument("Invalid matchup");
            auto status = g.value("status", "upcoming");
            if (status != "upcoming" && status != "live" && status != "final" && status != "postponed" &&
                status != "cancelled")
                throw std::invalid_argument("Invalid game status");
            g["status"] = status;
            g["source"] = "user_import";
            g["seasonType"] = g.value("seasonType", "regular");
            if (status == "live") {
                require_number(g, "elapsedMinutes", 0, 80);
                require_number(g, "period", 1, 10);
                require_number(g, "homeScore", 0, 300);
                require_number(g, "awayScore", 0, 300);
                require_string(g, "dataAsOf", 20);
                if (!parse_time(g["dataAsOf"]))
                    throw std::invalid_argument("Invalid live game timestamp");
            } else {
                g["homeScore"] = g.value("homeScore", 0);
                g["awayScore"] = g.value("awayScore", 0);
            }
            out["games"].push_back(g);
        }
    }
    auto records = [&](const char *kind) {
        json records = json::array();
        if (!document.contains(kind))
            return records;
        if (document[kind].is_array())
            records = document[kind];
        else if (document[kind].is_object())
            for (const auto &item : document[kind].items()) {
                auto v = item.value();
                if (std::string(kind) == "teams" && !v.contains("abbr"))
                    v["abbr"] = item.key();
                records.push_back(v);
            }
        else
            throw std::invalid_argument(std::string(kind) + " must be an array or object");
        if (records.size() > 600)
            throw std::invalid_argument("Too many imported feature records");
        return records;
    };
    for (auto t : records("teams")) {
        require_string(t, "abbr", 3);
        std::string abbr = t["abbr"];
        if (!teams.count(abbr))
            throw std::invalid_argument("Unknown NBA team abbreviation");
        require_number(t, "pace", 70, 130);
        require_number(t, "offRating", 70, 150);
        require_number(t, "defRating", 70, 150);
        if (t.contains("elo"))
            require_number(t, "elo", 800, 2200);
        if (t.contains("restDays"))
            require_number(t, "restDays", 0, 30);
        if (!t.contains("logs") || !t["logs"].is_array() || t["logs"].size() > 150)
            throw std::invalid_argument("Team logs must be an array of <=150 games");
        std::set<std::string> dates;
        for (auto &log : t["logs"]) {
            require_string(log, "date", 10);
            if (!parse_time(log["date"]) || !dates.insert(log["date"]).second)
                throw std::invalid_argument("Invalid or duplicate team log date");
            require_number(log, "points", 30, 250);
            require_number(log, "margin", -100, 100);
        }
        t["source"] = "user_import";
        t["dataAsOf"] = t.value("dataAsOf", now_iso());
        if (!parse_time(t["dataAsOf"]) || parse_time(t["dataAsOf"]) > epoch_now() + 30)
            throw std::invalid_argument("Invalid team feature timestamp");
        out["teams"][abbr] = t;
    }
    for (auto p : records("players")) {
        require_string(p, "name", 100);
        require_string(p, "team", 3);
        if (!teams.count(p["team"]))
            throw std::invalid_argument("Unknown player team");
        if (!p.contains("logs") || !p["logs"].is_array() || p["logs"].size() > 150)
            throw std::invalid_argument("Player logs must contain <=150 entries");
        std::set<std::string> dates;
        for (auto &log : p["logs"]) {
            require_string(log, "date", 10);
            if (!parse_time(log["date"]) || !dates.insert(log["date"]).second)
                throw std::invalid_argument("Invalid or duplicate player game date");
            require_number(log, "minutes", 0.1, 70);
            for (auto stat : {"points", "rebounds", "assists", "threes"})
                require_number(log, stat, 0, 100);
            for (auto stat : {"steals", "blocks", "fga", "fgm", "fta", "ftm", "threeAttempts", "oreb",
                              "turnovers", "fouls"})
                if (log.contains(stat))
                    require_number(log, stat, 0, 100);
            if (log.contains("home") && !log["home"].is_boolean())
                throw std::invalid_argument("home must be a boolean");
        }
        if (p.contains("injury"))
            require_string(p, "injury", 160);
        if (p.contains("projectedMinutes"))
            require_number(p, "projectedMinutes", 0.1, 48);
        if (p.contains("historicalPace"))
            require_number(p, "historicalPace", 70, 130);
        if (p.contains("starterConfirmed") && !p["starterConfirmed"].is_boolean())
            throw std::invalid_argument("starterConfirmed must be a boolean");
        if (p.value("starterConfirmed", false))
            require_identifier(p, "starterGameId");
        if (p.contains("firstBasketGames")) {
            require_number(p, "firstBasketGames", 0, 2000);
            require_number(p, "firstBasketWins", 0, number(p["firstBasketGames"]));
        }
        if (p.contains("firstBasketHistory")) {
            if (!p["firstBasketHistory"].is_array() || p["firstBasketHistory"].size() > 2000)
                throw std::invalid_argument("firstBasketHistory must contain <=2000 dated starter games");
            std::set<std::string> game_ids;
            for (const auto &observation : p["firstBasketHistory"]) {
                require_identifier(observation, "gameId");
                require_string(observation, "date", 10);
                if (observation["date"].get<std::string>().size() != 10 || !parse_time(observation["date"]) ||
                    !game_ids.insert(observation["gameId"]).second || !observation.contains("win") ||
                    !observation["win"].is_boolean())
                    throw std::invalid_argument("Invalid or duplicate dated first-basket observation");
            }
            p.erase("firstBasketGames");
            p.erase("firstBasketWins");
        }
        if (p.contains("live")) {
            auto &box = p["live"];
            require_identifier(box, "gameId");
            require_string(box, "dataAsOf", 20);
            if (!parse_time(box["dataAsOf"]))
                throw std::invalid_argument("Invalid live box-score timestamp");
            require_number(box, "minutes", 0, 70);
            for (auto stat : {"points", "rebounds", "assists", "threes"})
                require_number(box, stat, 0, 100);
            for (auto stat : {"steals", "blocks"})
                if (box.contains(stat))
                    require_number(box, stat, 0, 100);
        }
        p["source"] = "user_import";
        p["dataAsOf"] = p.value("dataAsOf", now_iso());
        if (!parse_time(p["dataAsOf"]) || parse_time(p["dataAsOf"]) > epoch_now() + 30)
            throw std::invalid_argument("Invalid player feature timestamp");
        if (normalize_name(p["name"]).empty())
            throw std::invalid_argument("Player name has no matchable letters");
        out["players"][normalize_name(p["name"])] = p;
    }
    if (document.contains("lineups")) {
        if (!document["lineups"].is_array() || document["lineups"].size() > 100)
            throw std::invalid_argument("lineups must contain <=100 game-scoped player confirmations");
        for (auto lineup : document["lineups"]) {
            require_identifier(lineup, "gameId");
            require_string(lineup, "player", 100);
            require_string(lineup, "team", 3);
            if (normalize_name(lineup.value("player", "")).empty())
                throw std::invalid_argument("Lineup player has no matchable name");
            if (!teams.count(lineup["team"]) || !lineup.contains("confirmed") ||
                !lineup["confirmed"].is_boolean())
                throw std::invalid_argument("Lineups require a standard NBA team and boolean confirmed");
            lineup["source"] = "user_import";
            out["lineups"].push_back(lineup);
        }
    }
    if (out["quotes"].empty() && out["games"].empty() && out["players"].empty() && out["teams"].empty() &&
        out["lineups"].empty())
        throw std::invalid_argument("Import contains no records");
    return out;
}
json parse_csv_import(const std::string &input) {
    auto rows = csv_rows(input);
    if (rows.size() < 2)
        throw std::invalid_argument("CSV needs a header and at least one quote row");
    if (rows[0][0].rfind("\xEF\xBB\xBF", 0) == 0)
        rows[0][0].erase(0, 3);
    std::map<std::string, size_t> headers;
    for (size_t i = 0; i < rows[0].size(); ++i) {
        if (!headers.emplace(rows[0][i], i).second)
            throw std::invalid_argument("Duplicate CSV header");
    }
    if (!headers.count("oddsPercent") && !headers.count("americanOdds"))
        throw std::invalid_argument("CSV requires oddsPercent or legacy americanOdds");
    for (auto key : {"gameId", "market", "side", "team", "book", "observedAt"})
        if (!headers.count(key))
            throw std::invalid_argument(std::string("Missing CSV column: ") + key);
    json quotes = json::array();
    for (size_t r = 1; r < rows.size(); ++r) {
        if (rows[r].size() != rows[0].size())
            throw std::invalid_argument("CSV column count mismatch on row " + std::to_string(r + 1));
        json q = json::object();
        for (const auto &field : headers) {
            const auto &str = rows[r][field.second];
            if (field.first == "line" || field.first == "americanOdds" || field.first == "oddsPercent") {
                if (str.empty())
                    q[field.first] = nullptr;
                else {
                    size_t used = 0;
                    try {
                        double value = std::stod(str, &used);
                        if (used != str.size() || !std::isfinite(value))
                            throw std::invalid_argument("bad number");
                        if (field.first == "americanOdds") {
                            if (!is_integer(value) || value < -100000 || value > 100000)
                                throw std::invalid_argument("bad odds");
                            q[field.first] = static_cast<int>(value);
                        } else
                            q[field.first] = value;
                    } catch (...) {
                        throw std::invalid_argument("Invalid number on CSV row " + std::to_string(r + 1));
                    }
                }
            } else
                q[field.first] = str;
        }
        quotes.push_back(q);
    }
    return validate_import({{"quotes", quotes}});
}
void merge_import(json &snapshot, const json &validated) {
    for (const auto &item : validated["players"].items())
        snapshot["players"][item.key()] = item.value();
    for (const auto &item : validated["teams"].items())
        snapshot["teams"][item.key()] = item.value();
    for (const auto &g : validated["games"]) {
        bool found = false;
        for (auto &current : snapshot["games"])
            if (current["id"] == g["id"]) {
                current = g;
                found = true;
                break;
            }
        if (!found)
            snapshot["games"].push_back(g);
    }
    for (auto q : validated["quotes"]) {
        q["demo"] = snapshot.value("mode", "demo") == "demo";
        bool found = false;
        for (auto &old : snapshot["quotes"])
            if (old.value("id", "") == q.value("id", "")) {
                old = q;
                found = true;
                break;
            }
        if (!found)
            snapshot["quotes"].push_back(q);
    }
    if (validated.contains("lineups")) {
        if (!snapshot.contains("lineups"))
            snapshot["lineups"] = json::array();
        for (const auto &lineup : validated["lineups"]) {
            const auto key = normalize_name(lineup.value("player", ""));
            bool found = false;
            for (auto &old : snapshot["lineups"])
                if (old.value("gameId", "") == lineup.value("gameId", "") &&
                    normalize_name(old.value("player", "")) == key) {
                    old = lineup;
                    found = true;
                    break;
                }
            if (!found)
                snapshot["lineups"].push_back(lineup);
        }
        if (snapshot["lineups"].size() > 100)
            throw std::invalid_argument("Combined lineups exceed 100 confirmations; clear old imports");
        // Apply all retained confirmations after feature records. A lineup-only import doesn't replace logs.
        for (const auto &lineup : snapshot["lineups"]) {
            const auto key = normalize_name(lineup.value("player", ""));
            if (!snapshot["players"].contains(key))
                continue; // A confirmation alone is not historical player data.
            auto &player = snapshot["players"][key];
            if (player.value("team", "") != lineup.value("team", ""))
                continue; // Do not undo a fresh roster/team assignment.
            player["starterConfirmed"] = lineup["confirmed"];
            player["starterGameId"] = lineup["gameId"];
        }
    }
    snapshot["generatedAt"] = now_iso();
}
std::string csv_template(const json &snapshot) {
    const bool any = !snapshot["games"].empty();
    const auto id = any ? snapshot["games"][0].value("id", "") : "REPLACE_WITH_GAME_ID";
    const auto team = any ? snapshot["games"][0].value("home", "LAL") : "LAL";
    return "gameId,market,player,team,side,line,oddsPercent,book,observedAt,settlement\r\n" + id +
           ",moneyline,," + team + ",home,,,REPLACE_WITH_BOOK," + now_iso() + ",full_game_including_ot\r\n";
}
json score_backtest(const json &doc) {
    if (!doc.contains("observations") || !doc["observations"].is_array() || doc["observations"].empty())
        throw std::invalid_argument("Backtest requires nonempty observations");

    // Require a complete matched sample whenever the caller asks for a market comparison.
    // This avoids silently comparing on a cherry-picked subset.
    bool any_market_comparison = false, all_market_comparisons = true;
    for (const auto &row : doc["observations"]) {
        if (!row.is_object())
            throw std::invalid_argument("Every backtest observation must be an object");
        const bool direct = row.contains("closingNoVigProbability");
        const bool has_closing_side = row.contains("closingOddsPercent");
        const bool has_closing_opposite = row.contains("closingOppositeOddsPercent");
        const bool pair = has_closing_side || has_closing_opposite;
        if (direct && pair)
            throw std::invalid_argument(
                "Use closingNoVigProbability or a closing odds pair, not both");
        if (pair && !(has_closing_side && has_closing_opposite))
            throw std::invalid_argument("Closing market comparison requires both sides of the odds pair");
        const bool matched = direct || pair;
        if (row.contains("closingAt") && !matched)
            throw std::invalid_argument("closingAt requires closing market probabilities or odds");
        any_market_comparison = any_market_comparison || matched;
        all_market_comparisons = all_market_comparisons && matched;
    }
    if (any_market_comparison && !all_market_comparisons)
        throw std::invalid_argument(
            "Market comparison must be supplied for every observation; partial samples can be cherry-picked");

    struct ClusterLoss {
        int observations = 0;
        double brierDifference = 0;
        double logLossDifference = 0;
    };
    struct ComparisonScores {
        int resolved = 0;
        int pushesExcluded = 0;
        double modelBrier = 0;
        double marketBrier = 0;
        double modelLogLoss = 0;
        double marketLogLoss = 0;
        std::map<std::string, ClusterLoss> clusters;
    };
    ComparisonScores overall_comparison;
    std::map<std::string, ComparisonScores> comparisons_by_market;

    double brier = 0, logloss = 0, profit = 0, staked = 0;
    int resolved = 0, selected = 0, wins = 0, pushes = 0;
    for (const auto &row : doc["observations"]) {
        require_number(row, "probability", 0.000001, 0.999999);
        if (row.contains("pushProbability"))
            require_number(row, "pushProbability", 0, 0.999999);
        json price = row;
        normalize_price(price);
        require_string(row, "predictedAt", 20);
        require_string(row, "startsAt", 20);
        require_string(row, "result", 10);
        auto predicted = parse_time(row["predictedAt"]), start = parse_time(row["startsAt"]);
        if (!predicted || !start || predicted >= start)
            throw std::invalid_argument("Leakage guard: predictions must precede game start");
        auto outcome = row["result"].get<std::string>();
        if (outcome != "win" && outcome != "loss" && outcome != "push")
            throw std::invalid_argument("Result must be win, loss or push");
        double p = number(row["probability"]), push = number(row.value("pushProbability", json(0)));
        if (push < 0 || p + push >= 1)
            throw std::invalid_argument("Backtest win + push probability must be below 1");

        double closing_market_probability = 0;
        std::string game_id, market_name = "unclassified";
        if (any_market_comparison) {
            require_string(row, "closingAt", 20);
            const auto closing = parse_time(row["closingAt"].get<std::string>());
            if (!closing || closing < predicted || closing >= start)
                throw std::invalid_argument(
                    "Leakage guard: closingAt must be between predictedAt and game start");
            require_identifier(row, "gameId");
            game_id = row["gameId"].get<std::string>();
            if (row.contains("market")) {
                require_string(row, "market", 80);
                market_name = row["market"].get<std::string>();
            }
            if (row.contains("closingNoVigProbability")) {
                require_number(row, "closingNoVigProbability", 0.000001, 0.999999);
                closing_market_probability = number(row["closingNoVigProbability"]);
            } else {
                require_number(row, "closingOddsPercent", 0.01, 99.99);
                require_number(row, "closingOppositeOddsPercent", 0.01, 99.99);
                const double selected_implied = number(row["closingOddsPercent"]) / 100.0;
                const double opposite_implied = number(row["closingOppositeOddsPercent"]) / 100.0;
                closing_market_probability = selected_implied / (selected_implied + opposite_implied);
            }
        }

        auto d = quote_decimal_odds(price);
        double resolved_p = p / (1 - push);
        if (outcome != "push") {
            double y = outcome == "win" ? 1 : 0;
            const double model_brier = (resolved_p - y) * (resolved_p - y);
            const double model_logloss = -y * std::log(resolved_p) - (1 - y) * std::log(1 - resolved_p);
            brier += model_brier;
            logloss += model_logloss;
            ++resolved;

            if (any_market_comparison) {
                const double market_brier =
                    (closing_market_probability - y) * (closing_market_probability - y);
                const double market_logloss = -y * std::log(closing_market_probability) -
                                              (1 - y) * std::log(1 - closing_market_probability);
                auto record = [&](ComparisonScores &scores) {
                    scores.modelBrier += model_brier;
                    scores.marketBrier += market_brier;
                    scores.modelLogLoss += model_logloss;
                    scores.marketLogLoss += market_logloss;
                    ++scores.resolved;
                    auto &cluster = scores.clusters[game_id];
                    ++cluster.observations;
                    cluster.brierDifference += model_brier - market_brier;
                    cluster.logLossDifference += model_logloss - market_logloss;
                };
                record(overall_comparison);
                record(comparisons_by_market[market_name]);
            }
        } else if (any_market_comparison) {
            ++overall_comparison.pushesExcluded;
            ++comparisons_by_market[market_name].pushesExcluded;
        }
        if (expected_value(p, push, price) > doc.value("minEv", 0.0)) {
            ++selected;
            staked += 1;
            if (outcome == "win") {
                profit += d - 1;
                ++wins;
            } else if (outcome == "loss")
                profit -= 1;
            else
                ++pushes;
        }
    }

    auto comparison_report = [](const ComparisonScores &scores) {
        const auto scored = scores.resolved;
        const double model_brier = scored ? scores.modelBrier / scored : 0;
        const double market_brier = scored ? scores.marketBrier / scored : 0;
        const double model_logloss = scored ? scores.modelLogLoss / scored : 0;
        const double market_logloss = scored ? scores.marketLogLoss / scored : 0;
        const double brier_difference = model_brier - market_brier;
        const double logloss_difference = model_logloss - market_logloss;
        constexpr size_t MIN_CLUSTERS_FOR_INTERVAL = 30;
        auto interval = [&](bool use_brier, double difference) -> json {
            if (scored == 0 || scores.clusters.size() < MIN_CLUSTERS_FOR_INTERVAL)
                return nullptr;
            double squared_cluster_residuals = 0;
            for (const auto &entry : scores.clusters) {
                const double cluster_difference = use_brier ? entry.second.brierDifference
                                                            : entry.second.logLossDifference;
                const double residual = cluster_difference - entry.second.observations * difference;
                squared_cluster_residuals += residual * residual;
            }
            const double clusters = static_cast<double>(scores.clusters.size());
            const double standard_error = std::sqrt(
                (clusters / (clusters - 1.0)) * squared_cluster_residuals / (scored * scored));
            return {{"lower", difference - 1.96 * standard_error},
                    {"upper", difference + 1.96 * standard_error},
                    {"standardError", standard_error},
                    {"method", "approximate normal interval clustered by gameId"}};
        };
        const auto brier_interval = interval(true, brier_difference);
        const auto logloss_interval = interval(false, logloss_difference);
        auto observed_result = [&](double difference) {
            if (!scored)
                return std::string("unscorable_all_pushes");
            if (std::abs(difference) < 1e-12)
                return std::string("tie_on_observed_sample");
            return difference < 0 ? std::string("model_lower_loss_on_observed_sample")
                                  : std::string("closing_market_lower_loss_on_observed_sample");
        };
        auto interval_result = [](const json &ci) {
            if (ci.is_null())
                return std::string("not_estimable_fewer_than_30_resolved_game_clusters");
            if (number(ci["upper"]) < 0)
                return std::string("model_lower_loss_in_approximate_interval");
            if (number(ci["lower"]) > 0)
                return std::string("closing_market_lower_loss_in_approximate_interval");
            return std::string("inconclusive_approximate_interval");
        };
        return json{{"resolved", scored},
                    {"pushesExcluded", scores.pushesExcluded},
                    {"gameClusters", scores.clusters.size()},
                    {"minimumGameClustersForInterval", MIN_CLUSTERS_FOR_INTERVAL},
                    {"modelBrier", scored ? json(model_brier) : json(nullptr)},
                    {"closingMarketBrier", scored ? json(market_brier) : json(nullptr)},
                    {"brierDifferenceModelMinusMarket", scored ? json(brier_difference) : json(nullptr)},
                    {"brierSkillScore", scored && market_brier > 0
                                             ? json((market_brier - model_brier) / market_brier)
                                             : json(nullptr)},
                    {"brierObservedResult", observed_result(brier_difference)},
                    {"brierDifference95CI", brier_interval},
                    {"brierIntervalResult", interval_result(brier_interval)},
                    {"modelLogLoss", scored ? json(model_logloss) : json(nullptr)},
                    {"closingMarketLogLoss", scored ? json(market_logloss) : json(nullptr)},
                    {"logLossDifferenceModelMinusMarket", scored ? json(logloss_difference) : json(nullptr)},
                    {"logLossSkillScore", scored && market_logloss > 0
                                              ? json((market_logloss - model_logloss) / market_logloss)
                                              : json(nullptr)},
                    {"logLossObservedResult", observed_result(logloss_difference)},
                    {"logLossDifference95CI", logloss_interval},
                    {"logLossIntervalResult", interval_result(logloss_interval)}};
    };

    json market_comparison = nullptr;
    if (any_market_comparison) {
        json by_market = json::object();
        for (const auto &entry : comparisons_by_market)
            by_market[entry.first] = comparison_report(entry.second);
        market_comparison = {{"basis", "Paired proper scores against the closing no-vig market probability."},
                             {"matchedObservations", doc["observations"].size()},
                             {"allObservationsMatched", true},
                             {"overall", comparison_report(overall_comparison)},
                             {"byMarket", by_market},
                             {"note",
                              "Descriptive out-of-sample comparison only. A lower score is better; negative "
                              "model-minus-market differences favor the model. The game-clustered normal "
                              "interval is approximate and is not proof of independent observations, no data "
                              "leakage, or future profitability. Closing prices and results must be authentic."}};
    }
    return {{"synthetic", doc.value("synthetic", false)},
            {"observations", doc["observations"].size()},
            {"resolved", resolved},
            {"brier", resolved ? json(brier / resolved) : json(nullptr)},
            {"logLoss", resolved ? json(logloss / resolved) : json(nullptr)},
            {"selectedBets", selected},
            {"wins", wins},
            {"pushes", pushes},
            {"profitUnits", profit},
            {"roiPercent", staked ? json(profit / staked * 100) : json(nullptr)},
            {"marketComparisonStatus", any_market_comparison ? "scored" : "not_provided"},
            {"marketComparison", market_comparison},
            {"note",
             "Scores supplied historical predictions, not a trained model. Timestamp checks cannot independently "
             "verify data provenance. Use chronological held-out games, obtainable prices, complete matched "
             "closing-market probabilities and enough independent game clusters; no metric alone proves "
             "a future edge."}};
}
} // namespace edgecourt
