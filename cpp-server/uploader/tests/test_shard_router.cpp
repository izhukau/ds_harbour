#include "uploader/shard_router.hpp"

#include <array>
#include <doctest/doctest.h>
#include <stdexcept>
#include <string>

TEST_CASE("indexFor stays in range and is deterministic") {
    ShardRouter router({"shard-a", "shard-b", "shard-c"});

    for (const std::string &key : {"txn-0001", "txn-0002", "store-london-01"}) {
        std::size_t idx = router.indexFor(key);
        CHECK(idx < router.count());
        CHECK(idx == router.indexFor(key));
    }
}

TEST_CASE("keys spread across every shard") {
    ShardRouter router({"a", "b", "c", "d"});
    std::array<int, 4> hits{};

    for (int i = 0; i < 2000; ++i) {
        hits[router.indexFor("txn-" + std::to_string(i))]++;
    }

    for (int h : hits) {
        CHECK(h > 0);
    }
}

TEST_CASE("count reflects the configured shards") {
    CHECK(ShardRouter({"a"}).count() == 1);
    CHECK(ShardRouter({"a", "b"}).count() == 2);
    CHECK(ShardRouter({"a", "b", "c"}).count() == 3);
}

TEST_CASE("a single shard routes everything to index zero") {
    ShardRouter router({"only"});
    CHECK(router.indexFor("anything") == 0);
    CHECK(router.indexFor("else") == 0);
}

TEST_CASE("empty configuration is rejected") {
    CHECK_THROWS_AS(ShardRouter({}), std::invalid_argument);
}
