#pragma once
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct BackendStatus {
    std::string url;
    bool healthy = true;
    size_t redirects = 0;
};

// Thread-safe pool of backend instances with round-robin selection
// over the currently healthy subset.
class BackendPool {
  public:
    explicit BackendPool(std::vector<std::string> urls);

    size_t size() const;
    void setHealthy(size_t index, bool healthy);

    // Returns the next healthy backend (round-robin), or nullopt if all are down.
    std::optional<std::string> pickNext();

    std::vector<BackendStatus> snapshot() const;

  private:
    mutable std::mutex mutex_;
    std::vector<BackendStatus> backends_;
    size_t next_ = 0;
};
