#pragma once
#include "uploader/payment.hpp"
#include "uploader/shard_router.hpp"

#include <optional>
#include <string>
#include <vector>

struct StoredPayment {
    std::string transactionId;
    std::string requestId;
    std::string storeId;
    std::string coffeeType;
    double price = 0.0;
    std::string currency;
    std::string loyaltyCardId;
};

struct RequestStatus {
    std::string requestId;
    std::string status;
    int total = 0;
    int done = 0;
    int failed = 0;
    int pending = 0;
};

class PaymentStore {
  public:
    explicit PaymentStore(ShardRouter &router);

    void ensureSchema();

    std::string createRequest(const std::vector<Payment> &payments);
    std::optional<RequestStatus> getStatus(const std::string &requestId);

    std::vector<std::string> claimPending(int limitPerShard = 4);
    std::vector<StoredPayment> pendingPayments(const std::string &requestId);
    void markPayment(const std::string &transactionId, const std::string &status,
                     const std::string &remoteId);
    void finishRequest(const std::string &requestId);

  private:
    ShardRouter &router_;
};
