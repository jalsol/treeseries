#include "treeseries/mem_evict_queue.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <latch>
#include <memory>
#include <ranges>
#include <thread>
#include <vector>

namespace {

// The ring owns a kMemSlabCount-cell array, so it lives on the heap.
auto MakeQueue() -> std::unique_ptr<MemEvictQueue> { return std::make_unique<MemEvictQueue>(); }

}  // namespace

TEST_CASE("MemEvictQueue pushes and pops a slab", "[mem_evict_queue]") {
  auto queue = MakeQueue();

  queue->Push(SlabId{7});
  auto slab_id = SlabId::kInvalid;
  REQUIRE(queue->TryPop(&slab_id));
  REQUIRE(slab_id == SlabId{7});
  REQUIRE_FALSE(queue->TryPop(&slab_id));
}

TEST_CASE("MemEvictQueue hands each slab to exactly one consumer", "[mem_evict_queue]") {
  constexpr auto kConsumers = 8U;
  constexpr auto kTotal = 1U << 17;
  static_assert(kTotal <= kMemSlabCount, "More slabs in flight than the ring holds");

  auto queue = MakeQueue();
  std::latch start{static_cast<std::ptrdiff_t>(kConsumers) + 1};
  std::array<std::vector<std::uint32_t>, kConsumers> received;
  std::atomic<std::uint32_t> taken{};

  std::thread producer{[&queue, &start] {
    start.arrive_and_wait();
    for (auto id = 0U; id < kTotal; ++id) {
      queue->Push(SlabId{id});
    }
  }};

  std::array<std::thread, kConsumers> consumers;
  for (auto i = 0U; i < kConsumers; ++i) {
    consumers[i] = std::thread([&queue, &start, &received, &taken, i] {
      start.arrive_and_wait();
      auto& mine = received[i];
      while (taken.load(std::memory_order_relaxed) < kTotal) {
        auto slab_id = SlabId::kInvalid;
        if (queue->TryPop(&slab_id)) {
          mine.push_back(slab_id);
          taken.fetch_add(1U, std::memory_order_relaxed);
        }
      }
    });
  }

  producer.join();
  for (auto& consumer : consumers) {
    consumer.join();
  }

  // Two consumers claiming the same cell, or a claim dropping its slab, shows up
  // as a duplicate or a hole here.
  auto all = std::vector<std::uint32_t>{};
  all.reserve(kTotal);
  for (auto& mine : received) {
    all.insert(all.end(), mine.begin(), mine.end());
  }
  std::ranges::sort(all);
  REQUIRE(all.size() == static_cast<std::size_t>(kTotal));
  REQUIRE(std::ranges::equal(all, std::views::iota(0U, kTotal)));
}
