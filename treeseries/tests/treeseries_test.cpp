#include "treeseries/treeseries.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <latch>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

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

  REQUIRE_NOTHROW((TreeSeries{&slab_file}));
  REQUIRE_NOTHROW((TreeSeries{&slab_file, 0U}));
}

TEST_CASE("TreeSeries accepts a fully allocated slab file", "[treeseries]") {
  auto slab_file = SlabFile::Create(TempPath("treeseries_engine_full.bin"));

  REQUIRE_NOTHROW((TreeSeries{&slab_file, kDiskSlabCount}));
  REQUIRE_THROWS_AS((TreeSeries{&slab_file, kDiskSlabCount + 1U}), std::runtime_error);
}

TEST_CASE("TreeSeries restores a partially allocated slab file", "[treeseries]") {
  auto slab_file = SlabFile::Create(TempPath("treeseries_engine_restored.bin"));

  REQUIRE_NOTHROW((TreeSeries{&slab_file, 5U}));
}

TEST_CASE("TreeSeries puts items until a slab fills and claims another", "[treeseries]") {
  auto slab_file = SlabFile::Create(TempPath("treeseries_engine_put.bin"));
  auto engine = TreeSeries{&slab_file};
  auto chunk = std::vector<std::byte>(kChunkSize);

  // Every Put hits one MetaHash cell, so the first kItemsPerSlab items land in one
  // slab and the next Put has to claim a second one. Filling past the slab's end
  // would run into the next slab instead of failing, so this is a smoke check.
  for (auto i = 0U; i < kItemsPerSlab + 1U; ++i) {
    REQUIRE_NOTHROW((engine.Put(0U, 0U, chunk)));
  }
}

TEST_CASE("TreeSeries rejects a chunk that is not kChunkSize bytes", "[treeseries]") {
  auto slab_file = SlabFile::Create(TempPath("treeseries_engine_chunk.bin"));
  auto engine = TreeSeries{&slab_file};
  auto short_chunk = std::vector<std::byte>(kChunkSize - 1U);
  auto long_chunk = std::vector<std::byte>(kChunkSize + 1U);

  REQUIRE_THROWS_AS((engine.Put(0U, 0U, short_chunk)), std::runtime_error);
  REQUIRE_THROWS_AS((engine.Put(0U, 0U, long_chunk)), std::runtime_error);
}

TEST_CASE("TreeSeries puts items across many cells, filling each slab", "[treeseries]") {
  constexpr auto kCells = 512U;

  auto slab_file = SlabFile::Create(TempPath("treeseries_engine_many.bin"));
  auto engine = TreeSeries{&slab_file};
  auto chunk = std::vector<std::byte>(kChunkSize);

  // The pairs below hit kCells distinct cells and make the source wrap the column
  // count. kItemsPerSlab + 1 puts per cell fills a slab, hands it off and claims
  // the next one, so this walks the whole state machine kCells times over.
  for (auto cell = 0U; cell < kCells; ++cell) {
    for (auto item = 0U; item < kItemsPerSlab + 1U; ++item) {
      REQUIRE_NOTHROW((engine.Put(MetaHash::kMetaHashCols * 3U + cell,
                                  static_cast<std::uint16_t>(cell), chunk)));
    }
  }

  // Both extremes exercise the modulo arithmetic.
  REQUIRE_NOTHROW((engine.Put(UINT64_MAX, UINT16_MAX, chunk)));
}

TEST_CASE("TreeSeries serialises puts into one cell", "[treeseries]") {
  constexpr auto kWriters = 8U;
  constexpr auto kPerWriter = 64U;

  auto slab_file = SlabFile::Create(TempPath("treeseries_engine_contended.bin"));
  auto engine = TreeSeries{&slab_file};
  auto chunk = std::vector<std::byte>(kChunkSize);
  std::latch start{static_cast<std::ptrdiff_t>(kWriters)};

  // Every writer shares one cell, so all of them run through the same mutex, the
  // same fill counter and the same slab handoff. Without a way to read the slab
  // back there is nothing to assert beyond surviving: no hang, no crash, and a
  // clean run under TSan.
  std::array<std::thread, kWriters> writers;
  for (auto i = 0U; i < kWriters; ++i) {
    writers[i] = std::thread([&engine, &chunk, &start] {
      start.arrive_and_wait();
      for (auto j = 0U; j < kPerWriter; ++j) {
        engine.Put(0U, 0U, chunk);
      }
    });
  }

  for (auto& writer : writers) {
    writer.join();
  }
}

// The engine has no way to read a slab back until Get exists, so the byte-level
// checks go through TreeSeries' test seam.
struct TreeSeriesTestAccess {
  [[nodiscard]] static auto Cell(const TreeSeries& engine, std::uint16_t metric_id,
                                 std::uint64_t source_id) -> const MetaHash::Item& {
    return engine.meta_hash_->GetItemFor(metric_id, source_id);
  }

  [[nodiscard]] static auto SlabBytes(const TreeSeries& engine, SlabId slab_id)
      -> std::span<std::byte, kSlabSize> {
    return engine.SlabBytes(slab_id);
  }

  [[nodiscard]] static auto TryPopFull(TreeSeries& engine, SlabId* slab_id) -> bool {
    return engine.mem_full_queue_->TryPop(slab_id);
  }

  [[nodiscard]] static auto FreeRemaining(const TreeSeries& engine) -> std::uint32_t {
    return engine.mem_free_queue_.Remaining();
  }
};

TEST_CASE("TreeSeries writes the item's bytes into the slab", "[treeseries]") {
  constexpr auto kSourceId = 0x1122334455667788ULL;
  constexpr auto kMetricId = static_cast<std::uint16_t>(0xABCDU);

  auto slab_file = SlabFile::Create(TempPath("treeseries_engine_bytes.bin"));
  auto engine = TreeSeries{&slab_file};
  auto chunk = std::vector<std::byte>(kChunkSize);
  for (auto i = 0U; i < kChunkSize; ++i) {
    chunk[i] = static_cast<std::byte>(i);
  }

  engine.Put(kSourceId, kMetricId, chunk);

  const auto& cell = TreeSeriesTestAccess::Cell(engine, kMetricId, kSourceId);
  REQUIRE(cell.used == 1U);

  const auto slab = TreeSeriesTestAccess::SlabBytes(engine, cell.slab_id);
  auto source_id = std::uint64_t{};
  auto metric_id = std::uint16_t{};
  std::memcpy(&source_id, slab.data(), kSourceIdSize);
  std::memcpy(&metric_id, slab.data() + kSourceIdSize, kMetricIdSize);

  REQUIRE(source_id == kSourceId);
  REQUIRE(metric_id == kMetricId);
  REQUIRE(std::memcmp(slab.data() + kSourceIdSize + kMetricIdSize, chunk.data(), kChunkSize) == 0);
}

TEST_CASE("TreeSeries hands a slab over at exactly kItemsPerSlab items", "[treeseries]") {
  auto slab_file = SlabFile::Create(TempPath("treeseries_engine_boundary.bin"));
  auto engine = TreeSeries{&slab_file};
  auto chunk = std::vector<std::byte>(kChunkSize);
  auto slab_id = SlabId::kInvalid;

  for (auto i = 0U; i < kItemsPerSlab - 1U; ++i) {
    engine.Put(0U, 0U, chunk);
  }

  // One item short: the cell still holds the slab, and nothing was handed off.
  const auto& cell = TreeSeriesTestAccess::Cell(engine, 0U, 0U);
  REQUIRE(cell.slab_id != SlabId::kInvalid);
  REQUIRE(cell.used == kItemsPerSlab - 1U);
  REQUIRE_FALSE(TreeSeriesTestAccess::TryPopFull(engine, &slab_id));

  const auto open_slab = cell.slab_id;
  engine.Put(0U, 0U, chunk);

  // The kItemsPerSlab-th item fills it: the slab enters MemFullQueue and the cell
  // is cleared in the same breath.
  REQUIRE(cell.slab_id == SlabId::kInvalid);
  REQUIRE(TreeSeriesTestAccess::TryPopFull(engine, &slab_id));
  REQUIRE(slab_id == open_slab);
}

TEST_CASE("TreeSeries claims a fresh slab after handing one over", "[treeseries]") {
  auto slab_file = SlabFile::Create(TempPath("treeseries_engine_fresh.bin"));
  auto engine = TreeSeries{&slab_file};
  auto chunk = std::vector<std::byte>(kChunkSize);

  for (auto i = 0U; i < kItemsPerSlab; ++i) {
    engine.Put(0U, 0U, chunk);
  }

  auto handed = SlabId::kInvalid;
  REQUIRE(TreeSeriesTestAccess::TryPopFull(engine, &handed));

  engine.Put(0U, 0U, chunk);

  const auto& cell = TreeSeriesTestAccess::Cell(engine, 0U, 0U);
  REQUIRE(cell.slab_id != SlabId::kInvalid);
  REQUIRE(cell.slab_id != handed);
  REQUIRE(cell.used == 1U);
}

TEST_CASE("TreeSeries keeps every handed-out slab accounted for", "[treeseries]") {
  constexpr auto kCells = 64U;

  auto slab_file = SlabFile::Create(TempPath("treeseries_engine_accounting.bin"));
  auto engine = TreeSeries{&slab_file};
  auto chunk = std::vector<std::byte>(kChunkSize);

  for (auto cell = 0U; cell < kCells; ++cell) {
    for (auto item = 0U; item < kItemsPerSlab; ++item) {
      engine.Put(cell, 0U, chunk);
    }
  }

  // Every cell filled exactly one slab, so the free queue is short by that many
  // and MemFullQueue holds that many, all distinct: a slab that went missing or
  // entered twice shows up as a count mismatch or a duplicate here.
  auto full = std::vector<std::uint32_t>{};
  auto slab_id = SlabId::kInvalid;
  while (TreeSeriesTestAccess::TryPopFull(engine, &slab_id)) {
    full.push_back(slab_id);
  }

  REQUIRE(full.size() == kCells);
  REQUIRE(kMemSlabCount - TreeSeriesTestAccess::FreeRemaining(engine) == kCells);

  std::ranges::sort(full);
  REQUIRE(std::ranges::equal(full, std::views::iota(0U, kCells)));
}
