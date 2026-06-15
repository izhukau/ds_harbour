#include "uploader/shard_router.hpp"

#include <pqxx/pqxx>
#include <stdexcept>

ShardRouter::ShardRouter(std::vector<std::string> connectionStrings)
    : connections_(std::move(connectionStrings)) {
    if (connections_.empty()) {
        throw std::invalid_argument("shard router needs at least one connection string");
    }
}

std::size_t ShardRouter::indexFor(const std::string &key) const {
    std::uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : key) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    return static_cast<std::size_t>(hash % connections_.size());
}

std::unique_ptr<pqxx::connection> ShardRouter::open(std::size_t shard) const {
    if (shard >= connections_.size()) {
        throw std::out_of_range("shard index out of range");
    }
    return std::make_unique<pqxx::connection>(connections_[shard]);
}
