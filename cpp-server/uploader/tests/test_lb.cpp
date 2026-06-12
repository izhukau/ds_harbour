#include "uploader/lb.hpp"

#include <doctest/doctest.h>

TEST_CASE("round-robin cycles through all healthy backends") {
    BackendPool pool({"http://a:1", "http://b:2", "http://c:3"});

    CHECK(pool.pickNext() == "http://a:1");
    CHECK(pool.pickNext() == "http://b:2");
    CHECK(pool.pickNext() == "http://c:3");
    CHECK(pool.pickNext() == "http://a:1");
}

TEST_CASE("unhealthy backends are skipped") {
    BackendPool pool({"http://a:1", "http://b:2", "http://c:3"});
    pool.setHealthy(1, false);

    CHECK(pool.pickNext() == "http://a:1");
    CHECK(pool.pickNext() == "http://c:3");
    CHECK(pool.pickNext() == "http://a:1");
}

TEST_CASE("returns nullopt when every backend is down") {
    BackendPool pool({"http://a:1", "http://b:2"});
    pool.setHealthy(0, false);
    pool.setHealthy(1, false);

    CHECK(!pool.pickNext().has_value());
}

TEST_CASE("backend recovers after becoming healthy again") {
    BackendPool pool({"http://a:1", "http://b:2"});
    pool.setHealthy(0, false);
    pool.setHealthy(1, false);
    CHECK(!pool.pickNext().has_value());

    pool.setHealthy(1, true);
    CHECK(pool.pickNext() == "http://b:2");
}

TEST_CASE("empty pool returns nullopt") {
    BackendPool pool({});
    CHECK(!pool.pickNext().has_value());
}

TEST_CASE("trailing slash is stripped from backend urls") {
    BackendPool pool({"http://a:1/"});
    CHECK(pool.pickNext() == "http://a:1");
}

TEST_CASE("snapshot counts redirects per backend") {
    BackendPool pool({"http://a:1", "http://b:2"});
    pool.pickNext();
    pool.pickNext();
    pool.pickNext();

    auto snap = pool.snapshot();
    REQUIRE(snap.size() == 2);
    CHECK(snap[0].redirects == 2);
    CHECK(snap[1].redirects == 1);
}
