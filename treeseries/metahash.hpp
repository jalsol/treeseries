#pragma once

#include <array>
#include <cstdint>
#include <mutex>

#include "treeseries/common.hpp"
#include "treeseries/slab.hpp"

struct IndexEntry;

struct MetaHash {
  static constexpr auto kMetaHashRows = 1uz << 5;
  static constexpr auto kMetaHashCols = 1uz << 11;

  struct Item {
    mutable std::mutex mutex;
    SlabId slab_id = SlabId::kInvalid;
    std::uint32_t used = 0;

    // The tree index entry each written slot belongs to, so a series' group in
    // the slab being filled can be found without touching the tree.
    std::array<IndexEntry*, kItemsPerSlab> entries{};
  };

  Item& GetItemFor(auto metric_id, auto source_id) {
    return table[metric_id % kMetaHashRows][source_id % kMetaHashCols];
  }

  std::array<std::array<Item, kMetaHashCols>, kMetaHashRows> table;
};
