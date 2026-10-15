#pragma once

#include <sys/mman.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

#include "treeseries/common.hpp"
#include "treeseries/disk_free_queue.hpp"
#include "treeseries/mem_evict_queue.hpp"
#include "treeseries/mem_free_queue.hpp"
#include "treeseries/mem_full_queue.hpp"
#include "treeseries/metahash.hpp"
#include "treeseries/slab_file.hpp"

using timestamp = std::uint64_t;

class TreeSeries {
 public:
  explicit TreeSeries(SlabFile* slab_file) : TreeSeries{slab_file, 0} {}

  TreeSeries(SlabFile* slab_file, std::uint32_t first_free)
      : slab_buffer_{MapBuffer()},
        slab_file_{*slab_file},
        disk_free_queue_{DiskFreeQueue::Restored(*slab_file, first_free)},
        mem_full_queue_{std::make_unique<MemFullQueue>()},
        mem_evict_queue_{std::make_unique<MemEvictQueue>()},
        meta_hash_{std::make_unique<MetaHash>()} {}

  TreeSeries(const TreeSeries&) = delete;
  TreeSeries& operator=(const TreeSeries&) = delete;

  auto Put(std::uint64_t source_id, std::uint16_t metric_id, std::span<std::byte>) {
    auto& item = meta_hash_->GetItemFor(metric_id, source_id);
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
  struct MappingDeleter {
    void operator()(std::byte* mapping) const {
      if (mapping != nullptr && ::munmap(mapping, kSlabBufferSize) == -1) {
        std::fprintf(stderr, "TreeSeries: Cannot munmap: %s\n", std::strerror(errno));
      }
    }
  };

  [[nodiscard]] static auto MapBuffer() -> std::unique_ptr<std::byte, MappingDeleter> {
    constexpr auto kFlags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE;

    auto* mapping = static_cast<std::byte*>(
        ::mmap(nullptr, kSlabBufferSize, PROT_READ | PROT_WRITE, kFlags | MAP_HUGETLB, -1, 0));
    if (mapping == MAP_FAILED) {
      mapping = static_cast<std::byte*>(
          ::mmap(nullptr, kSlabBufferSize, PROT_READ | PROT_WRITE, kFlags, -1, 0));
    }
    if (mapping == MAP_FAILED) {
      throw std::runtime_error(std::format("TreeSeries: Cannot mmap: {}", std::strerror(errno)));
    }

    return std::unique_ptr<std::byte, MappingDeleter>{mapping};
  }

  std::unique_ptr<std::byte, MappingDeleter> slab_buffer_;
  SlabFile& slab_file_;

  // NOTE: I'm not supporting HDD, thus DiskFullQueue and DiskEvictQueue should be eliminated.
  DiskFreeQueue disk_free_queue_;
  MemFreeQueue mem_free_queue_;
  std::unique_ptr<MemFullQueue> mem_full_queue_;
  std::unique_ptr<MemEvictQueue> mem_evict_queue_;

  std::unique_ptr<MetaHash> meta_hash_;
};
