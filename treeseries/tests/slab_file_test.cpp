#include "treeseries/slab_file.hpp"

#include <algorithm>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace {

auto TempPath(std::string_view name) -> std::string {
  return (std::filesystem::temp_directory_path() / name).string();
}

}  // namespace

TEST_CASE("SlabFile Create sizes and maps the whole slab file", "[slab_file]") {
  const auto path = TempPath("treeseries_slab_file_create.bin");
  const auto file = SlabFile::Create(path);

  REQUIRE(std::filesystem::file_size(path) == kSlabFileSize);

  const auto first = file.Slab(0);
  REQUIRE(first.size() == kSlabSize);

  const auto last = file.Slab(kDiskSlabCount - 1);
  REQUIRE(last.size() == kSlabSize);
  REQUIRE(std::to_integer<std::uintptr_t>(last[0]) == 0U);
}

TEST_CASE("SlabFile survives a reopen with contents intact", "[slab_file]") {
  const auto path = TempPath("treeseries_slab_file_reopen.bin");
  constexpr auto kSlot = kDiskSlabCount - 3;

  {
    auto file = SlabFile::Create(path);
    const auto slab = file.Slab(kSlot);
    std::ranges::fill(slab, std::byte{0x5A});
    file.Sync(kSlot, 1U);
  }

  const auto reopened = SlabFile::Open(path);
  const auto slab = reopened.Slab(kSlot);
  REQUIRE(std::to_integer<unsigned>(slab.front()) == 0x5AU);
  REQUIRE(std::to_integer<unsigned>(slab.back()) == 0x5AU);
}

TEST_CASE("SlabFile Open rejects a wrong-sized store", "[slab_file]") {
  const auto path = TempPath("treeseries_slab_file_short.bin");
  {
    std::ofstream out{path};
    out << "short";
  }

  REQUIRE_THROWS(SlabFile::Open(path));
}

TEST_CASE("SlabFile Open rejects a missing store", "[slab_file]") {
  const auto path = TempPath("treeseries_slab_file_missing.bin");
  std::filesystem::remove(path);

  REQUIRE_THROWS(SlabFile::Open(path));
}
