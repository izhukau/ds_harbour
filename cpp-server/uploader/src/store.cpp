#include "uploader/store.hpp"

#include <pqxx/pqxx>
#include <random>
#include <sstream>

namespace {

const char *kSchema = R"sql(
CREATE TABLE IF NOT EXISTS requests (
    id UUID PRIMARY KEY,
    status TEXT NOT NULL,
    total INT NOT NULL,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE TABLE IF NOT EXISTS payments (
    transaction_id TEXT PRIMARY KEY,
    request_id UUID NOT NULL,
    store_id TEXT NOT NULL,
    coffee_type TEXT NOT NULL,
    price NUMERIC(12, 2) NOT NULL,
    currency TEXT NOT NULL,
    loyalty_card_id TEXT,
    status TEXT NOT NULL,
    remote_id TEXT,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS payments_request_id_idx ON payments (request_id);
)sql";

std::string newUuid() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<std::uint64_t> dist;
    std::uint64_t hi = dist(rng);
    std::uint64_t lo = dist(rng);

    hi = (hi & ~(0xFull << 12)) | (0x4ull << 12);
    lo = (lo & ~(0x3ull << 62)) | (0x2ull << 62);

    char buf[37];
    std::snprintf(buf, sizeof(buf), "%08x-%04x-%04x-%04x-%012llx", static_cast<unsigned>(hi >> 32),
                  static_cast<unsigned>((hi >> 16) & 0xFFFF), static_cast<unsigned>(hi & 0xFFFF),
                  static_cast<unsigned>(lo >> 48),
                  static_cast<unsigned long long>(lo & 0xFFFFFFFFFFFFull));
    return std::string(buf);
}

} // namespace

PaymentStore::PaymentStore(ShardRouter &router) : router_(router) {}

void PaymentStore::ensureSchema() {
    for (std::size_t i = 0; i < router_.count(); ++i) {
        auto conn = router_.open(i);
        pqxx::work tx{*conn};
        tx.exec(kSchema);
        tx.commit();
    }
}

std::string PaymentStore::createRequest(const std::vector<Payment> &payments) {
    std::string requestId = newUuid();

    for (const auto &p : payments) {
        auto conn = router_.open(router_.indexFor(p.transactionId));
        pqxx::work tx{*conn};
        tx.exec("INSERT INTO payments (transaction_id, request_id, store_id, coffee_type, price, "
                "currency, loyalty_card_id, status) "
                "VALUES ($1, $2, $3, $4, $5, $6, $7, 'PENDING') "
                "ON CONFLICT (transaction_id) DO NOTHING",
                pqxx::params{p.transactionId, requestId, p.storeId, p.coffeeType, p.price,
                             p.currency, p.loyaltyCardId});
        tx.commit();
    }

    {
        auto conn = router_.open(router_.indexFor(requestId));
        pqxx::work tx{*conn};
        tx.exec("INSERT INTO requests (id, status, total) VALUES ($1, 'PENDING', $2)",
                pqxx::params{requestId, static_cast<int>(payments.size())});
        tx.commit();
    }

    return requestId;
}

std::optional<RequestStatus> PaymentStore::getStatus(const std::string &requestId) {
    RequestStatus status;
    status.requestId = requestId;

    {
        auto conn = router_.open(router_.indexFor(requestId));
        pqxx::work tx{*conn};
        pqxx::result res =
            tx.exec("SELECT status, total FROM requests WHERE id = $1", pqxx::params{requestId});
        tx.commit();
        if (res.empty()) {
            return std::nullopt;
        }
        status.status = res[0][0].as<std::string>();
        status.total = res[0][1].as<int>();
    }

    for (std::size_t i = 0; i < router_.count(); ++i) {
        auto conn = router_.open(i);
        pqxx::work tx{*conn};
        pqxx::result res =
            tx.exec("SELECT status, count(*) FROM payments WHERE request_id = $1 GROUP BY status",
                    pqxx::params{requestId});
        tx.commit();
        for (const auto &row : res) {
            const auto state = row[0].as<std::string>();
            const int n = row[1].as<int>();
            if (state == "DONE") {
                status.done += n;
            } else if (state == "FAILED") {
                status.failed += n;
            } else {
                status.pending += n;
            }
        }
    }

    return status;
}

std::vector<std::string> PaymentStore::claimPending(int limitPerShard) {
    std::vector<std::string> claimed;
    for (std::size_t i = 0; i < router_.count(); ++i) {
        auto conn = router_.open(i);
        pqxx::work tx{*conn};
        pqxx::result res = tx.exec(
            "UPDATE requests SET status = 'PROCESSING' WHERE id IN "
            "(SELECT id FROM requests WHERE status = 'PENDING' ORDER BY created_at LIMIT $1 "
            "FOR UPDATE SKIP LOCKED) RETURNING id",
            pqxx::params{limitPerShard});
        tx.commit();
        for (const auto &row : res) {
            claimed.push_back(row[0].as<std::string>());
        }
    }
    return claimed;
}

std::vector<StoredPayment> PaymentStore::pendingPayments(const std::string &requestId) {
    std::vector<StoredPayment> payments;
    for (std::size_t i = 0; i < router_.count(); ++i) {
        auto conn = router_.open(i);
        pqxx::work tx{*conn};
        pqxx::result res =
            tx.exec("SELECT transaction_id, store_id, coffee_type, price, currency, "
                    "loyalty_card_id FROM payments WHERE request_id = $1 AND status = 'PENDING'",
                    pqxx::params{requestId});
        tx.commit();
        for (const auto &row : res) {
            StoredPayment p;
            p.transactionId = row[0].as<std::string>();
            p.requestId = requestId;
            p.storeId = row[1].as<std::string>();
            p.coffeeType = row[2].as<std::string>();
            p.price = row[3].as<double>();
            p.currency = row[4].as<std::string>();
            p.loyaltyCardId = row[5].is_null() ? "" : row[5].as<std::string>();
            payments.push_back(std::move(p));
        }
    }
    return payments;
}

void PaymentStore::markPayment(const std::string &transactionId, const std::string &status,
                               const std::string &remoteId) {
    auto conn = router_.open(router_.indexFor(transactionId));
    pqxx::work tx{*conn};
    tx.exec("UPDATE payments SET status = $2, remote_id = $3 WHERE transaction_id = $1",
            pqxx::params{transactionId, status,
                         remoteId.empty() ? std::optional<std::string>{}
                                          : std::optional<std::string>{remoteId}});
    tx.commit();
}

void PaymentStore::finishRequest(const std::string &requestId) {
    auto conn = router_.open(router_.indexFor(requestId));
    pqxx::work tx{*conn};
    tx.exec("UPDATE requests SET status = 'DONE' WHERE id = $1", pqxx::params{requestId});
    tx.commit();
}
