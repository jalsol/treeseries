#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "treeseries/common.hpp"

enum SlabId : std::uint32_t { kInvalid = UINT32_MAX };

using Chunk = std::array<std::byte, kChunkSize>;

struct Item {
  std::uint64_t source_id{};
  std::uint16_t metric_id{};
  Chunk chunk;
};

static_assert(sizeof(Item) == kItemSize, "Item size mismatched");

static_assert(offsetof(Item, metric_id) == kSourceIdSize, "Item layout mismatched");
static_assert(offsetof(Item, chunk) == kSourceIdSize + kMetricIdSize, "Item layout mismatched");

struct Slab {
  std::array<Item, kItemsPerSlab> items;
};

static_assert(sizeof(Slab) == kSlabSize, "Slab size mismatched");
