#pragma once
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace pqxx {
class connection;
}

class ShardRouter {
  public:
    explicit ShardRouter(std::vector<std::string> connectionStrings);

    std::size_t count() const { return connections_.size(); }
    std::size_t indexFor(const std::string &key) const;

    std::unique_ptr<pqxx::connection> open(std::size_t shard) const;

  private:
    std::vector<std::string> connections_;
};
