/**
 * @file test_ratelimit_redis.cpp
 * @brief Integration test for the RateLimit Redis enforcement path (the
 *        sliding-window EVAL). The unit tests cover disabled/whitelist/
 *        fail-open; this exercises the actual counter against live Redis.
 *
 * Suite name RateLimitRedisTest → integration bucket (needs Redis).
 */

#include <gtest/gtest.h>

#include "cache/Cache.hpp"
#include "security/RateLimit.hpp"
#include "test_helpers.hpp"

namespace RL = Security::RateLimit;

namespace {

class RateLimitRedisTest : public ::testing::Test {
protected:
    void SetUp() override {
        if (!TestHelpers::is_redis_available())
            GTEST_SKIP() << "Redis not available";
        TestHelpers::reset_all_globals();
        Cache::initialize(TestHelpers::redis_url(), 2);
        // Clear any leftover window keys from a previous run.
        try {
            auto& r = Cache::get().get_client();
            std::vector<std::string> keys;
            r.keys("rl:sw:*", std::back_inserter(keys));
            for (const auto& k : keys)
                r.del(k);
        } catch (...) {}
    }
    void TearDown() override { TestHelpers::reset_all_globals(); }

    static RL::Config cfg(int requests) {
        RL::Config c;
        c.enabled = true;
        c.requests = requests;
        c.window_sec = 60;
        c.fail_open = true;
        return c;
    }
};

TEST_F(RateLimitRedisTest, AllowsUpToLimitThenDenies) {
    RL::Limiter lim(cfg(/*requests=*/3));
    const std::string id = "ip:10.1.2.3";

    for (int i = 0; i < 3; ++i) {
        auto d = lim.check(id);
        EXPECT_TRUE(d.allowed) << "request " << i << " should be allowed";
    }
    auto over = lim.check(id);
    EXPECT_FALSE(over.allowed) << "4th request in the window must be denied";
    EXPECT_GT(over.retry_after_sec, 0);
}

TEST_F(RateLimitRedisTest, SeparateIdentitiesHaveSeparateWindows) {
    RL::Limiter lim(cfg(/*requests=*/1));
    EXPECT_TRUE(lim.check("ip:1.1.1.1").allowed);
    EXPECT_FALSE(lim.check("ip:1.1.1.1").allowed);
    // A different identity is unaffected.
    EXPECT_TRUE(lim.check("ip:2.2.2.2").allowed);
}

}  // namespace
