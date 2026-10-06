#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "treeseries/common.hpp"

enum SlabId : int64_t { kInvalid = -1 };

using Chunk = std::array<std::byte, kChunkSize>;

struct Item {
  std::uint64_t source_id;
  std::uint16_t metric_id;
  Chunk chunk;
};

static_assert(sizeof(Item) == kItemSize, "Item size mismatched");

struct Slab {
  Item items[kSlabSize / kItemSize];
};

static_assert(sizeof(Slab) == kSlabSize, "Slab size mismatched");
