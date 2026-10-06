#pragma once

constexpr auto kSlabSize = 4096uz;
constexpr auto kItemSize = 256uz;
constexpr auto kMetricIdSize = 2uz;
constexpr auto kSourceIdSize = 8uz;
constexpr auto kChunkSize = kItemSize - kSourceIdSize - kMetricIdSize;

static_assert(kSlabSize % kItemSize == 0, "Slab size isn't divisible by item size");

constexpr auto GB = 1024uz * 1024 * 1024;
constexpr auto kSlabBufferSize = 2 * GB;
constexpr auto kSlabFileSize = 6 * GB;
