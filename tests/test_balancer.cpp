#include <gtest/gtest.h>

#include <map>
#include <set>

#include "balancer.h"

using namespace edge;

namespace {
std::vector<std::unique_ptr<Backend>> make_backends(int n) {
  std::vector<std::unique_ptr<Backend>> v;
  for (int i = 0; i < n; ++i) {
    std::string err;
    auto b = make_backend("127.0.0.1:" + std::to_string(9000 + i), &err);
    EXPECT_NE(b, nullptr) << err;
    v.push_back(std::move(b));
  }
  return v;
}
}  // namespace

TEST(MakeBackend, ParsesAndValidates) {
  std::string err;
  auto b = make_backend("127.0.0.1:8080", &err);
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(b->name, "127.0.0.1:8080");
  EXPECT_EQ(b->port, 8080);
  EXPECT_EQ(make_backend("nocolon", &err), nullptr);
  EXPECT_EQ(make_backend("127.0.0.1:0", &err), nullptr);
  EXPECT_EQ(make_backend("127.0.0.1:99999", &err), nullptr);
  EXPECT_EQ(make_backend(":80", &err), nullptr);
}

TEST(Balancer, RoundRobinCyclesEvenly) {
  Balancer lb(make_backends(3), Balancer::Policy::RoundRobin);
  std::map<Backend*, int> counts;
  for (int i = 0; i < 300; ++i) ++counts[lb.pick("k", {})];
  ASSERT_EQ(counts.size(), 3u);
  for (auto& kv : counts) EXPECT_EQ(kv.second, 100);
}

TEST(Balancer, RoundRobinSkipsUnhealthyAndExcluded) {
  Balancer lb(make_backends(3), Balancer::Policy::RoundRobin);
  Backend* down = lb.backends()[1].get();
  down->healthy = false;
  for (int i = 0; i < 50; ++i) EXPECT_NE(lb.pick("k", {}), down);
  Backend* excluded = lb.backends()[0].get();
  for (int i = 0; i < 50; ++i) {
    Backend* p = lb.pick("k", {excluded});
    EXPECT_EQ(p, lb.backends()[2].get());  // only healthy, non-excluded backend
  }
}

TEST(Balancer, FailsOpenWhenEverythingIsUnhealthy) {
  Balancer lb(make_backends(2), Balancer::Policy::RoundRobin);
  for (auto& b : lb.backends()) b->healthy = false;
  EXPECT_NE(lb.pick("k", {}), nullptr);
  // ...but never returns an excluded backend, and gives up when all are excluded.
  std::vector<Backend*> all = {lb.backends()[0].get(), lb.backends()[1].get()};
  EXPECT_EQ(lb.pick("k", all), nullptr);
  EXPECT_EQ(lb.pick("k", {all[0]}), all[1]);
}

TEST(Balancer, ConsistentHashIsDeterministic) {
  Balancer lb(make_backends(4), Balancer::Policy::ConsistentHash);
  for (int i = 0; i < 200; ++i) {
    std::string key = "host/page" + std::to_string(i);
    EXPECT_EQ(lb.pick(key, {}), lb.pick(key, {}));
  }
}

TEST(Balancer, ConsistentHashSpreadsKeys) {
  Balancer lb(make_backends(4), Balancer::Policy::ConsistentHash);
  std::map<Backend*, int> counts;
  const int kKeys = 8000;
  for (int i = 0; i < kKeys; ++i) ++counts[lb.pick("host/page" + std::to_string(i), {})];
  ASSERT_EQ(counts.size(), 4u);
  for (auto& kv : counts) {
    EXPECT_GT(kv.second, kKeys * 0.15);  // ideal is 25%
    EXPECT_LT(kv.second, kKeys * 0.35);
  }
}

TEST(Balancer, ConsistentHashOnlyMovesKeysOfTheFailedBackend) {
  Balancer lb(make_backends(4), Balancer::Policy::ConsistentHash);
  const int kKeys = 4000;
  std::vector<Backend*> before(kKeys);
  for (int i = 0; i < kKeys; ++i) before[i] = lb.pick("k" + std::to_string(i), {});
  Backend* failed = lb.backends()[2].get();
  failed->healthy = false;
  int moved = 0, owned_by_failed = 0;
  for (int i = 0; i < kKeys; ++i) {
    Backend* after = lb.pick("k" + std::to_string(i), {});
    EXPECT_NE(after, failed);
    if (before[i] == failed) {
      ++owned_by_failed;
    } else if (after != before[i]) {
      ++moved;  // a key that did NOT live on the failed backend must not move
    }
  }
  EXPECT_GT(owned_by_failed, 0);
  EXPECT_EQ(moved, 0);
  // Recovery restores the original mapping exactly.
  failed->healthy = true;
  for (int i = 0; i < kKeys; ++i) EXPECT_EQ(lb.pick("k" + std::to_string(i), {}), before[i]);
}

TEST(Balancer, ConsistentHashRetryPicksADifferentBackend) {
  Balancer lb(make_backends(3), Balancer::Policy::ConsistentHash);
  Backend* first = lb.pick("some/key", {});
  Backend* second = lb.pick("some/key", {first});
  ASSERT_NE(second, nullptr);
  EXPECT_NE(second, first);
  Backend* third = lb.pick("some/key", {first, second});
  ASSERT_NE(third, nullptr);
  EXPECT_EQ(std::set<Backend*>({first, second, third}).size(), 3u);
}

TEST(Balancer, ActiveProbesUseRiseAndFall) {
  Balancer lb(make_backends(1), Balancer::Policy::RoundRobin);
  Backend* b = lb.backends()[0].get();
  lb.report_probe(b, false, /*fall=*/3, /*rise=*/2);
  lb.report_probe(b, false, 3, 2);
  EXPECT_TRUE(b->healthy);  // two failures are not enough
  lb.report_probe(b, false, 3, 2);
  EXPECT_FALSE(b->healthy);
  lb.report_probe(b, true, 3, 2);
  EXPECT_FALSE(b->healthy);  // one success is not enough
  lb.report_probe(b, true, 3, 2);
  EXPECT_TRUE(b->healthy);
}

TEST(Balancer, PassiveFailuresMarkDownAndSuccessResetsTheCount) {
  Balancer lb(make_backends(1), Balancer::Policy::RoundRobin);
  Backend* b = lb.backends()[0].get();
  lb.report_failure(b, 3);
  lb.report_failure(b, 3);
  lb.report_success(b);  // resets the streak
  lb.report_failure(b, 3);
  lb.report_failure(b, 3);
  EXPECT_TRUE(b->healthy);
  lb.report_failure(b, 3);
  EXPECT_FALSE(b->healthy);
}
