#include "uploader/lb.hpp"

BackendPool::BackendPool(std::vector<std::string> urls) {
    backends_.reserve(urls.size());
    for (auto &url : urls) {
        // strip a single trailing slash so Location = url + path stays clean
        if (!url.empty() && url.back() == '/') {
            url.pop_back();
        }
        backends_.push_back(BackendStatus{url});
    }
}

size_t BackendPool::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return backends_.size();
}

void BackendPool::setHealthy(size_t index, bool healthy) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (index < backends_.size()) {
        backends_[index].healthy = healthy;
    }
}

std::optional<std::string> BackendPool::pickNext() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (backends_.empty()) {
        return std::nullopt;
    }

    for (size_t i = 0; i < backends_.size(); ++i) {
        size_t idx = (next_ + i) % backends_.size();
        if (backends_[idx].healthy) {
            next_ = (idx + 1) % backends_.size();
            ++backends_[idx].redirects;
            return backends_[idx].url;
        }
    }
    return std::nullopt;
}

std::vector<BackendStatus> BackendPool::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return backends_;
}
