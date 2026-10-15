#include "treeseries/tree_index.hpp"

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace {

using Tuple = std::tuple<std::uint64_t, std::uint16_t, Timestamp>;

auto KeyOf(const Tuple& tuple) -> std::array<std::byte, TreeIndex::kKeySize> {
  const auto& [source_id, metric_id, start] = tuple;
  return TreeIndex::Key(source_id, metric_id, start);
}

[[nodiscard]] auto StartsOf(const std::vector<const IndexEntry*>& entries)
    -> std::vector<Timestamp> {
  auto starts = std::vector<Timestamp>{};
  starts.reserve(entries.size());
  std::ranges::transform(entries, std::back_inserter(starts),
                         [](const IndexEntry* entry) { return entry->min_start; });
  return starts;
}

}  // namespace

TEST_CASE("TreeIndex encodes keys in tuple order", "[tree_index]") {
  // Each pair differs in one field only, and the fields' positions in the key
  // have to mirror their weight in the tuple: masstree compares raw bytes.
  const auto tuples = std::vector<Tuple>{
      {0, 0, 0},
      {0, 0, 1},
      {0, 1, 0},
      {0, 256, 0},
      {0, 65535, 0},
      {1, 0, 0},
      {7, 9, 0},
      {256, 0, 0},
      {UINT64_MAX, UINT16_MAX, UINT64_MAX - 1},
      {UINT64_MAX, UINT16_MAX, UINT64_MAX},
  };

  for (auto i = 0uz; i + 1 < tuples.size(); ++i) {
    const auto left = KeyOf(tuples[i]);
    const auto right = KeyOf(tuples[i + 1]);
    INFO(i);
    REQUIRE(std::ranges::lexicographical_compare(left, right));
  }
}

TEST_CASE("TreeIndex finds and scans a series' groups", "[tree_index]") {
  constexpr auto kSeries = 7U;
  constexpr auto kLower = 6U;   // a series that sorts before, to keep out
  constexpr auto kHigher = 8U;  // and one that sorts after
  constexpr auto kCount = 2048U;

  auto index = TreeIndex{};
  for (auto i = 0U; i < kCount; ++i) {
    // Enough groups to split leaves several times over, in three series that
    // share a metric.
    index.Insert(kSeries, 9U, 10U * i, i);
    index.Insert(kLower, 9U, 10U * i, i);
    index.Insert(kHigher, 9U, 10U * i, i);
  }

  // Every group is pointing at the slab it was inserted with.
  for (auto i = 0U; i < kCount; i += 97U) {
    const auto* entry = index.Find(kSeries, 9U, 10U * i);
    REQUIRE(entry != nullptr);
    REQUIRE(entry->source_id == kSeries);
    REQUIRE(entry->metric_id == 9U);
    REQUIRE(entry->min_start == 10U * i);
    REQUIRE(entry->location.load() == i);
  }
  REQUIRE(index.Find(kSeries, 9U, 5U) == nullptr);  // not a group this series has
  REQUIRE(index.Find(9U, 9U, 0U) == nullptr);       // not a series at all

  // A range inside a series: the groups starting in it, and the one before,
  // which is the only candidate for a chunk covering its start.
  auto entries = index.Scan(kSeries, 9U, 25U, 65U);
  REQUIRE(StartsOf(entries) == std::vector<Timestamp>{20U, 30U, 40U, 50U, 60U});

  // The series' first group has no predecessor to find.
  entries = index.Scan(kSeries, 9U, 0U, 15U);
  REQUIRE(StartsOf(entries) == std::vector<Timestamp>{0U, 10U});

  // A range between two groups still gets the earlier one.
  entries = index.Scan(kSeries, 9U, 16U, 19U);
  REQUIRE(StartsOf(entries) == std::vector<Timestamp>{10U});

  // Past the last group, that group is all there is.
  entries = index.Scan(kSeries, 9U, 10U * kCount, UINT64_MAX);
  REQUIRE(StartsOf(entries) == std::vector<Timestamp>{10U * (kCount - 1U)});

  // An inverted range, and a series nobody inserted.
  REQUIRE(index.Scan(kSeries, 9U, 65U, 25U).empty());
  REQUIRE(index.Scan(kSeries, 4U, 0U, UINT64_MAX).empty());

  // A different series' groups never come along.
  for (const auto& entry : index.Scan(kSeries, 9U, 25U, 65U)) {
    REQUIRE(entry->source_id == kSeries);
  }
}

TEST_CASE("TreeIndex refuses a group that is already indexed", "[tree_index]") {
  auto index = TreeIndex{};
  index.Insert(3U, 4U, 100U, 0U);

  // The same key twice is a caller bug: a series writes each chunk once.
  REQUIRE_THROWS_AS(index.Insert(3U, 4U, 100U, 1U), std::runtime_error);

  // Same series and time under another metric is a different group.
  REQUIRE_NOTHROW(index.Insert(3U, 5U, 100U, 1U));
}
