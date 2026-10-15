#pragma once

#include <cstdint>

using Timestamp = std::uint64_t;

constexpr auto kSlabSize = 4096uz;
constexpr auto kItemSize = 256uz;
constexpr auto kMetricIdSize = 2uz;
constexpr auto kSourceIdSize = 8uz;
constexpr auto kChunkSize = kItemSize - kSourceIdSize - kMetricIdSize;
constexpr auto kItemsPerSlab = kSlabSize / kItemSize;

static_assert(kSlabSize == kItemsPerSlab * kItemSize, "Slab size isn't a whole number of items");

constexpr auto GB = 1024uz * 1024 * 1024;
constexpr auto kSlabFileSize = 6 * GB;

// A server's slab buffer, and a test build's: every engine maps and prefaults
// the whole thing, so a suite that makes a dozen of them wants it small. The
// tests' .bazelrc line defines TREESERIES_TEST_BUILD for their sources.
#ifdef TREESERIES_TEST_BUILD
constexpr auto kSlabBufferSize = 16uz * 1024 * 1024;
#else
constexpr auto kSlabBufferSize = 2uz * GB;
#endif

static_assert(kSlabFileSize == 6uz * GB, "Slab file must be 6 GiB");
static_assert(kSlabBufferSize % kSlabSize == 0, "Slab buffer isn't divisible by slab size");

// Width uint32_t => SlabId arithmetic never mixes with size_t.
constexpr auto kMemSlabCount = static_cast<std::uint32_t>(kSlabBufferSize / kSlabSize);
constexpr auto kDiskSlabCount = static_cast<std::uint32_t>(kSlabFileSize / kSlabSize);

static_assert(kSlabFileSize % kSlabSize == 0, "Slab file isn't divisible by slab size");
static_assert(kMemSlabCount > 0 && kDiskSlabCount > 0);
static_assert(kMemSlabCount <= UINT32_MAX && kDiskSlabCount <= UINT32_MAX, "SlabId must be 32-bit");

constexpr auto kCacheLineSize = 64;
