#include "treeseries/mem_full_queue.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <latch>
#include <memory>
#include <ranges>
#include <thread>
#include <vector>

namespace {

// The ring owns a kMemSlabCount-entry buffer, so it lives on the heap.
auto MakeQueue() -> std::unique_ptr<MemFullQueue> { return std::make_unique<MemFullQueue>(); }

}  // namespace

TEST_CASE("MemFullQueue pushes and pops a slab", "[mem_full_queue]") {
  auto queue = MakeQueue();

  queue->Push(SlabId{7});
  auto slab_id = SlabId::kInvalid;
  REQUIRE(queue->TryPop(&slab_id));
  REQUIRE(slab_id == SlabId{7});
  REQUIRE_FALSE(queue->TryPop(&slab_id));
}

TEST_CASE("MemFullQueue holds every slab and drains in push order", "[mem_full_queue]") {
  auto queue = MakeQueue();

  // The ring is sized to the slab count, so holding all the slabs is the fullest
  // it can ever get; Push has no full case to report, and a push beyond it means
  // a slab was queued twice.
  for (auto id = 0U; id < kMemSlabCount; ++id) {
    queue->Push(SlabId{id});
  }

  auto slab_id = SlabId::kInvalid;
  for (auto id = 0U; id < kMemSlabCount; ++id) {
    REQUIRE(queue->TryPop(&slab_id));
    REQUIRE(slab_id == SlabId{id});
  }
  REQUIRE_FALSE(queue->TryPop(&slab_id));
}

TEST_CASE("MemFullQueue loses nothing under concurrent producers", "[mem_full_queue]") {
  constexpr auto kProducers = 8U;
  constexpr auto kPerProducer = 1U << 14;
  constexpr auto kTotal = kProducers * kPerProducer;

  auto queue = MakeQueue();
  std::latch start{static_cast<std::ptrdiff_t>(kProducers) + 1};
  std::vector<std::uint32_t> received;
  received.reserve(kTotal);

  std::array<std::thread, kProducers> producers;
  for (auto i = 0U; i < kProducers; ++i) {
    producers[i] = std::thread([&queue, &start, i] {
      start.arrive_and_wait();
      for (auto j = 0U; j < kPerProducer; ++j) {
        // Pushing fewer slabs than the ring holds means no producer ever laps the
        // consumer, so every push lands.
        queue->Push(SlabId{i * kPerProducer + j});
      }
    });
  }

  std::thread consumer{[&queue, &start, &received] {
    start.arrive_and_wait();
    while (received.size() < kTotal) {
      auto slab_id = SlabId::kInvalid;
      if (queue->TryPop(&slab_id)) {
        received.push_back(slab_id);
      }
    }
  }};

  for (auto& producer : producers) {
    producer.join();
  }
  consumer.join();

  std::ranges::sort(received);
  REQUIRE(received.size() == static_cast<std::size_t>(kTotal));
  REQUIRE(std::ranges::equal(received, std::views::iota(0U, kTotal)));
}
