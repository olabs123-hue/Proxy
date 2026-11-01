#include <gtest/gtest.h>

#include "cache.h"

using namespace edge;

namespace {
std::shared_ptr<const Response> resp(size_t body_bytes, int status = 200) {
  auto r = std::make_shared<Response>();
  r->status = status;
  r->body = std::string(body_bytes, 'x');
  return r;
}
}  // namespace

TEST(Cache, HitMissAndExpiry) {
  Cache c(1 << 20);
  EXPECT_EQ(c.get("a", 0), nullptr);
  ASSERT_TRUE(c.put("a", resp(10), 5.0, 100.0));
  EXPECT_NE(c.get("a", 104.9), nullptr);
  EXPECT_EQ(c.get("a", 105.0), nullptr);  // expired entries are removed on access
  EXPECT_EQ(c.entries(), 0u);
  EXPECT_EQ(c.bytes(), 0u);
}

TEST(Cache, RejectsZeroTtlAndOversizedObjects) {
  Cache c(1 << 20, /*max_object=*/1000);
  EXPECT_FALSE(c.put("a", resp(10), 0.0, 0));
  EXPECT_FALSE(c.put("b", resp(5000), 60.0, 0));
  EXPECT_EQ(c.entries(), 0u);
}

TEST(Cache, EvictsLeastRecentlyUsedWhenFull) {
  Cache c(1000, 1000);
  // Each entry is roughly 128 + key + body bytes; 3 fit, 4 do not.
  ASSERT_TRUE(c.put("a", resp(200), 60, 0));
  ASSERT_TRUE(c.put("b", resp(200), 60, 0));
  ASSERT_TRUE(c.put("c", resp(200), 60, 0));
  ASSERT_NE(c.get("a", 1), nullptr);  // touch a: b is now the oldest
  ASSERT_TRUE(c.put("d", resp(200), 60, 1));
  EXPECT_EQ(c.get("b", 2), nullptr);
  EXPECT_NE(c.get("a", 2), nullptr);
  EXPECT_NE(c.get("c", 2), nullptr);
  EXPECT_NE(c.get("d", 2), nullptr);
  EXPECT_GE(c.evictions(), 1u);
  EXPECT_LE(c.bytes(), 1000u);
}

TEST(Cache, ReplacingAKeyUpdatesAccounting) {
  Cache c(1 << 20);
  c.put("a", resp(100), 60, 0);
  size_t before = c.bytes();
  c.put("a", resp(500), 60, 0);
  EXPECT_EQ(c.entries(), 1u);
  EXPECT_EQ(c.bytes(), before + 400);
  c.erase("a");
  EXPECT_EQ(c.bytes(), 0u);
}

TEST(Cache, PurgeExpiredRemovesOnlyExpired) {
  Cache c(1 << 20);
  c.put("short", resp(10), 1, 0);
  c.put("long", resp(10), 100, 0);
  EXPECT_EQ(c.purge_expired(10), 1u);
  EXPECT_EQ(c.get("short", 10), nullptr);
  EXPECT_NE(c.get("long", 10), nullptr);
}

TEST(Cache, HoldersKeepResponsesAliveAfterEviction) {
  Cache c(400, 400);
  c.put("a", resp(100), 60, 0);
  auto held = c.get("a", 1);
  ASSERT_NE(held, nullptr);
  c.put("b", resp(100), 60, 1);
  c.put("c", resp(100), 60, 1);  // evicts a
  EXPECT_EQ(c.get("a", 2), nullptr);
  EXPECT_EQ(held->body.size(), 100u);  // still valid for the in-flight write
}
