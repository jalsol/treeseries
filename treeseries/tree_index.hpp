#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <format>
#include <mutex>
#include <stdexcept>
#include <vector>

#include "third_party/masstree_wrapper.hpp"
#include "treeseries/common.hpp"

// Bit 31 of an entry's location: the slab lives in the slab file, not the buffer.
constexpr auto kIndexDiskFlag = std::uint32_t{1} << 31;

[[nodiscard]] constexpr auto SlotBit(std::uint32_t slot) -> std::uint16_t {
  return static_cast<std::uint16_t>(1u << slot);
}

// One (slab, series) group: what one time series contributed to one slab,
// keyed by the series' first StartTime in it.
struct IndexEntry {
  IndexEntry(std::uint64_t source, std::uint16_t metric, Timestamp min, std::uint32_t where)
      : source_id{source}, metric_id{metric}, min_start{min}, max_start{min}, location{where} {}

  std::uint64_t source_id;
  std::uint16_t metric_id;
  Timestamp min_start;                  // the series' first item in the slab; in the key
  std::atomic<Timestamp> max_start;     // the last item's; grows as items are appended
  std::atomic<std::uint32_t> location;  // kIndexDiskFlag | slab id
  std::atomic<std::uint16_t> slots;     // bit i: item i of the slab is this group's
};

// The tree index of §3.3.2: a lockless B+ tree keyed by (SourceID, MetricID,
// first StartTime), one entry per (slab, series) group.
class TreeIndex {
 public:
  // masstree hands scans the key suffix past the current 8-byte ikey, not the
  // key, so range decisions come from the value: the wrapper's end bound is
  // unusable for keys this long.
  static constexpr auto kKeySize = 8uz + 2uz + 8uz;
  static constexpr auto kScanAttempts = 4uz;

  // masstree's threadinfo is thread-local: every using thread registers first.
  static void RegisterThread() {
    if (Tree::ti == nullptr) {
      static auto next_id = std::atomic<int>{};
      Tree::thread_init(next_id.fetch_add(1, std::memory_order_relaxed));
    }
  }

  // Keys compare as raw bytes, so every field is encoded big-endian.
  [[nodiscard]] static auto Key(std::uint64_t source_id, std::uint16_t metric_id, Timestamp start)
      -> std::array<std::byte, kKeySize> {
    auto key = std::array<std::byte, kKeySize>{};
    const auto source = std::bit_cast<std::array<std::byte, 8>>(std::byteswap(source_id));
    const auto metric = std::bit_cast<std::array<std::byte, 2>>(std::byteswap(metric_id));
    const auto time = std::bit_cast<std::array<std::byte, 8>>(std::byteswap(start));
    std::ranges::copy(source, key.begin());
    std::ranges::copy(metric, key.begin() + 8);
    std::ranges::copy(time, key.begin() + 10);
    return key;
  }

  // Indexes a new group. (source, metric, min_start) must be new: a series
  // writes its chunks in time order.
  auto Insert(std::uint64_t source_id, std::uint16_t metric_id, Timestamp min_start,
              std::uint32_t location) -> IndexEntry& {
    RegisterThread();

    auto* entry = [&] {
      std::scoped_lock lock{arena_mutex_};
      return &arena_.emplace_back(source_id, metric_id, min_start, location);
    }();

    const auto key = Key(source_id, metric_id, min_start);
    if (!tree_.insert_value(reinterpret_cast<const char*>(key.data()), kKeySize, entry)) {
      throw std::runtime_error(
          std::format("TreeIndex: source {} metric {} at {} is already indexed; a series must "
                      "write each chunk once, in time order",
                      source_id, metric_id, min_start));
    }

    return *entry;
  }

  [[nodiscard]] auto Find(std::uint64_t source_id, std::uint16_t metric_id, Timestamp min_start)
      -> IndexEntry* {
    RegisterThread();

    const auto key = Key(source_id, metric_id, min_start);
    return tree_.get_value(reinterpret_cast<const char*>(key.data()), kKeySize);
  }

  // Every group of the series that may hold chunks in [t_start, t_end], in key
  // order: the groups starting inside the range, plus the one just before it,
  // the only candidate for a chunk covering t_start.
  [[nodiscard]] auto Scan(std::uint64_t source_id, std::uint16_t metric_id, Timestamp t_start,
                          Timestamp t_end) -> std::vector<const IndexEntry*> {
    RegisterThread();

    auto entries = std::vector<const IndexEntry*>{};
    if (t_start > t_end) {
      return entries;
    }

    const auto lower = Key(source_id, metric_id, t_start);
    const auto upper = Key(source_id, metric_id, t_end);
    const auto floor = Key(source_id, metric_id, 0);

    for (auto attempt = 0uz; attempt < kScanAttempts; ++attempt) {
      entries.clear();
      auto context = ScanContext{source_id, metric_id, t_end, &entries};
      tree_.scan(reinterpret_cast<const char*>(lower.data()), kKeySize, false,
                 reinterpret_cast<const char*>(upper.data()), kKeySize, false,
                 std::move(context.callbacks()));

      auto previous = ScanContext{source_id, metric_id, t_end, nullptr};
      tree_.rscan(reinterpret_cast<const char*>(floor.data()), kKeySize, false,
                  reinterpret_cast<const char*>(lower.data()), kKeySize, true,
                  std::move(previous.callbacks()), 1);

      if (!context.Stable() || !previous.Stable()) {
        continue;  // A leaf changed under a concurrent Put; the pass is torn.
      }

      const auto* predecessor = previous.predecessor;
      if (predecessor != nullptr && predecessor->source_id == source_id &&
          predecessor->metric_id == metric_id) {
        entries.insert(entries.begin(), predecessor);  // entries stay in key order
      }

      return entries;
    }

    return entries;
  }

 private:
  using Tree = MasstreeWrapper<IndexEntry>;

  // Per-pass state: the entries collected so far (none, for the predecessor
  // probe), and the leaf-version trail below.
  struct ScanContext {
    std::uint64_t source_id;
    std::uint16_t metric_id;
    Timestamp t_end;
    std::vector<const IndexEntry*>* entries;
    const IndexEntry* predecessor = nullptr;
    const Tree::leaf_type* last_leaf = nullptr;
    std::uint64_t last_version = 0;
    bool unstable = false;

    [[nodiscard]] auto callbacks() -> Tree::Callback {
      return Tree::Callback{
          .per_node_func = [this](const Tree::leaf_type* leaf, std::uint64_t version,
                                  bool& keep_going) { VisitLeaf(leaf, version, keep_going); },
          .per_kv_func = [this](const Tree::Str&, const IndexEntry* entry,
                                bool& keep_going) { VisitValue(entry, keep_going); },
      };
    }

    // Validating the previous leaf as the next one starts catches a Put that
    // landed in a leaf we just walked. (leaf::full_version_value() is the live
    // version; the wrapper's get_version_value() names a member this masstree
    // revision lacks.)
    void VisitLeaf(const Tree::leaf_type* leaf, std::uint64_t version, bool& keep_going) {
      if (last_leaf != nullptr && last_leaf->full_version_value() != last_version) {
        unstable = true;
        keep_going = false;
        return;
      }

      last_leaf = leaf;
      last_version = version;
    }

    void VisitValue(const IndexEntry* entry, bool& keep_going) {
      if (entries == nullptr) {
        predecessor = entry;
        keep_going = false;
        return;
      }

      // Keys are ordered by (source, metric, time): the first group past the
      // series or the range ends the scan.
      if (entry->source_id != source_id || entry->metric_id != metric_id ||
          entry->min_start > t_end) {
        keep_going = false;
        return;
      }

      entries->push_back(entry);
    }

    [[nodiscard]] auto Stable() const -> bool {
      return !unstable && (last_leaf == nullptr || last_leaf->full_version_value() == last_version);
    }
  };

  Tree tree_;
  std::deque<IndexEntry> arena_;  // stable addresses; freed together with the index
  std::mutex arena_mutex_;        // taken under a MetaHash cell lock, never above one
};
