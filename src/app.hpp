#pragma once
#include "core.hpp"
#include "feeds.hpp"
#include "httplib/httplib.h"
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <thread>

namespace edgecourt {
class App {
  public:
    App(std::filesystem::path data_dir, const std::string &initial_mode);
    ~App();
    int bind(const std::string &host, int port);
    void listen();
    void stop();

  private:
    void routes();
    void worker_loop();
    void rebuild_locked();
    void refresh();
    void change_mode(const std::string &mode);
    json import(const json &request);
    void save(const std::string &name, const json &value);
    json load(const std::string &name);
    bool authorize(const httplib::Request &req, httplib::Response &res);
    std::filesystem::path data_dir_;
    PublicFeeds feeds_;
    httplib::Server server_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::thread worker_;
    json snapshot_, dashboard_, imports_;
    std::string token_;
    bool stopping_ = false, requested_ = false;
    unsigned long generation_ = 0;
    int revision_ = 0;
    std::atomic<bool> cancel_refresh_{false};
};
std::filesystem::path default_data_dir();
} // namespace edgecourt
