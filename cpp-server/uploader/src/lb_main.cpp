#include <httplib.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "uploader/lb.hpp"

namespace {

constexpr int kHealthIntervalSec = 3;
constexpr int kHealthTimeoutSec = 2;
// Backend has no actuator, so probe the real API: any HTTP answer below 500
// means the instance is up and serving requests.
constexpr const char *kHealthPath = "/api/v1/payments?storeId=lb-health-check";

void healthCheckLoop(BackendPool &pool, std::atomic<bool> &running) {
    while (running) {
        auto backends = pool.snapshot();
        for (size_t i = 0; i < backends.size(); ++i) {
            httplib::Client probe(backends[i].url);
            probe.set_connection_timeout(kHealthTimeoutSec, 0);
            probe.set_read_timeout(kHealthTimeoutSec, 0);

            httplib::Result res = probe.Get(kHealthPath);
            bool healthy = res && res->status < 500;

            if (healthy != backends[i].healthy) {
                std::cout << "[health] " << backends[i].url << " is now "
                          << (healthy ? "UP" : "DOWN") << "\n";
            }
            pool.setHealthy(i, healthy);
        }
        std::this_thread::sleep_for(std::chrono::seconds(kHealthIntervalSec));
    }
}

std::string statusJson(const BackendPool &pool) {
    std::ostringstream out;
    out << "{\"backends\":[";
    auto backends = pool.snapshot();
    for (size_t i = 0; i < backends.size(); ++i) {
        if (i > 0) {
            out << ",";
        }
        out << "{\"url\":\"" << backends[i].url
            << "\",\"healthy\":" << (backends[i].healthy ? "true" : "false")
            << ",\"redirects\":" << backends[i].redirects << "}";
    }
    out << "]}";
    return out.str();
}

void redirect(BackendPool &pool, const httplib::Request &req, httplib::Response &res) {
    std::optional<std::string> backend = pool.pickNext();
    if (!backend) {
        res.status = 503;
        res.set_content("{\"error\":\"no healthy backends\"}", "application/json");
        return;
    }

    // req.target keeps the original path + query string
    const std::string &target = req.target.empty() ? req.path : req.target;
    res.status = 302;
    res.set_header("Location", *backend + target);

    std::cout << "[lb] " << req.method << " " << target << " -> " << *backend << "\n";
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        std::cerr << "usage: " << argv[0] << " <port> <backend-url> [more-backend-urls...]\n";
        return 1;
    }

    int port = 0;
    try {
        port = std::stoi(argv[1]);
    } catch (const std::exception &) {
        std::cerr << "invalid port: " << argv[1] << "\n";
        return 1;
    }

    std::vector<std::string> urls;
    urls.reserve(argc - 2);
    for (int arg = 2; arg < argc; ++arg) {
        urls.emplace_back(argv[arg]);
    }

    BackendPool pool(urls);

    std::atomic<bool> running{true};
    std::thread health([&] { healthCheckLoop(pool, running); });

    httplib::Server server;

    server.Get("/lb/status", [&](const httplib::Request &, httplib::Response &res) {
        res.set_content(statusJson(pool), "application/json");
    });

    auto handler = [&](const httplib::Request &req, httplib::Response &res) {
        redirect(pool, req, res);
    };
    server.Get(".*", handler);
    server.Post(".*", handler);
    server.Put(".*", handler);
    server.Patch(".*", handler);
    server.Delete(".*", handler);

    std::cout << "load balancer listening on :" << port << " with " << urls.size()
              << " backend(s)\n";

    if (!server.listen("0.0.0.0", port)) {
        std::cerr << "failed to listen on port " << port << "\n";
        running = false;
        health.join();
        return 1;
    }

    running = false;
    health.join();
    return 0;
}
