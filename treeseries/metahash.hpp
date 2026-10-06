#pragma once

#include <array>
#include <mutex>

#include "treeseries/slab.hpp"

struct MetaHash {
  static constexpr auto kMetaHashRows = 100uz;
  static constexpr auto kMetaHashCols = 10000uz;

  struct Item {
    mutable std::mutex mutex;
    SlabId slab_id = SlabId::kInvalid;
  };

  Item& Get(auto i, auto j) { return table[i][j]; }

  std::array<std::array<Item, kMetaHashCols>, kMetaHashRows> table;
};
