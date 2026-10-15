#include "treeseries/treeseries.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
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
#include "treeseries/tree_index.hpp"

namespace {

// Each engine gets a slab file and a disk info area of its own. Constructing
// one maps and prefaults the whole slab buffer, so the tests are long on
// purpose: a handful of engines, each doing plenty.
struct EngineFiles {
  explicit EngineFiles(std::string_view name)
      : slab_path{(std::filesystem::temp_directory_path() / (std::string{name} + ".bin")).string()},
        info_path{
            (std::filesystem::temp_directory_path() / (std::string{name} + ".info")).string()} {
    // Leftovers from an earlier run are not "recovery": the slab file gets
    // truncated, but the disk info area would be replayed into the new engine.
    // Bazel's sandbox hands out a fresh /tmp; a plain run of the binary does not.
    std::filesystem::remove(slab_path);
    std::filesystem::remove(info_path);
  }

  std::string slab_path;
  std::string info_path;
};

// Chunks are opaque to the engine; the tests give them a start time in the
// first 8 bytes (as Fig. 9 does) and a marker byte to tell them apart. Put
// takes a span, so a chunk needs somewhere to live before it goes in.
auto MakeChunk(Timestamp start, std::uint8_t marker = 0) -> std::vector<std::byte> {
  auto chunk = std::vector<std::byte>(kChunkSize);
  std::memcpy(chunk.data(), &start, sizeof(start));
  chunk[8] = static_cast<std::byte>(marker);
  return chunk;
}

void PutChunk(TreeSeries& engine, std::uint64_t source_id, std::uint16_t metric_id, Timestamp start,
              std::uint8_t marker = 0) {
  auto chunk = MakeChunk(start, marker);
  engine.Put(source_id, metric_id, chunk);
}

auto ChunkStart(const Chunk& chunk) -> Timestamp {
  auto start = Timestamp{};
  std::memcpy(&start, chunk.data(), sizeof(start));
  return start;
}

auto ChunkMarker(const Chunk& chunk) -> std::uint8_t {
  return std::to_integer<std::uint8_t>(chunk[8]);
}

auto StartsOf(const std::vector<Chunk>& chunks) -> std::vector<Timestamp> {
  auto starts = std::vector<Timestamp>{};
  starts.reserve(chunks.size());
  std::ranges::transform(chunks, std::back_inserter(starts), ChunkStart);
  return starts;
}

}  // namespace

static_assert(!std::is_copy_constructible_v<TreeSeries>);
static_assert(!std::is_move_constructible_v<TreeSeries>);

TEST_CASE("TreeSeries constructs over a fresh slab file", "[treeseries]") {
  auto files = EngineFiles{"treeseries_engine_fresh"};
  auto slab_file = SlabFile::Create(files.slab_path);

  REQUIRE_NOTHROW((TreeSeries{&slab_file, files.info_path}));
}

TEST_CASE("TreeSeries puts items until a slab fills and claims another", "[treeseries]") {
  auto files = EngineFiles{"treeseries_engine_put"};
  auto slab_file = SlabFile::Create(files.slab_path);
  auto engine = TreeSeries{&slab_file, files.info_path};

  // Every Put hits one MetaHash cell, so the first kItemsPerSlab items land in one
  // slab and the next Put has to claim a second one. Filling past the slab's end
  // would run into the next slab instead of failing, so this is a smoke check.
  for (auto i = 0U; i < kItemsPerSlab + 1U; ++i) {
    REQUIRE_NOTHROW(PutChunk(engine, 0U, 0U, i));
  }
}

TEST_CASE("TreeSeries rejects a chunk that is not kChunkSize bytes", "[treeseries]") {
  auto files = EngineFiles{"treeseries_engine_chunk"};
  auto slab_file = SlabFile::Create(files.slab_path);
  auto engine = TreeSeries{&slab_file, files.info_path};
  auto short_chunk = std::vector<std::byte>(kChunkSize - 1U);
  auto long_chunk = std::vector<std::byte>(kChunkSize + 1U);

  REQUIRE_THROWS_AS((engine.Put(0U, 0U, short_chunk)), std::runtime_error);
  REQUIRE_THROWS_AS((engine.Put(0U, 0U, long_chunk)), std::runtime_error);
}

TEST_CASE("TreeSeries puts items across many cells, filling each slab", "[treeseries]") {
  constexpr auto kCells = 512U;

  auto files = EngineFiles{"treeseries_engine_many"};
  auto slab_file = SlabFile::Create(files.slab_path);
  auto engine = TreeSeries{&slab_file, files.info_path};

  // The pairs below hit kCells distinct cells and make the source wrap the column
  // count. kItemsPerSlab + 1 puts per cell fills a slab, hands it off and claims
  // the next one, so this walks the whole state machine kCells times over.
  for (auto cell = 0U; cell < kCells; ++cell) {
    for (auto item = 0U; item < kItemsPerSlab + 1U; ++item) {
      REQUIRE_NOTHROW(PutChunk(engine, MetaHash::kMetaHashCols * 3U + cell,
                               static_cast<std::uint16_t>(cell), item));
    }
  }

  // Both extremes exercise the modulo arithmetic.
  REQUIRE_NOTHROW(PutChunk(engine, UINT64_MAX, UINT16_MAX, 0U));
}

TEST_CASE("TreeSeries serialises puts into one cell", "[treeseries]") {
  constexpr auto kWriters = 8U;
  constexpr auto kPerWriter = 64U;

  auto files = EngineFiles{"treeseries_engine_contended"};
  auto slab_file = SlabFile::Create(files.slab_path);
  auto engine = TreeSeries{&slab_file, files.info_path};
  std::latch start{static_cast<std::ptrdiff_t>(kWriters)};

  // Every writer shares one cell, so all of them run through the same mutex,
  // the same fill counter, the same slab handoff and the same MetaHash::Item.
  // Sources a column apart are different series, one writer each: a series'
  // items have to arrive in time order, and only its own writer can promise
  // that.
  std::array<std::thread, kWriters> writers;
  for (auto i = 0U; i < kWriters; ++i) {
    writers[i] = std::thread([&engine, &start, i] {
      start.arrive_and_wait();
      for (auto j = 0U; j < kPerWriter; ++j) {
        PutChunk(engine, i * MetaHash::kMetaHashCols, 0U, j);
      }
    });
  }

  for (auto& writer : writers) {
    writer.join();
  }
}

// The engine has no way to read a slab back before it is indexed, so the
// byte-level checks go through TreeSeries' test seam.
struct TreeSeriesTestAccess {
  [[nodiscard]] static auto Cell(TreeSeries& engine, std::uint16_t metric_id,
                                 std::uint64_t source_id) -> MetaHash::Item& {
    return engine.meta_hash_->GetItemFor(metric_id, source_id);
  }

  [[nodiscard]] static auto SlabBytes(const TreeSeries& engine, SlabId slab_id)
      -> std::span<std::byte, kSlabSize> {
    return engine.SlabBytes(slab_id);
  }

  [[nodiscard]] static auto DiskSlab(const TreeSeries& engine, std::uint32_t disk_id)
      -> std::span<std::byte, kSlabSize> {
    return engine.slab_file_.Slab(disk_id);
  }

  [[nodiscard]] static auto TryPopFull(TreeSeries& engine, SlabId* slab_id) -> bool {
    return engine.mem_full_queue_->TryPop(slab_id);
  }

  [[nodiscard]] static auto TryPopEvict(TreeSeries& engine, SlabId* slab_id) -> bool {
    return engine.mem_evict_queue_->TryPop(slab_id);
  }

  [[nodiscard]] static auto FreeRemaining(const TreeSeries& engine) -> std::uint32_t {
    return engine.mem_free_queue_.Remaining();
  }

  [[nodiscard]] static auto DiskUsed(const TreeSeries& engine) -> std::uint32_t {
    return kDiskSlabCount - engine.disk_free_queue_.Remaining();
  }

  [[nodiscard]] static auto Entry(TreeSeries& engine, std::uint64_t source_id,
                                  std::uint16_t metric_id, Timestamp min_start)
      -> const IndexEntry* {
    return engine.tree_index_.Find(source_id, metric_id, min_start);
  }

  // Migrations are asynchronous; the tests wait for the migrator to go quiet.
  static void WaitIdle(TreeSeries& engine) {
    while (engine.mem_full_queue_->Size() != 0 ||
           engine.migrator_busy_.load(std::memory_order_relaxed)) {
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
  }
};

TEST_CASE("TreeSeries writes the item's bytes into the slab", "[treeseries]") {
  constexpr auto kSourceId = 0x1122334455667788ULL;
  constexpr auto kMetricId = static_cast<std::uint16_t>(0xABCDU);

  auto files = EngineFiles{"treeseries_engine_bytes"};
  auto slab_file = SlabFile::Create(files.slab_path);
  auto engine = TreeSeries{&slab_file, files.info_path};
  auto chunk = MakeChunk(0x1234U, 0x5AU);

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
  auto files = EngineFiles{"treeseries_engine_boundary"};
  auto slab_file = SlabFile::Create(files.slab_path);
  auto engine = TreeSeries{&slab_file, files.info_path};
  auto slab_id = SlabId::kInvalid;

  for (auto i = 0U; i < kItemsPerSlab - 1U; ++i) {
    PutChunk(engine, 0U, 0U, i);
  }

  // One item short: the cell still holds the slab, and nothing was handed off.
  const auto& cell = TreeSeriesTestAccess::Cell(engine, 0U, 0U);
  REQUIRE(cell.slab_id != SlabId::kInvalid);
  REQUIRE(cell.used == kItemsPerSlab - 1U);
  REQUIRE_FALSE(TreeSeriesTestAccess::TryPopFull(engine, &slab_id));

  const auto open_slab = cell.slab_id;
  PutChunk(engine, 0U, 0U, kItemsPerSlab - 1U);

  // The kItemsPerSlab-th item fills it: the slab enters MemFullQueue and the cell
  // is cleared in the same breath.
  REQUIRE(cell.slab_id == SlabId::kInvalid);
  REQUIRE(TreeSeriesTestAccess::TryPopFull(engine, &slab_id));
  REQUIRE(slab_id == open_slab);
}

TEST_CASE("TreeSeries claims a fresh slab after handing one over", "[treeseries]") {
  auto files = EngineFiles{"treeseries_engine_fresh_slab"};
  auto slab_file = SlabFile::Create(files.slab_path);
  auto engine = TreeSeries{&slab_file, files.info_path};

  for (auto i = 0U; i < kItemsPerSlab; ++i) {
    PutChunk(engine, 0U, 0U, i);
  }

  auto handed = SlabId::kInvalid;
  REQUIRE(TreeSeriesTestAccess::TryPopFull(engine, &handed));

  PutChunk(engine, 0U, 0U, kItemsPerSlab);

  const auto& cell = TreeSeriesTestAccess::Cell(engine, 0U, 0U);
  REQUIRE(cell.slab_id != SlabId::kInvalid);
  REQUIRE(cell.slab_id != handed);
  REQUIRE(cell.used == 1U);
}

TEST_CASE("TreeSeries keeps every handed-out slab accounted for", "[treeseries]") {
  constexpr auto kCells = 64U;

  auto files = EngineFiles{"treeseries_engine_accounting"};
  auto slab_file = SlabFile::Create(files.slab_path);
  auto engine = TreeSeries{&slab_file, files.info_path};

  for (auto cell = 0U; cell < kCells; ++cell) {
    for (auto item = 0U; item < kItemsPerSlab; ++item) {
      PutChunk(engine, cell, 0U, item);
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

TEST_CASE("TreeSeries returns a series' chunks by time range", "[treeseries]") {
  constexpr auto kSource = 0xABCDULL;
  constexpr auto kMetric = static_cast<std::uint16_t>(7U);

  auto files = EngineFiles{"treeseries_engine_get"};
  auto slab_file = SlabFile::Create(files.slab_path);
  auto engine = TreeSeries{&slab_file, files.info_path};

  // Two series share one slab; each writes chunks 1000 apart in time.
  for (auto i = 0U; i < 8U; ++i) {
    PutChunk(engine, kSource, kMetric, 1000U * (i + 1U), static_cast<std::uint8_t>(i));
    PutChunk(engine, kSource + 1U, kMetric, 1000U * (i + 1U));
  }

  // A range inside the series returns the chunks that start in it, plus the one
  // before, which may cover the range's start.
  auto chunks = engine.Get(kSource, kMetric, 2500U, 6500U);
  REQUIRE(StartsOf(chunks) == std::vector<Timestamp>{2000U, 3000U, 4000U, 5000U, 6000U});
  REQUIRE(ChunkMarker(chunks.front()) == 1U);

  // The look-back is exactly one chunk, however far back the entry starts.
  chunks = engine.Get(kSource, kMetric, 3200U, 4200U);
  REQUIRE(StartsOf(chunks) == std::vector<Timestamp>{3000U, 4000U});
  REQUIRE(ChunkMarker(chunks.front()) == 2U);

  // Both ends of the range are inclusive, and the series' first chunk has no
  // look-back to make.
  chunks = engine.Get(kSource, kMetric, 1000U, 4000U);
  REQUIRE(StartsOf(chunks) == std::vector<Timestamp>{1000U, 2000U, 3000U, 4000U});
  REQUIRE(ChunkMarker(chunks.front()) == 0U);

  // Nothing before the first chunk, nothing past the last one but the last one,
  // an inverted range and an unwritten series are all easy cases.
  REQUIRE(engine.Get(kSource, kMetric, 0U, 500U).empty());
  chunks = engine.Get(kSource, kMetric, 9000U, 10000U);
  REQUIRE(StartsOf(chunks) == std::vector<Timestamp>{8000U});
  REQUIRE(engine.Get(kSource, kMetric, 5000U, 4000U).empty());
  REQUIRE(engine.Get(kSource + 2U, kMetric, 0U, UINT64_MAX).empty());

  // The other series shares the slab but not its chunks.
  chunks = engine.Get(kSource + 1U, kMetric, 2500U, 4500U);
  REQUIRE(StartsOf(chunks) == std::vector<Timestamp>{2000U, 3000U, 4000U});
  REQUIRE(ChunkMarker(chunks.front()) == 0U);
}

TEST_CASE("TreeSeries gets chunks a migration moved to the slab file", "[treeseries]") {
  constexpr auto kSource = 0x1234ULL;
  constexpr auto kMetric = static_cast<std::uint16_t>(3U);
  constexpr auto kSlabs = 2U;

  auto files = EngineFiles{"treeseries_engine_migrate"};
  auto slab_file = SlabFile::Create(files.slab_path);
  // A threshold of one slab migrates as soon as anything is handed over.
  auto engine = TreeSeries{&slab_file, files.info_path, 1U};

  for (auto i = 0U; i < kItemsPerSlab * kSlabs; ++i) {
    PutChunk(engine, kSource, kMetric, 100U * (i + 1U), static_cast<std::uint8_t>(i));
  }
  PutChunk(engine, 0U, UINT16_MAX, 1U);  // a second cell, with a partial slab

  TreeSeriesTestAccess::WaitIdle(engine);

  // Both full slabs went to disk, and the index followed them there: the first
  // item of the disk slab is the series' first chunk.
  REQUIRE(TreeSeriesTestAccess::DiskUsed(engine) == kSlabs);
  const auto* entry = TreeSeriesTestAccess::Entry(engine, kSource, kMetric, 100U);
  REQUIRE(entry != nullptr);
  REQUIRE((entry->location.load() & kIndexDiskFlag) != 0);

  const auto disk_slab =
      TreeSeriesTestAccess::DiskSlab(engine, entry->location.load() & ~kIndexDiskFlag);
  auto head_source = std::uint64_t{};
  auto head_metric = std::uint16_t{};
  auto head_start = Timestamp{};
  std::memcpy(&head_source, disk_slab.data(), kSourceIdSize);
  std::memcpy(&head_metric, disk_slab.data() + kSourceIdSize, kMetricIdSize);
  std::memcpy(&head_start, disk_slab.data() + kSourceIdSize + kMetricIdSize, sizeof(head_start));
  REQUIRE(head_source == kSource);
  REQUIRE(head_metric == kMetric);
  REQUIRE(head_start == 100U);

  // The slabs are back in the recycle queue, and the data is still readable.
  auto slab_id = SlabId::kInvalid;
  REQUIRE(TreeSeriesTestAccess::TryPopEvict(engine, &slab_id));
  const auto chunks = engine.Get(kSource, kMetric, 0U, UINT64_MAX);
  REQUIRE(chunks.size() == kItemsPerSlab * kSlabs);
  REQUIRE(ChunkStart(chunks.front()) == 100U);
  REQUIRE(ChunkMarker(chunks.back()) == kItemsPerSlab * kSlabs - 1U);
}

TEST_CASE("TreeSeries recovers what it migrated before it stopped", "[treeseries]") {
  constexpr auto kSource = 0x99ULL;
  constexpr auto kMetric = static_cast<std::uint16_t>(1U);

  auto files = EngineFiles{"treeseries_engine_recover"};
  auto slab_file = SlabFile::Create(files.slab_path);

  {
    auto engine = TreeSeries{&slab_file, files.info_path, 1U};
    for (auto i = 0U; i < kItemsPerSlab; ++i) {
      PutChunk(engine, kSource, kMetric, 10U * (i + 1U), static_cast<std::uint8_t>(i));
    }
    PutChunk(engine, 0U, 0U, 1U);  // a second cell, with a partial slab
  }

  // A new engine over the same files: the info area rebuilds the index, so both
  // slabs are queryable and their disk slabs are not handed out again.
  auto reopened = SlabFile::Open(files.slab_path);
  auto engine = TreeSeries{&reopened, files.info_path, 1U};

  const auto chunks = engine.Get(kSource, kMetric, 0U, UINT64_MAX);
  REQUIRE(chunks.size() == kItemsPerSlab);
  REQUIRE(ChunkStart(chunks.front()) == 10U);
  REQUIRE(ChunkMarker(chunks.front()) == 0U);
  REQUIRE(ChunkMarker(chunks.back()) == kItemsPerSlab - 1U);
  REQUIRE(TreeSeriesTestAccess::DiskUsed(engine) == 2U);

  // A slab that filled on its own is no more durable than one the shutdown
  // flushed: both are there, down to the single-item one.
  REQUIRE(engine.Get(0U, 0U, 0U, UINT64_MAX).size() == 1U);

  // The free-disk cursor came back too: the next migration lands behind the
  // recovered slabs instead of overwriting them.
  for (auto i = 0U; i < kItemsPerSlab; ++i) {
    PutChunk(engine, kSource, kMetric, 1000U + 10U * i);
  }
  TreeSeriesTestAccess::WaitIdle(engine);
  REQUIRE(TreeSeriesTestAccess::DiskUsed(engine) == 3U);
  REQUIRE(engine.Get(kSource, kMetric, 0U, UINT64_MAX).size() == kItemsPerSlab * 2U);
}

TEST_CASE("TreeSeries drops a torn disk info record", "[treeseries]") {
  constexpr auto kSource = 0x77ULL;
  constexpr auto kMetric = static_cast<std::uint16_t>(9U);

  auto files = EngineFiles{"treeseries_engine_torn"};
  auto slab_file = SlabFile::Create(files.slab_path);

  {
    auto engine = TreeSeries{&slab_file, files.info_path, 1U};
    for (auto i = 0U; i < kItemsPerSlab; ++i) {
      PutChunk(engine, kSource, kMetric, 10U * (i + 1U));
    }
    TreeSeriesTestAccess::WaitIdle(engine);
  }

  // Simulate a crash midway through appending the next record.
  const auto size = std::filesystem::file_size(files.info_path);
  std::filesystem::resize_file(files.info_path, size + sizeof(DiskInfoHeader) / 2);

  auto reopened = SlabFile::Open(files.slab_path);
  auto engine = TreeSeries{&reopened, files.info_path, 1U};

  REQUIRE(engine.Get(kSource, kMetric, 0U, UINT64_MAX).size() == kItemsPerSlab);
  REQUIRE(std::filesystem::file_size(files.info_path) == size);  // the tail was dropped
}

TEST_CASE("TreeSeries refuses a series that goes back in time", "[treeseries]") {
  auto files = EngineFiles{"treeseries_engine_order"};
  auto slab_file = SlabFile::Create(files.slab_path);
  auto engine = TreeSeries{&slab_file, files.info_path};

  PutChunk(engine, 1U, 1U, 100U);
  REQUIRE_NOTHROW(PutChunk(engine, 1U, 1U, 100U));  // a duplicate is still in order
  REQUIRE_THROWS_AS(PutChunk(engine, 1U, 1U, 99U), std::runtime_error);

  // The same chunk into a later slab is a duplicate of an indexed start time.
  for (auto i = 1U; i < kItemsPerSlab; ++i) {
    PutChunk(engine, 1U, 1U, 100U + i);
  }
  REQUIRE_THROWS_AS(PutChunk(engine, 1U, 1U, 100U), std::runtime_error);
}

TEST_CASE("TreeSeries reads while writing to one series", "[treeseries]") {
  constexpr auto kWriters = 4U;
  constexpr auto kPerWriter = kItemsPerSlab * 2U;

  auto files = EngineFiles{"treeseries_engine_reader"};
  auto slab_file = SlabFile::Create(files.slab_path);
  auto engine = TreeSeries{&slab_file, files.info_path, 1U};
  std::latch start{static_cast<std::ptrdiff_t>(kWriters + 1U)};

  std::array<std::thread, kWriters> writers;
  for (auto w = 0U; w < kWriters; ++w) {
    writers[w] = std::thread([&engine, &start, w] {
      start.arrive_and_wait();
      for (auto i = 0U; i < kPerWriter; ++i) {
        PutChunk(engine, w, 1U, static_cast<Timestamp>(i) * kWriters + w);
      }
    });
  }

  // One reader hammers the same series across slab handoffs and migrations.
  // There is nothing to assert about what a racing reader sees, only that it
  // sees well-formed chunks and never crashes: TSan and ASan do the real work.
  auto reader = std::thread([&engine, &start] {
    start.arrive_and_wait();
    for (auto i = 0U; i < 50U; ++i) {
      for (auto w = 0U; w < kWriters; ++w) {
        for (const auto& chunk : engine.Get(w, 1U, 0U, UINT64_MAX)) {
          if (ChunkStart(chunk) % kWriters != w) {
            std::abort();
          }
        }
      }
    }
  });

  for (auto& writer : writers) {
    writer.join();
  }
  reader.join();
}
