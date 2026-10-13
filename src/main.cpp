#include "app.hpp"
#include <csignal>
#include <fstream>
#include <iostream>
#include <thread>
#ifdef _WIN32
#include <shellapi.h>
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
volatile std::sig_atomic_t interrupted = 0;
void signal_handler(int) {
    interrupted = 1;
}
void open_browser(const std::string &url) {
#ifdef _WIN32
    auto result = reinterpret_cast<INT_PTR>(
        ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    if (result <= 32)
        std::cerr << "Could not open your browser automatically. Open " << url << " manually.\n";
#else
    auto child = fork();
    if (child == 0) {
        execlp("xdg-open", "xdg-open", url.c_str(), static_cast<char *>(nullptr));
        _exit(127);
    }
    if (child > 0) {
        int status = 0;
        waitpid(child, &status, 0);
    }
#endif
}
void help() {
    std::cout << "Edgecourt " << edgecourt::VERSION << " - local NBA market research\n\n"
              << "  Edgecourt.exe                        Start and open your browser\n"
              << "  Edgecourt.exe --live                 Request public feeds (+ optional API-Sports/SharpAPI keys)\n"
              << "  Edgecourt.exe --demo                 Synthetic demonstration\n"
              << "  --port 8741                          Preferred HTTP port\n"
              << "  --host 127.0.0.1                     Loopback only (default)\n"
              << "  --no-browser                         Do not launch a browser\n"
              << "  --data-dir PATH                      Override local storage\n"
              << "  --offline                            Disable public requests; use local inputs\n"
              << "  --history-dir PATH                   Local historical feature bundle directory\n"
              << "  --backtest PATH                      Score predictions; compare with closing market if supplied\n"
              << "  --version / --help                   Print version / usage\n\n"
              << "Research only. The model is experimental and uncalibrated. No guaranteed profits.\n";
}
} // namespace
int main(int argc, char **argv) {
    try {
        bool browser = true, fetch_public = true;
        int port = 8741;
        std::string host = "127.0.0.1", mode, backtest;
        std::filesystem::path history_dir;
        auto dir = edgecourt::default_data_dir();
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            auto value = [&]() {
                if (i + 1 >= argc)
                    throw std::invalid_argument("Missing argument after " + arg);
                return std::string(argv[++i]);
            };
            if (arg == "--help" || arg == "-h") {
                help();
                return 0;
            } else if (arg == "--version") {
                std::cout << edgecourt::VERSION << "\n";
                return 0;
            } else if (arg == "--no-browser")
                browser = false;
            else if (arg == "--offline")
                fetch_public = false;
            else if (arg == "--demo")
                mode = "demo";
            else if (arg == "--live")
                mode = "live";
            else if (arg == "--data-dir")
                dir = value();
            else if (arg == "--history-dir")
                history_dir = value();
            else if (arg == "--backtest")
                backtest = value();
            else if (arg == "--host")
                host = value();
            else if (arg == "--port") {
                auto v = value();
                size_t used = 0;
                port = std::stoi(v, &used);
                if (used != v.size() || port < 1024 || port > 65535)
                    throw std::invalid_argument("Port must be 1024..65535");
            } else
                throw std::invalid_argument("Unknown option: " + arg);
        }
        if (!backtest.empty()) {
            std::ifstream input(backtest);
            if (!input)
                throw std::runtime_error("Cannot open backtest file");
            edgecourt::json document;
            input >> document;
            std::cout << edgecourt::score_backtest(document).dump(2) << "\n";
            return 0;
        }
        if (host != "127.0.0.1" && host != "0.0.0.0")
            throw std::invalid_argument("Host must be 127.0.0.1 (desktop) or 0.0.0.0 (preview)");
        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);
        edgecourt::App app(dir, mode, history_dir, fetch_public);
        int bound = app.bind(host, port);
        if (bound <= 0)
            throw std::runtime_error("Cannot bind a local HTTP port");
        const auto url = "http://127.0.0.1:" + std::to_string(bound);
        std::cout << "\n  EDGECOURT  |  NBA market research\n\n  Dashboard: " << url
                  << "\n  Local data: " << dir.string()
                  << "\n\n  Keep this window open while using the dashboard.\n  Press Ctrl+C or close this "
                     "window to stop.\n  Experimental model. Demo data is synthetic. No auto-betting.\n"
                  << std::endl;
        if (host == "0.0.0.0")
            std::cout << "  Preview listening on 0.0.0.0:" << bound << "; use the provided preview URL.\n"
                      << std::flush;
        std::atomic<bool> done{false};
        std::thread monitor([&] {
            while (!done) {
                if (interrupted) {
                    app.stop();
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
        std::thread opener;
        if (browser)
            opener = std::thread([&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(350));
                open_browser(url);
            });
        app.listen();
        done = true;
        if (monitor.joinable())
            monitor.join();
        if (opener.joinable())
            opener.join();
        app.stop();
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Edgecourt: " << error.what() << "\nUse --help for instructions.\n";
        return 1;
    }
}
