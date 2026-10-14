#pragma once

#include <cstdint>

constexpr auto kSlabSize = 4096uz;
constexpr auto kItemSize = 256uz;
constexpr auto kMetricIdSize = 2uz;
constexpr auto kSourceIdSize = 8uz;
constexpr auto kChunkSize = kItemSize - kSourceIdSize - kMetricIdSize;
constexpr auto kItemsPerSlab = kSlabSize / kItemSize;

static_assert(kSlabSize == kItemsPerSlab * kItemSize, "Slab size isn't a whole number of items");

constexpr auto GB = 1024uz * 1024 * 1024;
constexpr auto kSlabBufferSize = 2 * GB;
constexpr auto kSlabFileSize = 6 * GB;
static_assert(kSlabBufferSize == 2uz * GB, "Slab buffer must be 2 GiB");
static_assert(kSlabFileSize == 6uz * GB, "Slab file must be 6 GiB");

// Width uint32_t => SlabId arithmetic never mixes with size_t.
constexpr auto kMemSlabCount = static_cast<std::uint32_t>(kSlabBufferSize / kSlabSize);
constexpr auto kDiskSlabCount = static_cast<std::uint32_t>(kSlabFileSize / kSlabSize);

static_assert(kSlabFileSize % kSlabSize == 0, "Slab file isn't divisible by slab size");
static_assert(kMemSlabCount > 0 && kDiskSlabCount > 0);
static_assert(kMemSlabCount <= UINT32_MAX && kDiskSlabCount <= UINT32_MAX, "SlabId must be 32-bit");

constexpr auto kCacheLineSize = 64;
