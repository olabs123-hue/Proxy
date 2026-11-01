#include <gtest/gtest.h>

#include "rate_limiter.h"

using edge::RateLimiter;

TEST(RateLimiter, AllowsBurstThenRejects) {
  RateLimiter rl(10, 5);
  for (int i = 0; i < 5; ++i) EXPECT_TRUE(rl.allow(1, 100.0).allowed) << i;
  auto d = rl.allow(1, 100.0);
  EXPECT_FALSE(d.allowed);
  EXPECT_NEAR(d.retry_after, 0.1, 1e-9);  // one token at 10/s
}

TEST(RateLimiter, RefillsOverTime) {
  RateLimiter rl(10, 5);
  for (int i = 0; i < 5; ++i) rl.allow(1, 0.0);
  EXPECT_FALSE(rl.allow(1, 0.05).allowed);  // half a token
  EXPECT_TRUE(rl.allow(1, 0.11).allowed);   // one token refilled
  EXPECT_FALSE(rl.allow(1, 0.11).allowed);
  // Long idle period refills only up to the burst size.
  for (int i = 0; i < 5; ++i) EXPECT_TRUE(rl.allow(1, 1000.0).allowed);
  EXPECT_FALSE(rl.allow(1, 1000.0).allowed);
}

TEST(RateLimiter, ClientsAreIndependent) {
  RateLimiter rl(1, 1);
  EXPECT_TRUE(rl.allow(1, 0).allowed);
  EXPECT_FALSE(rl.allow(1, 0).allowed);
  EXPECT_TRUE(rl.allow(2, 0).allowed);
}

TEST(RateLimiter, DisabledAllowsEverything) {
  RateLimiter rl(0, 0);
  EXPECT_FALSE(rl.enabled());
  for (int i = 0; i < 1000; ++i) EXPECT_TRUE(rl.allow(1, 0).allowed);
  EXPECT_EQ(rl.size(), 0u);
}

TEST(RateLimiter, SweepDropsFullBuckets) {
  RateLimiter rl(10, 5);
  for (uint32_t k = 0; k < 100; ++k) rl.allow(k, 0.0);
  EXPECT_EQ(rl.size(), 100u);
  EXPECT_EQ(rl.sweep(0.0), 0u);   // just used: not full yet
  EXPECT_EQ(rl.sweep(10.0), 100u);  // long idle: all full again
  EXPECT_EQ(rl.size(), 0u);
}

TEST(RateLimiter, MemoryStaysBounded) {
  RateLimiter rl(1, 1, /*max_entries=*/50);
  for (uint32_t k = 0; k < 10000; ++k) rl.allow(k, 0.0);
  EXPECT_LE(rl.size(), 50u);
}
