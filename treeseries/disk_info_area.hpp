#pragma once

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <span>
#include <stdexcept>
#include <string>

#include "treeseries/common.hpp"

constexpr auto kDiskInfoMagic = std::uint32_t{0x49445354};  // "TSDI", little-endian

// What one (slab, series) group looks like once its slab is on disk. Native
// endianness and layout: this area is a same-machine artifact, like the slab
// file itself.
struct DiskInfoEntry {
  // Ordered so the record is exactly 32 bytes with no interior padding.
  std::uint64_t source_id;
  std::uint64_t min_start;
  std::uint64_t max_start;
  std::uint16_t metric_id;
  std::uint16_t slots;
  std::uint16_t reserved;
};

static_assert(sizeof(DiskInfoEntry) == 32, "Disk info entry layout mismatched");

struct DiskInfoHeader {
  std::uint32_t magic;
  std::uint32_t disk_id;
  std::uint16_t count;
  std::uint16_t reserved;
};

static_assert(sizeof(DiskInfoHeader) == 12, "Disk info header layout mismatched");

// §3.3.3's disk slabinfo area: one record per slab holding the index entries
// that slab needs for recovery. A record is appended only once the slab's
// bytes are durable, so replaying the intact records rebuilds exactly the part
// of the index that had reached the slab file.
class DiskInfoArea {
 public:
  explicit DiskInfoArea(const std::string& path) {
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd_ == -1) {
      throw std::runtime_error(
          std::format("DiskInfoArea: Cannot open {}: {}", path, std::strerror(errno)));
    }
  }

  DiskInfoArea(const DiskInfoArea&) = delete;
  DiskInfoArea& operator=(const DiskInfoArea&) = delete;

  ~DiskInfoArea() {
    if (fd_ != -1) {
      ::close(fd_);
    }
  }

  // Hands every intact record to `consume(disk_id, entries)` in file order and
  // drops a torn tail left by a crash mid-append, so appends stay in sync.
  // Returns the first free disk slab id.
  template <typename Fn>
  auto Replay(const Fn& consume) -> std::uint32_t {
    auto header = DiskInfoHeader{};
    auto entries = std::array<DiskInfoEntry, kItemsPerSlab>{};
    auto offset = off_t{0};
    auto first_free = std::uint32_t{0};

    while (true) {
      if (!ReadExact(offset, &header, sizeof(header)) || header.magic != kDiskInfoMagic ||
          header.count > kItemsPerSlab || header.disk_id >= kDiskSlabCount) {
        break;
      }

      const auto bytes = std::size_t{header.count} * sizeof(DiskInfoEntry);
      if (!ReadExact(offset + static_cast<off_t>(sizeof(header)), entries.data(), bytes)) {
        break;
      }

      consume(header.disk_id, std::span{entries}.first(header.count));
      first_free = std::max(first_free, header.disk_id + 1);
      offset += static_cast<off_t>(sizeof(header) + bytes);
    }

    if (::ftruncate(fd_, offset) == -1) {
      throw std::runtime_error(
          std::format("DiskInfoArea: Cannot truncate: {}", std::strerror(errno)));
    }
    if (::lseek(fd_, offset, SEEK_SET) == -1) {
      throw std::runtime_error(std::format("DiskInfoArea: Cannot seek: {}", std::strerror(errno)));
    }

    return first_free;
  }

  void Append(std::uint32_t disk_id, std::span<const DiskInfoEntry> entries) {
    const auto header = DiskInfoHeader{.magic = kDiskInfoMagic,
                                       .disk_id = disk_id,
                                       .count = static_cast<std::uint16_t>(entries.size()),
                                       .reserved = 0};
    WriteAll(&header, sizeof(header));
    WriteAll(entries.data(), entries.size_bytes());
  }

  // The batch's records reach the disk together, after their slabs did.
  void Sync() {
    if (::fdatasync(fd_) == -1) {
      throw std::runtime_error(std::format("DiskInfoArea: Cannot sync: {}", std::strerror(errno)));
    }
  }

 private:
  [[nodiscard]] auto ReadExact(off_t offset, void* buffer, std::size_t bytes) const -> bool {
    auto* target = static_cast<std::byte*>(buffer);

    while (bytes != 0) {
      const auto got = ::pread(fd_, target, bytes, offset);
      if (got <= 0) {
        return false;
      }

      target += got;
      offset += got;
      bytes -= static_cast<std::size_t>(got);
    }

    return true;
  }

  void WriteAll(const void* buffer, std::size_t bytes) {
    const auto* source = static_cast<const std::byte*>(buffer);

    while (bytes != 0) {
      const auto put = ::write(fd_, source, bytes);
      if (put <= 0) {
        throw std::runtime_error(
            std::format("DiskInfoArea: Cannot append: {}", std::strerror(errno)));
      }

      source += put;
      bytes -= static_cast<std::size_t>(put);
    }
  }

  int fd_ = -1;
};
