#include "app.hpp"
#include "embedded_assets.hpp"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <random>
#include <regex>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif

namespace edgecourt {
namespace {
std::string session_token() {
    std::random_device random;
    std::string token;
    const char *hex = "0123456789abcdef";
    for (int i = 0; i < 64; ++i)
        token += hex[random() % 16];
    return token;
}
void reply(httplib::Response &response, const json &value, int status = 200) {
    response.status = status;
    response.set_content(value.dump(), "application/json; charset=utf-8");
    response.set_header("Cache-Control", "no-store");
}
bool safe_host(std::string host) {
    static const std::regex allowed(
        "^(localhost|127\\.0\\.0\\.1|\\[::1\\]|[a-zA-Z0-9-]+\\.e2b\\.app)(:[0-9]{1,5})?$");
    return std::regex_match(host, allowed);
}
} // namespace
std::filesystem::path default_data_dir() {
#ifdef _WIN32
    if (const auto *dir = std::getenv("LOCALAPPDATA"))
        return std::filesystem::path(dir) / "Edgecourt";
#else
    if (const auto *dir = std::getenv("XDG_DATA_HOME"))
        return std::filesystem::path(dir) / "edgecourt";
    if (const auto *home = std::getenv("HOME"))
        return std::filesystem::path(home) / ".local" / "share" / "edgecourt";
#endif
    return std::filesystem::current_path() / "edgecourt-data";
}
App::App(std::filesystem::path dir, const std::string &initial_mode, std::filesystem::path history_dir,
         bool fetch_public)
    : data_dir_(std::move(dir)),
      history_dir_(history_dir.empty() ? data_dir_ / "history" : std::move(history_dir)),
      feeds_(data_dir_ / "cache"), token_(session_token()), feeds_enabled_(fetch_public) {
    std::filesystem::create_directories(data_dir_);
    std::string mode = initial_mode;
    if (mode.empty())
        mode = load("settings.json").value("mode", "demo");
    if (mode != "live")
        mode = "demo";
    snapshot_ = mode == "demo" ? demo_snapshot() : empty_snapshot("live");
    base_snapshot_ = snapshot_;
    try {
        history_ = load_history_bundle(history_dir_);
    } catch (const std::exception &error) {
        history_.status["state"] = "unavailable";
        history_.status["message"] = std::string("Local history was not loaded: ") + error.what();
    }
    apply_history(snapshot_, history_);
    imports_ = {{"quotes", json::array()},
                {"players", json::object()},
                {"teams", json::object()},
                {"games", json::array()},
                {"lineups", json::array()}};
    auto stored = load("imports.json");
    if (!stored.empty()) {
        try {
            imports_ = validate_import(stored);
        } catch (const std::exception &error) {
            snapshot_["sources"].push_back(
                {{"id", "imports"},
                 {"label", "Saved user imports"},
                 {"state", "unavailable"},
                 {"message", std::string("Saved import was not loaded: ") + error.what()},
                 {"count", 0},
                 {"updatedAt", ""}});
        }
    }
    if (mode == "live") {
        merge_import(snapshot_, imports_);
        requested_ = feeds_enabled_;
        snapshot_["refreshing"] = requested_;
    }
    rebuild_locked();
    routes();
    worker_ = std::thread(&App::worker_loop, this);
}
App::~App() {
    stop();
    if (worker_.joinable())
        worker_.join();
}
json App::load(const std::string &name) {
    try {
        auto path = data_dir_ / name;
        if (std::filesystem::exists(path) && std::filesystem::file_size(path) < 4 * 1024 * 1024) {
            std::ifstream file(path);
            json value;
            file >> value;
            if (value.is_object())
                return value;
        }
    } catch (...) {
    }
    return json::object();
}
void App::save(const std::string &name, const json &value) {
    auto destination = data_dir_ / name, temp = data_dir_ / (name + ".tmp");
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file)
            throw std::runtime_error("Cannot write the application data directory");
        file << value.dump(2);
        file.flush();
        if (!file)
            throw std::runtime_error("Cannot save application data");
    }
#ifdef _WIN32
    if (!MoveFileExW(temp.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot replace saved application data");
#else
    std::filesystem::rename(temp, destination);
#endif
}
void App::rebuild_locked() {
    snapshot_["revision"] = ++revision_;
    snapshot_["historicalData"] = history_.status;
    dashboard_ = build_dashboard(snapshot_);
    dashboard_["historicalData"] = history_.status;
    dashboard_["publicFeedsEnabled"] = feeds_enabled_;
    if (!feeds_enabled_)
        dashboard_["sources"].push_back(
            {{"id", "offline"},
             {"label", "Offline workspace"},
             {"state", "idle"},
             {"count", 0},
             {"updatedAt", ""},
             {"message", "Public network requests are disabled by --offline. Local history "
                         "and explicit user imports are used; freshness guards still apply."}});
    dashboard_["sources"].push_back(
        {{"id", "basketball-reference-history"},
         {"label", "Basketball Reference historical play-by-play"},
         {"state", snapshot_.value("mode", "demo") == "demo"
                       ? "idle"
                       : history_.status.value("state", "not_installed")},
         {"updatedAt", history_.status.value("exportedAt", "")},
         {"count", history_.status.value("games", 0)},
         {"message",
          snapshot_.value("mode", "demo") == "demo"
              ? "Local history is not applied to synthetic Demo. See the historical coverage panel."
              : history_.status.value("message", "")}});
    dashboard_["authToken"] = token_;
    if (snapshot_.value("mode", "demo") == "live" &&
        (!imports_["quotes"].empty() || !imports_["players"].empty() || !imports_["games"].empty() ||
         !imports_["lineups"].empty()))
        dashboard_["sources"].push_back(
            {{"id", "imports"},
             {"label", "Local user imports"},
             {"state", "user_import"},
             {"updatedAt", snapshot_.value("generatedAt", "")},
             {"count", imports_["quotes"].size()},
             {"message", "Locally saved quotes and supplied feature records. Price freshness guards still "
                         "apply; no external verification."}});
}
int App::bind(const std::string &host, int port) {
    if (server_.bind_to_port(host, port))
        return port;
    return server_.bind_to_any_port(host);
}
void App::listen() {
    server_.listen_after_bind();
}
void App::stop() {
    {
        std::lock_guard<std::mutex> guard(mutex_);
        stopping_ = true;
        cancel_refresh_ = true;
    }
    condition_.notify_all();
    server_.stop();
}
void App::refresh() {
    std::lock_guard<std::mutex> guard(mutex_);
    if (snapshot_.value("mode", "demo") == "demo") {
        snapshot_ = demo_snapshot();
        rebuild_locked();
        return;
    }
    if (!feeds_enabled_) {
        snapshot_["generatedAt"] = now_iso();
        rebuild_locked();
        return;
    }
    requested_ = true;
    snapshot_["refreshing"] = true;
    dashboard_["refreshing"] = true;
    condition_.notify_one();
}
void App::change_mode(const std::string &mode) {
    if (mode != "live" && mode != "demo")
        throw std::invalid_argument("Mode must be live or demo");
    std::lock_guard<std::mutex> guard(mutex_);
    if (snapshot_.value("mode", "") == mode)
        return;
    save("settings.json", {{"mode", mode}});
    cancel_refresh_ = true;
    ++generation_;
    snapshot_ = mode == "demo" ? demo_snapshot() : empty_snapshot("live");
    base_snapshot_ = snapshot_;
    if (mode == "live") {
        apply_history(snapshot_, history_);
        merge_import(snapshot_, imports_);
        requested_ = feeds_enabled_;
        snapshot_["refreshing"] = requested_;
    } else
        requested_ = false;
    rebuild_locked();
    condition_.notify_one();
}
json App::import(const json &request) {
    const auto format = request.value("format", "json");
    if (!request.contains("data") || !request["data"].is_string())
        throw std::invalid_argument("Provide text in the data field");
    const auto content = request["data"].get<std::string>();
    if (content.size() > 4 * 1024 * 1024)
        throw std::invalid_argument("Import is larger than 4 MB");
    json incoming;
    if (format == "csv")
        incoming = parse_csv_import(content);
    else if (format == "html")
        incoming = scrape_saved_html(content, request.value("player", ""), request.value("team", ""),
                                     request.value("observedAt", ""));
    else if (format == "json")
        incoming = validate_import(json::parse(content));
    else
        throw std::invalid_argument("Format must be json, csv or html");
    std::lock_guard<std::mutex> guard(mutex_);
    if (incoming.value("demoOnly", false) && snapshot_.value("mode", "demo") == "live")
        throw std::invalid_argument("Declared synthetic example data may only be imported into Demo mode.");
    json candidate = snapshot_;
    merge_import(candidate, incoming);
    // Do not permit orphan prices, even if they could otherwise be rendered as unavailable.
    for (const auto &q : incoming["quotes"]) {
        bool found = false;
        for (const auto &g : candidate["games"])
            if (g.value("id", "") == q.value("gameId", "") &&
                (q.value("team", "") == g.value("home", "") || q.value("team", "") == g.value("away", ""))) {
                found = true;
                if (q.value("market", "") == "moneyline" &&
                    q.value("team", "") != g.value(q.value("side", "home"), ""))
                    throw std::invalid_argument("Moneyline side does not match the selection team");
                break;
            }
        if (!found)
            throw std::invalid_argument("A quote references an unknown game or a team outside its matchup. "
                                        "Include the matching game in JSON or use a displayed game ID.");
    }
    for (const auto &lineup : incoming["lineups"]) {
        bool found = false;
        for (const auto &game : candidate["games"])
            if (game.value("id", "") == lineup.value("gameId", "") &&
                (game.value("home", "") == lineup.value("team", "") ||
                 game.value("away", "") == lineup.value("team", ""))) {
                found = true;
                break;
            }
        if (!found)
            throw std::invalid_argument("A lineup references an unknown game or a team outside its matchup");
    }
    json next_imports = imports_;
    if (snapshot_.value("mode", "demo") == "live") {
        json aggregate = empty_snapshot("live");
        aggregate["quotes"] = imports_["quotes"];
        aggregate["players"] = imports_["players"];
        aggregate["teams"] = imports_["teams"];
        aggregate["games"] = imports_["games"];
        aggregate["lineups"] = imports_["lineups"];
        merge_import(aggregate, incoming);
        for (auto key : {"quotes", "players", "teams", "games", "lineups"})
            next_imports[key] = aggregate[key];
        if (next_imports.dump().size() > 4 * 1024 * 1024)
            throw std::invalid_argument(
                "Combined saved imports exceed 4 MB. Clear saved imports or import a smaller dataset.");
        save("imports.json", next_imports);
        imports_ = std::move(next_imports);
    }
    // All validation and persistence succeeded before changing the visible state.
    snapshot_ = std::move(candidate);
    rebuild_locked();
    return {{"ok", true},
            {"quotes", incoming["quotes"].size()},
            {"players", incoming["players"].size()},
            {"teams", incoming["teams"].size()},
            {"games", incoming["games"].size()},
            {"lineups", incoming["lineups"].size()},
            {"persisted", snapshot_.value("mode", "demo") == "live"},
            {"message", snapshot_.value("mode", "demo") == "demo"
                            ? "Imported into synthetic demo only. Demo imports reset on refresh."
                            : "Saved locally. Verify all imported prices and settlement rules."}};
}
void App::worker_loop() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopping_) {
        condition_.wait_for(lock, std::chrono::seconds(60), [this] { return stopping_ || requested_; });
        if (stopping_)
            break;
        if (snapshot_.value("mode", "demo") != "live") {
            // Refresh only synthetic timestamps. Never re-stamp a user-imported price.
            for (auto &q : snapshot_["quotes"])
                if (q.value("source", "") == "synthetic")
                    q["observedAt"] = now_iso();
            for (auto &g : snapshot_["games"])
                if (g.value("source", "") == "synthetic")
                    g["dataAsOf"] = now_iso();
            for (auto &item : snapshot_["players"].items())
                if (item.value().value("source", "") == "synthetic" && item.value().contains("live"))
                    item.value()["live"]["dataAsOf"] = now_iso();
            snapshot_["generatedAt"] = now_iso();
            rebuild_locked();
            continue;
        }
        requested_ = false;
        if (!feeds_enabled_)
            continue;
        auto generation = generation_;
        auto previous = snapshot_;
        snapshot_["refreshing"] = true;
        dashboard_["refreshing"] = true;
        cancel_refresh_ = false;
        lock.unlock();
        json updated;
        std::string error;
        try {
            updated = feeds_.refresh(previous, &cancel_refresh_);
        } catch (const std::exception &exception) {
            error = exception.what();
        }
        lock.lock();
        if (stopping_)
            break;
        if (generation != generation_ || snapshot_.value("mode", "") != "live")
            continue;
        if (!error.empty()) {
            snapshot_["sources"].push_back({{"id", "refresh-error"},
                                            {"label", "Refresh worker"},
                                            {"state", "unavailable"},
                                            {"count", 0},
                                            {"updatedAt", now_iso()},
                                            {"message", error}});
        } else {
            base_snapshot_ = updated;
            apply_history(updated, history_);
            merge_import(updated, imports_);
            snapshot_ = std::move(updated);
        }
        snapshot_["refreshing"] = false;
        rebuild_locked();
    }
}
json App::reload_history() {
    // Validate the entire replacement off-lock; a bad file never changes the accepted bundle.
    auto incoming = load_history_bundle(history_dir_);
    std::lock_guard<std::mutex> guard(mutex_);
    if (snapshot_.value("mode", "demo") == "live") {
        auto candidate = base_snapshot_;
        candidate["refreshing"] = snapshot_.value("refreshing", false);
        apply_history(candidate, incoming);
        merge_import(candidate, imports_);
        snapshot_ = std::move(candidate);
    }
    history_ = std::move(incoming);
    rebuild_locked();
    return {{"ok", true},
            {"message", "Local history reloaded. " + history_.status.value("message", "")},
            {"historicalData", history_.status}};
}
bool App::authorize(const httplib::Request &req, httplib::Response &res) {
    if (req.get_header_value("X-Edgecourt-Token") != token_) {
        reply(res, {{"error", "Invalid session token. Reload the dashboard."}}, 403);
        return false;
    }
    auto origin = req.get_header_value("Origin"), host = req.get_header_value("Host");
    if (!origin.empty() && origin != "http://" + host && origin != "https://" + host) {
        reply(res, {{"error", "Cross-origin writes are not allowed"}}, 403);
        return false;
    }
    return true;
}
void App::routes() {
    server_.set_payload_max_length(5 * 1024 * 1024);
    server_.set_read_timeout(10, 0);
    server_.set_write_timeout(10, 0);
    server_.set_keep_alive_max_count(100);
    server_.set_pre_routing_handler([](const httplib::Request &req, httplib::Response &res) {
        if (!safe_host(req.get_header_value("Host"))) {
            reply(res, {{"error", "Untrusted Host header"}}, 403);
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });
    server_.set_post_routing_handler([](const httplib::Request &, httplib::Response &res) {
        res.set_header("X-Content-Type-Options", "nosniff");
        res.set_header("Referrer-Policy", "no-referrer");
        res.set_header(
            "Content-Security-Policy",
            "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; "
            "connect-src 'self'; font-src 'self'; object-src 'none'; base-uri 'none'; form-action 'self'");
        // No frame-ancestor/X-Frame-Options restriction: the Arena live preview embeds this UI.
    });
    server_.Get("/", [](const httplib::Request &, httplib::Response &res) {
        res.set_content(assets::index_html, "text/html; charset=utf-8");
        res.set_header("Cache-Control", "no-cache");
    });
    server_.Get("/styles.css", [](const httplib::Request &, httplib::Response &res) {
        res.set_content(assets::styles_css, "text/css; charset=utf-8");
    });
    server_.Get("/app.js", [](const httplib::Request &, httplib::Response &res) {
        res.set_content(assets::app_js, "application/javascript; charset=utf-8");
    });
    server_.Get("/favicon.svg", [](const httplib::Request &, httplib::Response &res) {
        res.set_content(assets::favicon_svg, "image/svg+xml");
    });
    server_.Get("/api/health", [](const httplib::Request &, httplib::Response &res) {
        reply(res, {{"app", "Edgecourt"}, {"version", VERSION}, {"ok", true}});
    });
    server_.Get("/api/state", [this](const httplib::Request &, httplib::Response &res) {
        std::lock_guard<std::mutex> guard(mutex_);
        reply(res, expire_dashboard(dashboard_));
    });
    server_.Get("/api/template.csv", [this](const httplib::Request &, httplib::Response &res) {
        std::lock_guard<std::mutex> guard(mutex_);
        res.set_content(csv_template(snapshot_), "text/csv; charset=utf-8");
        res.set_header("Content-Disposition", "attachment; filename=edgecourt-quotes-template.csv");
    });
    server_.Post("/api/refresh", [this](const httplib::Request &req, httplib::Response &res) {
        if (!authorize(req, res))
            return;
        refresh();
        reply(res, {{"ok", true}, {"message", "Refresh requested"}});
    });
    server_.Post("/api/mode", [this](const httplib::Request &req, httplib::Response &res) {
        if (!authorize(req, res))
            return;
        try {
            auto body = json::parse(req.body);
            change_mode(body.value("mode", ""));
            reply(res, {{"ok", true}});
        } catch (const std::exception &error) {
            reply(res, {{"error", error.what()}}, 400);
        }
    });
    server_.Post("/api/import", [this](const httplib::Request &req, httplib::Response &res) {
        if (!authorize(req, res))
            return;
        try {
            reply(res, import(json::parse(req.body)));
        } catch (const std::exception &error) {
            reply(res, {{"error", error.what()}}, 400);
        }
    });
    server_.Post("/api/history/reload", [this](const httplib::Request &req, httplib::Response &res) {
        if (!authorize(req, res))
            return;
        try {
            reply(res, reload_history());
        } catch (const std::exception &error) {
            reply(
                res,
                {{"error", std::string("History reload failed; previous bundle retained: ") + error.what()}},
                400);
        }
    });
    server_.Post("/api/clear-imports", [this](const httplib::Request &req, httplib::Response &res) {
        if (!authorize(req, res))
            return;
        try {
            std::lock_guard<std::mutex> guard(mutex_);
            json cleared = {{"quotes", json::array()},
                            {"players", json::object()},
                            {"teams", json::object()},
                            {"games", json::array()},
                            {"lineups", json::array()}};
            save("imports.json", cleared);
            imports_ = cleared;
            cancel_refresh_ = true;
            ++generation_;
            snapshot_ = snapshot_.value("mode", "demo") == "demo" ? demo_snapshot() : empty_snapshot("live");
            base_snapshot_ = snapshot_;
            apply_history(snapshot_, history_);
            requested_ = snapshot_.value("mode", "") == "live" && feeds_enabled_;
            snapshot_["refreshing"] = requested_;
            rebuild_locked();
            condition_.notify_one();
            reply(res, {{"ok", true}});
        } catch (const std::exception &error) {
            reply(res, {{"error", error.what()}}, 400);
        }
    });
    server_.set_error_handler([](const httplib::Request &, httplib::Response &res) {
        if (res.body.empty())
            reply(res, {{"error", res.status == 413 ? "Import payload is too large" : "Route not found"}},
                  res.status);
    });
    server_.set_exception_handler(
        [](const httplib::Request &, httplib::Response &res, std::exception_ptr exception) {
            std::string message = "Request failed";
            try {
                if (exception)
                    std::rethrow_exception(exception);
            } catch (const std::exception &error) {
                message = error.what();
            }
            reply(res, {{"error", message}}, 500);
        });
}
} // namespace edgecourt
