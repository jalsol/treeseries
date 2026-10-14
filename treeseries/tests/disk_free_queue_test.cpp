#include "treeseries/disk_free_queue.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "treeseries/slab_file.hpp"

namespace {

auto TempPath(std::string_view name) -> std::string {
  return (std::filesystem::temp_directory_path() / name).string();
}

}  // namespace

TEST_CASE("DiskFreeQueue hands out contiguous non-overlapping runs", "[disk_free_queue]") {
  const auto file = SlabFile::Create(TempPath("treeseries_disk_free_queue_runs.bin"));
  DiskFreeQueue queue{file};

  const auto first = queue.TryAllocate(3U);
  REQUIRE(first.has_value());
  REQUIRE(first->first == 0U);
  REQUIRE(first->count == 3U);

  const auto second = queue.TryAllocate(2U);
  REQUIRE(second.has_value());
  REQUIRE(second->first == first->first + first->count);
  REQUIRE(second->count == 2U);

  REQUIRE(queue.Remaining() == kDiskSlabCount - 5U);
}

TEST_CASE("DiskFreeQueue refuses oversized runs without advancing", "[disk_free_queue]") {
  const auto file = SlabFile::Create(TempPath("treeseries_disk_free_queue_oversized.bin"));
  DiskFreeQueue queue{file};

  REQUIRE_FALSE(queue.TryAllocate(0U).has_value());
  REQUIRE_FALSE(queue.TryAllocate(kDiskSlabCount + 1U).has_value());
  REQUIRE(queue.Remaining() == kDiskSlabCount);

  const auto whole = queue.TryAllocate(kDiskSlabCount);
  REQUIRE(whole.has_value());
  REQUIRE(whole->first == 0U);
  REQUIRE(whole->count == kDiskSlabCount);

  REQUIRE(queue.Remaining() == 0U);
  REQUIRE_FALSE(queue.TryAllocate(1U).has_value());
  REQUIRE(queue.Remaining() == 0U);
}

TEST_CASE("DiskFreeQueue allocations map onto writable file space", "[disk_free_queue]") {
  constexpr auto kSlot = 2U;
  const auto path = TempPath("treeseries_disk_free_queue_write.bin");

  {
    auto file = SlabFile::Create(path);
    DiskFreeQueue queue{file};

    const auto run = queue.TryAllocate(kSlot + 1U);
    REQUIRE(run.has_value());
    REQUIRE(run->first == 0U);

    const auto slab = queue.Slab(run->first + kSlot);
    std::ranges::fill(slab, std::byte{0x3C});
    file.Sync(run->first + kSlot, 1U);
  }

  const auto reopened = SlabFile::Open(path);
  REQUIRE(std::to_integer<unsigned>(reopened.Slab(kSlot).front()) == 0x3CU);
}

TEST_CASE("DiskFreeQueue::Restored rejects slots past the end", "[disk_free_queue]") {
  const auto file = SlabFile::Create(TempPath("treeseries_disk_free_queue_restored.bin"));

  auto full = DiskFreeQueue::Restored(file, kDiskSlabCount);
  REQUIRE(full.Remaining() == 0U);
  REQUIRE_FALSE(full.TryAllocate(1U).has_value());

  REQUIRE_THROWS_AS(DiskFreeQueue::Restored(file, kDiskSlabCount + 1U), std::runtime_error);
  REQUIRE_THROWS_AS(DiskFreeQueue::Restored(file, UINT32_MAX), std::runtime_error);

  auto resumed = DiskFreeQueue::Restored(file, 7U);
  REQUIRE(resumed.Remaining() == kDiskSlabCount - 7U);

  const auto run = resumed.TryAllocate(2U);
  REQUIRE(run.has_value());
  REQUIRE(run->first == 7U);
  REQUIRE(run->count == 2U);
}
