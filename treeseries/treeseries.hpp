#pragma once

#include <sys/mman.h>

#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <vector>

#include "treeseries/common.hpp"
#include "treeseries/metahash.hpp"

using timestamp = std::uint64_t;

class TreeSeries {
 public:
  TreeSeries()
      : slab_buffer_{static_cast<std::byte*>(::mmap(nullptr, kSlabBufferSize, PROT_READ | PROT_WRITE,
                                                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_POPULATE, -1, 0))} {
    if (slab_buffer_ == MAP_FAILED) {
      throw std::runtime_error("TreeSeries: Cannot mmap.");
    }
  }

  TreeSeries(const TreeSeries&) = delete;
  TreeSeries& operator=(const TreeSeries&) = delete;

  ~TreeSeries() {
    if (::munmap(slab_buffer_, kSlabBufferSize) == -1) {
      std::fprintf(stderr, "TreeSeries: Cannot munmap: %s\n", std::strerror(errno));
    }
  }

  auto Put(std::uint64_t, std::uint16_t metric_id, std::span<std::byte>) {
    const auto i = metric_id % MetaHash::kMetaHashRows;
    const auto j = metric_id % MetaHash::kMetaHashCols;
    auto& item = meta_hash_.Get(i, j);

    std::scoped_lock lock(item.mutex);
    if (item.slab_id == SlabId::kInvalid) {
      // TODO
    }

    // TODO:
  }

  auto Get(std::uint64_t, std::uint16_t, timestamp, timestamp) -> std::vector<Chunk> {
    // TODO
    return {};
  }

 private:
  std::byte* slab_buffer_;
  MetaHash meta_hash_;
};
