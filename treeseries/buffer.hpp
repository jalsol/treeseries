#pragma once

#include <sys/mman.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "treeseries/common.hpp"

class SlabBuffer {
 public:
  SlabBuffer()
      : buffer_{::mmap(nullptr, kSlabBufferSize, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_POPULATE, -1, 0)} {
    if (buffer_ == MAP_FAILED) {
      throw std::runtime_error("SlabBuffer: Cannot mmap.");
    }
  }

  SlabBuffer(const SlabBuffer&) = delete;
  SlabBuffer& operator=(const SlabBuffer&) = delete;

  ~SlabBuffer() {
    if (::munmap(buffer_, kSlabBufferSize) == -1) {
      std::fprintf(stderr, "SlabBuffer: Cannot munmap: %s\n", std::strerror(errno));
    }
  }

  void Read();
  void Write();

 private:
  void* buffer_;
};
