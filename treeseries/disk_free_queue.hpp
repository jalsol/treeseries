#pragma once

#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <stdexcept>

#include "treeseries/common.hpp"
#include "treeseries/slab_file.hpp"

struct DiskRun {
  std::uint32_t first;
  std::uint32_t count;
};

class DiskFreeQueue {
 public:
  explicit DiskFreeQueue(const SlabFile& file) : file_{&file} {}

  [[nodiscard]] auto TryAllocate(std::uint32_t count) -> std::optional<DiskRun> {
    if (count == 0 || count > kDiskSlabCount - next_) {
      return std::nullopt;
    }

    const DiskRun run{.first = next_, .count = count};
    next_ += count;
    return run;
  }

  [[nodiscard]] auto Slab(std::uint32_t first) const -> std::span<std::byte, kSlabSize> {
    return file_->Slab(first);
  }

  [[nodiscard]] auto Remaining() const -> std::uint32_t { return kDiskSlabCount - next_; }

  [[nodiscard]] static auto Restored(const SlabFile& file, std::uint32_t first_free)
      -> DiskFreeQueue {
    if (first_free > kDiskSlabCount) {
      throw std::runtime_error(
          std::format("DiskFreeQueue: disk slab {} is past the end of the slab file", first_free));
    }

    return DiskFreeQueue{file, first_free};
  }

 private:
  DiskFreeQueue(const SlabFile& file, std::uint32_t next) : file_{&file}, next_{next} {}

  const SlabFile* file_;
  std::uint32_t next_ = 0;
};
