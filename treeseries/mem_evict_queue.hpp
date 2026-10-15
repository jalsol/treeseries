#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "treeseries/slab.hpp"

class MemEvictQueue {
  static_assert(std::has_single_bit(kMemSlabCount), "Mem slab count isn't power of 2");

 public:
  MemEvictQueue() {
    for (auto pos = 0u; pos < kMemSlabCount; ++pos) {
      cells_[pos].sequence.store(pos, std::memory_order_relaxed);
    }
  }

  void Push(SlabId slab_id) {
    auto& cell = cells_[tail_ & kMask];

    if (cell.sequence.load(std::memory_order_acquire) != tail_) {
      std::fprintf(stderr,
                   "MemEvictQueue: push at position %u laps the consumers; a slab is in "
                   "two queues or was never taken from one\n",
                   static_cast<unsigned>(tail_));
      std::abort();
    }

    cell.data.store(slab_id, std::memory_order_relaxed);
    ++tail_;
    cell.sequence.store(tail_, std::memory_order_release);
  }

  [[nodiscard]] auto TryPop(SlabId* slab_id) -> bool {
    auto pos = head_.load(std::memory_order_relaxed);

    while (true) {
      auto& cell = cells_[pos & kMask];
      if (cell.sequence.load(std::memory_order_acquire) != pos + 1u) {
        return false;
      }

      const auto slab = cell.data.load(std::memory_order_relaxed);

      if (!head_.compare_exchange_weak(pos, pos + 1u, std::memory_order_relaxed)) {
        continue;
      }

      cell.sequence.store(pos + kMemSlabCount, std::memory_order_release);
      *slab_id = slab;
      return true;
    }
  }

 private:
  struct Cell {
    std::atomic<std::uint32_t> sequence;
    std::atomic<SlabId> data;
  };

  static constexpr auto kMask = kMemSlabCount - 1;

  std::array<Cell, kMemSlabCount> cells_;
  alignas(kCacheLineSize) std::uint32_t tail_{};
  alignas(kCacheLineSize) std::atomic<std::uint32_t> head_{};
};
