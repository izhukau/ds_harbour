#include <httplib.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

#include "uploader/client.hpp"
#include "uploader/shard_router.hpp"
#include "uploader/store.hpp"

namespace {

using nlohmann::json;

std::optional<Payment> paymentFromJson(const json &j, std::string &error) {
    for (const char *field : {"storeId", "transactionId", "coffeeType", "currency"}) {
        if (!j.contains(field) || !j[field].is_string() || j[field].get<std::string>().empty()) {
            error = std::string("missing or invalid field: ") + field;
            return std::nullopt;
        }
    }
    if (!j.contains("price") || !j["price"].is_number()) {
        error = "missing or invalid field: price";
        return std::nullopt;
    }

    std::string loyalty = j.value("loyaltyCardId", std::string{});
    return Payment{j["storeId"].get<std::string>(),    j["transactionId"].get<std::string>(),
                   j["coffeeType"].get<std::string>(), j["price"].get<double>(),
                   j["currency"].get<std::string>(),   loyalty};
}

void processRequest(PaymentStore &store, const std::string &remoteUrl,
                    const std::string &requestId) {
    auto payments = store.pendingPayments(requestId);
    std::cout << "[worker] request " << requestId << ": " << payments.size() << " payment(s)\n";

    for (const auto &sp : payments) {
        Payment p{sp.storeId, sp.transactionId, sp.coffeeType,
                  sp.price,   sp.currency,      sp.loyaltyCardId};

        SendResult res = sendWithRetry(remoteUrl, p);
        bool ok = res.responded && (res.status == 200 || res.status == 201);

        std::string remoteId;
        if (ok && !res.body.empty()) {
            try {
                remoteId = json::parse(res.body).value("paymentId", std::string{});
            } catch (const std::exception &) {
            }
        }

        store.markPayment(sp.transactionId, ok ? "DONE" : "FAILED", remoteId);
    }

    store.finishRequest(requestId);
    std::cout << "[worker] request " << requestId << " done\n";
}

void workerLoop(PaymentStore &store, const std::string &remoteUrl, std::atomic<bool> &running) {
    while (running) {
        std::vector<std::string> claimed;
        try {
            claimed = store.claimPending();
        } catch (const std::exception &e) {
            std::cerr << "[worker] claim error: " << e.what() << "\n";
        }

        if (claimed.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            continue;
        }

        for (const auto &id : claimed) {
            try {
                processRequest(store, remoteUrl, id);
            } catch (const std::exception &e) {
                std::cerr << "[worker] process error for " << id << ": " << e.what() << "\n";
            }
        }
    }
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 4) {
        std::cerr << "usage: " << argv[0]
                  << " <http-port> <remote-url> <shard-conn-1> [shard-conn-2 ...]\n";
        return 1;
    }

    int port = 0;
    try {
        port = std::stoi(argv[1]);
    } catch (const std::exception &) {
        std::cerr << "invalid port: " << argv[1] << "\n";
        return 1;
    }

    const std::string remoteUrl = argv[2];

    std::vector<std::string> shards;
    for (int i = 3; i < argc; ++i) {
        shards.emplace_back(argv[i]);
    }

    ShardRouter router(shards);
    PaymentStore store(router);

    try {
        store.ensureSchema();
    } catch (const std::exception &e) {
        std::cerr << "schema setup failed: " << e.what() << "\n";
        return 1;
    }

    std::atomic<bool> running{true};
    std::thread worker([&] { workerLoop(store, remoteUrl, running); });

    httplib::Server server;

    server.Post("/requests", [&](const httplib::Request &req, httplib::Response &res) {
        json body;
        try {
            body = json::parse(req.body);
        } catch (const std::exception &) {
            res.status = 400;
            res.set_content(R"({"error":"invalid json"})", "application/json");
            return;
        }

        if (!body.contains("payments") || !body["payments"].is_array() ||
            body["payments"].empty()) {
            res.status = 400;
            res.set_content(R"({"error":"payments must be a non-empty array"})",
                            "application/json");
            return;
        }

        std::vector<Payment> payments;
        for (const auto &item : body["payments"]) {
            std::string error;
            auto p = paymentFromJson(item, error);
            if (!p) {
                res.status = 400;
                res.set_content(json{{"error", error}}.dump(), "application/json");
                return;
            }
            payments.push_back(*p);
        }

        try {
            std::string requestId = store.createRequest(payments);
            res.status = 202;
            res.set_content(json{{"requestId", requestId}, {"accepted", payments.size()}}.dump(),
                            "application/json");
        } catch (const std::exception &e) {
            res.status = 500;
            res.set_content(json{{"error", e.what()}}.dump(), "application/json");
        }
    });

    server.Get(R"(/requests/([0-9a-fA-F\-]+))",
               [&](const httplib::Request &req, httplib::Response &res) {
                   const std::string id = req.matches[1];
                   try {
                       auto status = store.getStatus(id);
                       if (!status) {
                           res.status = 404;
                           res.set_content(R"({"error":"unknown request id"})", "application/json");
                           return;
                       }
                       res.set_content(json{{"requestId", status->requestId},
                                            {"status", status->status},
                                            {"total", status->total},
                                            {"done", status->done},
                                            {"failed", status->failed},
                                            {"pending", status->pending}}
                                           .dump(),
                                       "application/json");
                   } catch (const std::exception &e) {
                       res.status = 500;
                       res.set_content(json{{"error", e.what()}}.dump(), "application/json");
                   }
               });

    server.Get("/health", [](const httplib::Request &, httplib::Response &res) {
        res.set_content(R"({"status":"ok"})", "application/json");
    });

    std::cout << "async payment server on :" << port << " -> remote " << remoteUrl << " across "
              << router.count() << " shard(s)\n";

    if (!server.listen("0.0.0.0", port)) {
        std::cerr << "failed to listen on port " << port << "\n";
        running = false;
        worker.join();
        return 1;
    }

    running = false;
    worker.join();
    return 0;
}
