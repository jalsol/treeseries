#pragma once

#include <algorithm>
#include <atomic>
#include <optional>
#include <utility>

#include "treeseries/common.hpp"
#include "treeseries/slab.hpp"

class MemFreeQueue {
 public:
  [[nodiscard]] auto TryPop() -> std::optional<SlabId> {
    if (exhausted_.load(std::memory_order_relaxed)) {
      return std::nullopt;
    }

    const auto id = next_.fetch_add(1, std::memory_order_relaxed);
    if (id < kMemSlabCount) {
      return SlabId{id};
    }

    exhausted_.store(true, std::memory_order_relaxed);
    return std::nullopt;
  }

  [[nodiscard]] auto TryPopBlock(std::uint32_t quantity)
      -> std::optional<std::pair<SlabId, std::uint32_t>> {
    if (quantity == 0) {
      return std::nullopt;
    }

    auto current = next_.load(std::memory_order_relaxed);
    while (current < kMemSlabCount) {
      const auto count = std::min(quantity, kMemSlabCount - current);
      if (next_.compare_exchange_weak(current, current + count, std::memory_order_relaxed)) {
        return std::pair{SlabId{current}, count};
      }
    }

    exhausted_.store(true, std::memory_order_relaxed);
    return std::nullopt;
  }

  [[nodiscard]] auto Remaining() const -> std::uint32_t {
    const auto used = next_.load(std::memory_order_relaxed);
    return used >= kMemSlabCount ? 0 : kMemSlabCount - used;
  }

 private:
  alignas(kCacheLineSize) std::atomic<std::uint32_t> next_{};
  alignas(kCacheLineSize) std::atomic<bool> exhausted_{};
};
