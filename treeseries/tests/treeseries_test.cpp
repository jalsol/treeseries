#include "treeseries/treeseries.hpp"

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

#include "treeseries/slab_file.hpp"

namespace {

auto TempPath(std::string_view name) -> std::string {
  return (std::filesystem::temp_directory_path() / name).string();
}

}  // namespace

static_assert(!std::is_copy_constructible_v<TreeSeries>);
static_assert(!std::is_move_constructible_v<TreeSeries>);

TEST_CASE("TreeSeries constructs over a fresh slab file", "[treeseries]") {
  auto slab_file = SlabFile::Create(TempPath("treeseries_engine_fresh.bin"));

  REQUIRE_NOTHROW((TreeSeries{slab_file}));
  REQUIRE_NOTHROW((TreeSeries{slab_file, 0U}));
}

TEST_CASE("TreeSeries accepts a fully allocated slab file", "[treeseries]") {
  auto slab_file = SlabFile::Create(TempPath("treeseries_engine_full.bin"));

  REQUIRE_NOTHROW((TreeSeries{slab_file, kDiskSlabCount}));
  REQUIRE_THROWS_AS((TreeSeries{slab_file, kDiskSlabCount + 1U}), std::runtime_error);
}

TEST_CASE("TreeSeries restores a partially allocated slab file", "[treeseries]") {
  auto slab_file = SlabFile::Create(TempPath("treeseries_engine_restored.bin"));

  REQUIRE_NOTHROW((TreeSeries{slab_file, 5U}));
}
