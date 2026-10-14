#pragma once

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <span>
#include <stdexcept>
#include <string>

#include "treeseries/common.hpp"

class SlabFile {
 public:
  [[nodiscard]] static auto Create(const std::string& path) -> SlabFile {
    const auto fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd == -1) {
      throw std::runtime_error(
          std::format("SlabFile: Cannot open {}: {}", path, std::strerror(errno)));
    }

    if (::ftruncate(fd, static_cast<off_t>(kSlabFileSize)) == -1) {
      const auto error = errno;
      ::close(fd);
      throw std::runtime_error(
          std::format("SlabFile: Cannot truncate {}: {}", path, std::strerror(error)));
    }

    return Map(fd, path);
  }

  [[nodiscard]] static auto Open(const std::string& path) -> SlabFile {
    const auto fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd == -1) {
      throw std::runtime_error(
          std::format("SlabFile: Cannot open {}: {}", path, std::strerror(errno)));
    }

    struct stat info{};
    if (::fstat(fd, &info) == -1) {
      const auto error = errno;
      ::close(fd);
      throw std::runtime_error(
          std::format("SlabFile: Cannot stat {}: {}", path, std::strerror(error)));
    }

    if (info.st_size != static_cast<off_t>(kSlabFileSize)) {
      ::close(fd);
      throw std::runtime_error(std::format("SlabFile: {} is not {} bytes", path, kSlabFileSize));
    }

    return Map(fd, path);
  }

  SlabFile(const SlabFile&) = delete;
  SlabFile& operator=(const SlabFile&) = delete;

  ~SlabFile() {
    if (::munmap(mapping_.data(), mapping_.size()) == -1) {
      std::fprintf(stderr, "SlabFile: Cannot munmap: %s\n", std::strerror(errno));
    }
    if (fd_ != -1) {
      ::close(fd_);
    }
  }

  [[nodiscard]] auto Slab(std::uint32_t id) const -> std::span<std::byte, kSlabSize> {
    return std::span<std::byte, kSlabSize>{mapping_.subspan(id * kSlabSize, kSlabSize)};
  }

  void Sync(std::uint32_t first, std::uint32_t count) const {
    if (count == 0) {
      return;
    }

    const auto run = mapping_.subspan(first * kSlabSize, count * kSlabSize);
    if (::msync(run.data(), run.size(), MS_SYNC) == -1) {
      throw std::runtime_error(std::format("SlabFile: Cannot msync: {}", std::strerror(errno)));
    }
  }

 private:
  SlabFile(int fd, std::byte* mapping) : fd_{fd}, mapping_{mapping, kSlabFileSize} {}

  [[nodiscard]] static auto Map(int fd, const std::string& path) -> SlabFile {
    auto* mapping = static_cast<std::byte*>(
        ::mmap(nullptr, kSlabFileSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    if (mapping == MAP_FAILED) {
      const auto error = errno;
      ::close(fd);
      throw std::runtime_error(
          std::format("SlabFile: Cannot mmap {}: {}", path, std::strerror(error)));
    }

    return SlabFile{fd, mapping};
  }

  int fd_ = -1;
  std::span<std::byte, kSlabFileSize> mapping_;
};
