#include "treeseries/mem_free_queue.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <latch>
#include <ranges>
#include <thread>
#include <vector>

TEST_CASE("MemFreeQueue drains in order and stays exhausted", "[mem_free_queue]") {
  MemFreeQueue queue;

  std::vector<std::uint32_t> ids;
  ids.reserve(kMemSlabCount);
  while (const auto id = queue.TryPop()) {
    ids.push_back(*id);
  }

  REQUIRE(std::ranges::equal(ids, std::views::iota(0U, kMemSlabCount)));
  REQUIRE(queue.Remaining() == 0U);
  REQUIRE_FALSE(queue.TryPop().has_value());
}

TEST_CASE("MemFreeQueue blocks are contiguous and disjoint from pops", "[mem_free_queue]") {
  MemFreeQueue queue;

  const auto first = queue.TryPop();
  REQUIRE(first.has_value());
  REQUIRE(*first == 0U);

  const auto block = queue.TryPopBlock(4U);
  REQUIRE(block.has_value());
  REQUIRE(block->first == 1U);
  REQUIRE(block->second == 4U);

  const auto next = queue.TryPop();
  REQUIRE(next.has_value());
  REQUIRE(*next == 5U);
  REQUIRE(queue.Remaining() == kMemSlabCount - 6U);
}

TEST_CASE("MemFreeQueue zero-size block is not exhaustion", "[mem_free_queue]") {
  MemFreeQueue queue;

  REQUIRE_FALSE(queue.TryPopBlock(0U).has_value());
  REQUIRE(queue.Remaining() == kMemSlabCount);

  const auto id = queue.TryPop();
  REQUIRE(id.has_value());
  REQUIRE(*id == 0U);
}

TEST_CASE("MemFreeQueue oversized block cannot re-issue IDs", "[mem_free_queue]") {
  MemFreeQueue queue;

  const auto taken = queue.TryPop();
  REQUIRE(taken.has_value());
  REQUIRE(*taken == 0U);

  const auto block = queue.TryPopBlock(UINT32_MAX);
  REQUIRE(block.has_value());
  REQUIRE(block->first == 1U);
  REQUIRE(block->second == kMemSlabCount - 1U);

  REQUIRE(queue.Remaining() == 0U);
  REQUIRE_FALSE(queue.TryPop().has_value());
  REQUIRE_FALSE(queue.TryPopBlock(1U).has_value());
}

TEST_CASE("MemFreeQueue issues no duplicates under concurrent pops", "[mem_free_queue]") {
  constexpr auto kThreads = 8U;

  MemFreeQueue queue;
  std::latch start{static_cast<std::ptrdiff_t>(kThreads)};
  std::array<std::vector<std::uint32_t>, kThreads> per_thread;
  std::array<std::thread, kThreads> threads;

  for (auto i = 0U; i < kThreads; ++i) {
    threads[i] = std::thread([&queue, &per_thread, &start, i] {
      per_thread[i].reserve(kMemSlabCount / kThreads + 1);
      start.arrive_and_wait();
      while (const auto id = queue.TryPop()) {
        per_thread[i].push_back(*id);
      }
    });
  }

  for (auto& thread : threads) {
    thread.join();
  }

  auto all = per_thread | std::views::join | std::ranges::to<std::vector>();
  std::ranges::sort(all);
  REQUIRE(std::ranges::equal(all, std::views::iota(0U, kMemSlabCount)));
}
