#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "treeseries/slab.hpp"

class MemFullQueue {
  static_assert(std::has_single_bit(kMemSlabCount), "Mem slab count isn't power of 2");

 public:
  MemFullQueue() {
    for (auto pos = 0u; pos < kMemSlabCount; ++pos) {
      cells_[pos].sequence.store(pos, std::memory_order_relaxed);
    }
  }

  void Push(SlabId slab_id) {
    const auto pos = tail_.fetch_add(1u, std::memory_order_relaxed);
    auto& cell = cells_[pos & kMask];

    if (cell.sequence.load(std::memory_order_acquire) != pos) {
      std::fprintf(stderr,
                   "MemFullQueue: push at position %u laps the consumer; a slab is in "
                   "two queues or was never taken from one\n",
                   static_cast<unsigned>(pos));
      std::abort();
    }

    cell.data.store(slab_id, std::memory_order_relaxed);
    cell.sequence.store(pos + 1u, std::memory_order_release);
  }

  [[nodiscard]] auto TryPop(SlabId* slab_id) -> bool {
    auto& cell = cells_[head_ & kMask];

    if (cell.sequence.load(std::memory_order_acquire) != head_ + 1u) {
      return false;
    }

    *slab_id = cell.data.load(std::memory_order_relaxed);
    cell.sequence.store(head_ + kMemSlabCount, std::memory_order_release);
    ++head_;
    return true;
  }

 private:
  struct Cell {
    std::atomic<std::uint32_t> sequence;
    std::atomic<SlabId> data;
  };

  static constexpr auto kMask = kMemSlabCount - 1;

  std::array<Cell, kMemSlabCount> cells_;
  alignas(kCacheLineSize) std::atomic<std::uint32_t> tail_{};
  alignas(kCacheLineSize) std::uint32_t head_{};
};
