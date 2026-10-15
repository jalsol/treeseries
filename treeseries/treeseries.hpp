#pragma once

#include <sys/mman.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <memory>
#include <ranges>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "treeseries/common.hpp"
#include "treeseries/disk_free_queue.hpp"
#include "treeseries/disk_info_area.hpp"
#include "treeseries/mem_evict_queue.hpp"
#include "treeseries/mem_free_queue.hpp"
#include "treeseries/mem_full_queue.hpp"
#include "treeseries/metahash.hpp"
#include "treeseries/slab_file.hpp"
#include "treeseries/tree_index.hpp"

class TreeSeries {
  // Lets the tests read slab bytes and queue membership back; neither is public API.
  friend struct TreeSeriesTestAccess;

 public:
  static_assert(kChunkSize >= sizeof(Timestamp), "chunk must carry its StartTime");

  TreeSeries(SlabFile* slab_file, const std::string& info_path,
             std::uint32_t migration_threshold = kMemSlabCount / 6U)
      : slab_buffer_{MapBuffer()},
        slab_file_{*slab_file},
        disk_free_queue_{*slab_file},
        disk_info_area_{info_path},
        mem_full_queue_{std::make_unique<MemFullQueue>()},
        mem_evict_queue_{std::make_unique<MemEvictQueue>()},
        meta_hash_{std::make_unique<MetaHash>()},
        migration_threshold_{migration_threshold} {
    const auto first_free = disk_info_area_.Replay(
        [this](std::uint32_t disk_id, std::span<const DiskInfoEntry> entries) {
          RestoreEntries(disk_id, entries);
        });
    disk_free_queue_ = DiskFreeQueue::Restored(slab_file_, first_free);

    migrator_ = std::jthread{[this](std::stop_token stop) { MigratorMain(stop); }};
  }

  TreeSeries(const TreeSeries&) = delete;
  TreeSeries& operator=(const TreeSeries&) = delete;

  ~TreeSeries() {
    Flush();
    migrator_.request_stop();
    migrator_cv_.notify_all();
    if (migrator_.joinable()) {
      migrator_.join();
    }
  }

  void Flush() {
    for (auto& row : meta_hash_->table) {
      for (auto& item : row) {
        std::scoped_lock lock{item.mutex};
        if (item.slab_id == SlabId::kInvalid) {
          continue;
        }

        slab_used_[item.slab_id] = item.used;
        mem_full_queue_->Push(item.slab_id);
        item.slab_id = SlabId::kInvalid;
        item.used = 0;
        std::ranges::fill(item.entries, nullptr);
      }
    }

    flush_requested_.store(true, std::memory_order_release);
    migrator_cv_.notify_all();
    while (flush_requested_.load(std::memory_order_acquire)) {
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
  }

  auto Put(std::uint64_t source_id, std::uint16_t metric_id, std::span<std::byte> chunk) {
    if (chunk.size() != kChunkSize) {
      throw std::runtime_error(
          std::format("TreeSeries: chunk is {} bytes, expected {}", chunk.size(), kChunkSize));
    }

    auto handed_off = false;
    {
      auto& item = meta_hash_->GetItemFor(metric_id, source_id);
      std::scoped_lock lock(item.mutex);

      if (item.slab_id == SlabId::kInvalid) {
        ClaimSlab(item);
      }

      const auto slot = static_cast<std::uint16_t>(item.used);
      const auto bytes = SlabBytes(item.slab_id).subspan(item.used * kItemSize, kItemSize);

      std::memcpy(bytes.data(), &source_id, kSourceIdSize);
      std::memcpy(bytes.data() + kSourceIdSize, &metric_id, kMetricIdSize);
      std::memcpy(bytes.data() + kSourceIdSize + kMetricIdSize, chunk.data(), kChunkSize);

      item.entries[slot] = IndexSeries(item, slot, source_id, metric_id, StartTimeOf(chunk));

      ++item.used;
      if (item.used == kItemsPerSlab) {
        slab_used_[item.slab_id] = kItemsPerSlab;
        mem_full_queue_->Push(item.slab_id);
        item.slab_id = SlabId::kInvalid;
        handed_off = true;
      }
    }

    if (handed_off && mem_full_queue_->Size() >= migration_threshold_) {
      migrator_cv_.notify_one();
    }
  }

  auto Get(std::uint64_t source_id, std::uint16_t metric_id, Timestamp t_start, Timestamp t_end)
      -> std::vector<Chunk> {
    auto found = std::vector<std::pair<Timestamp, Chunk>>{};
    auto straddler = std::pair<Timestamp, Chunk>{};
    auto has_straddler = false;

    for (const auto* entry : tree_index_.Scan(source_id, metric_id, t_start, t_end)) {
      for (auto& [start, chunk] : EntryChunks(*entry)) {
        if (start > t_end) {
          continue;
        }
        if (start >= t_start) {
          found.emplace_back(start, chunk);
        } else if (!has_straddler || start > straddler.first) {
          straddler = {start, chunk};
          has_straddler = true;
        }
      }
    }

    if (has_straddler) {
      found.push_back(straddler);
    }
    std::ranges::sort(found, {}, &std::pair<Timestamp, Chunk>::first);

    auto chunks = std::vector<Chunk>{};
    chunks.reserve(found.size());
    std::ranges::transform(found, std::back_inserter(chunks),
                           [](auto& pair) { return std::move(pair.second); });
    return chunks;
  }

 private:
  static constexpr auto kMigrationBatch = std::uint32_t{1} << 10;
  static constexpr auto kReadAttempts = 3uz;

  struct MappingDeleter {
    void operator()(std::byte* mapping) const {
      if (mapping != nullptr && ::munmap(mapping, kSlabBufferSize) == -1) {
        std::fprintf(stderr, "TreeSeries: Cannot munmap: %s\n", std::strerror(errno));
      }
    }
  };

  [[nodiscard]] static auto MapBuffer() -> std::unique_ptr<std::byte, MappingDeleter> {
    constexpr auto kFlags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE;

    auto* mapping = static_cast<std::byte*>(
        ::mmap(nullptr, kSlabBufferSize, PROT_READ | PROT_WRITE, kFlags | MAP_HUGETLB, -1, 0));
    if (mapping == MAP_FAILED) {
      mapping = static_cast<std::byte*>(
          ::mmap(nullptr, kSlabBufferSize, PROT_READ | PROT_WRITE, kFlags, -1, 0));
    }
    if (mapping == MAP_FAILED) {
      throw std::runtime_error(std::format("TreeSeries: Cannot mmap: {}", std::strerror(errno)));
    }

    return std::unique_ptr<std::byte, MappingDeleter>{mapping};
  }

  [[nodiscard]] auto SlabBytes(SlabId slab_id) const -> std::span<std::byte, kSlabSize> {
    const auto offset = static_cast<std::size_t>(slab_id) * kSlabSize;
    return std::span<std::byte, kSlabSize>{slab_buffer_.get() + offset, kSlabSize};
  }

  struct ItemHead {
    std::uint64_t source_id;
    std::uint16_t metric_id;
    Timestamp start;
  };

  [[nodiscard]] static auto ReadItemHead(const std::byte* base) -> ItemHead {
    auto head = ItemHead{};
    std::memcpy(&head.source_id, base, kSourceIdSize);
    std::memcpy(&head.metric_id, base + kSourceIdSize, kMetricIdSize);
    std::memcpy(&head.start, base + kSourceIdSize + kMetricIdSize, sizeof(head.start));
    return head;
  }

  [[nodiscard]] static auto StartTimeOf(std::span<const std::byte> chunk) -> Timestamp {
    auto start = Timestamp{};
    std::memcpy(&start, chunk.data(), sizeof(start));
    return start;
  }

  void ClaimSlab(MetaHash::Item& item) {
    if (const auto slab_id = mem_free_queue_.TryPop(); slab_id.has_value()) {
      item.slab_id = slab_id.value();
    } else {
      while (!mem_evict_queue_->TryPop(&item.slab_id)) {
        std::this_thread::yield();
      }
    }

    item.used = 0;
    std::ranges::fill(item.entries, nullptr);
  }

  auto IndexSeries(MetaHash::Item& item, std::uint16_t slot, std::uint64_t source_id,
                   std::uint16_t metric_id, Timestamp start) -> IndexEntry* {
    for (auto i = 0u; i < item.used; ++i) {
      auto* entry = item.entries[i];
      if (entry == nullptr || entry->source_id != source_id || entry->metric_id != metric_id) {
        continue;
      }

      if (start < entry->max_start.load(std::memory_order_relaxed)) {
        throw std::runtime_error(std::format(
            "TreeSeries: source {} metric {} goes back in time within a slab ({} after {}); a "
            "series' chunks must be written in time order",
            source_id, metric_id, start, entry->max_start.load(std::memory_order_relaxed)));
      }

      entry->max_start.store(start, std::memory_order_relaxed);
      entry->slots.fetch_or(SlotBit(slot), std::memory_order_release);
      return entry;
    }

    auto* entry = &tree_index_.Insert(source_id, metric_id, start, MemLocation(item.slab_id));
    entry->slots.store(SlotBit(slot), std::memory_order_release);
    return entry;
  }

  [[nodiscard]] static auto MemLocation(SlabId slab_id) -> std::uint32_t {
    return static_cast<std::uint32_t>(slab_id) & ~kIndexDiskFlag;
  }

  [[nodiscard]] auto EntryChunks(const IndexEntry& entry) const
      -> std::vector<std::pair<Timestamp, Chunk>> {
    for (auto attempt = 0uz; attempt < kReadAttempts; ++attempt) {
      const auto location = entry.location.load(std::memory_order_acquire);
      const auto slab_id = location & ~kIndexDiskFlag;
      const auto slab =
          (location & kIndexDiskFlag) != 0 ? slab_file_.Slab(slab_id) : SlabBytes(SlabId{slab_id});
      const auto slots = entry.slots.load(std::memory_order_acquire);

      auto chunks = std::vector<std::pair<Timestamp, Chunk>>{};
      for (auto slot = 0uz; slot < kItemsPerSlab; ++slot) {
        if ((slots & SlotBit(static_cast<std::uint32_t>(slot))) == 0) {
          continue;
        }

        const auto* base = slab.data() + slot * kItemSize;
        const auto head = ReadItemHead(base);
        if (head.source_id != entry.source_id || head.metric_id != entry.metric_id) {
          continue;
        }

        auto chunk = Chunk{};
        std::memcpy(chunk.data(), base + kSourceIdSize + kMetricIdSize, kChunkSize);
        chunks.emplace_back(head.start, chunk);
      }

      if (attempt + 1 == kReadAttempts ||
          entry.location.load(std::memory_order_acquire) == location) {
        return chunks;
      }
    }

    return {};
  }

  void RestoreEntries(std::uint32_t disk_id, std::span<const DiskInfoEntry> entries) {
    for (const auto& record : entries) {
      auto& entry = tree_index_.Insert(record.source_id, record.metric_id, record.min_start,
                                       kIndexDiskFlag | disk_id);
      entry.max_start.store(record.max_start, std::memory_order_relaxed);
      entry.slots.store(record.slots, std::memory_order_relaxed);
    }
  }

  void MigratorMain(std::stop_token stop) {
    TreeIndex::RegisterThread();

    while (true) {
      {
        std::unique_lock lock{migrator_mutex_};
        migrator_cv_.wait(lock, [&] {
          return stop.stop_requested() || flush_requested_.load(std::memory_order_relaxed) ||
                 (mem_full_queue_->Size() >= migration_threshold_ &&
                  disk_free_queue_.Remaining() != 0);
        });
      }

      if (stop.stop_requested()) {
        while (MigrateBatch()) {
        }
        return;
      }

      MigrateBatch();
      if (flush_requested_.load(std::memory_order_relaxed) && mem_full_queue_->Size() == 0) {
        flush_requested_.store(false, std::memory_order_release);
      }
    }
  }

  auto MigrateBatch() -> bool {
    migrator_busy_.store(true, std::memory_order_relaxed);
    const auto migrated = MigrateRuns();
    migrator_busy_.store(false, std::memory_order_relaxed);
    return migrated;
  }

  auto MigrateRuns() -> bool {
    const auto wanted =
        std::min({mem_full_queue_->Size(), kMigrationBatch, disk_free_queue_.Remaining()});
    if (wanted == 0) {
      if (mem_full_queue_->Size() != 0 && !disk_full_logged_.exchange(true)) {
        std::fprintf(stderr, "TreeSeries: slab file is full; writes will stall\n");
      }
      return false;
    }

    auto slab_ids = std::vector<std::uint32_t>{};
    slab_ids.reserve(wanted);
    auto slab_id = SlabId::kInvalid;
    while (slab_ids.size() < wanted && mem_full_queue_->TryPop(&slab_id)) {
      slab_ids.push_back(slab_id);
    }
    std::ranges::sort(slab_ids);

    const auto groups = AllocateDiskRuns(slab_ids);

    for (const auto& group : groups) {
      const auto* source = slab_buffer_.get() + std::size_t{group.mem_first} * kSlabSize;
      auto* target = slab_file_.Slab(group.disk_first).data();
      std::memcpy(target, source, std::size_t{group.count} * kSlabSize);
      slab_file_.Sync(group.disk_first, group.count);
    }

    for (const auto& group : groups) {
      for (auto i = 0u; i < group.count; ++i) {
        MoveSlabToDisk(SlabId{group.mem_first + i}, group.disk_first + i);
      }
    }
    disk_info_area_.Sync();

    for (const auto& group : groups) {
      for (auto i = 0u; i < group.count; ++i) {
        mem_evict_queue_->Push(SlabId{group.mem_first + i});
      }
    }

    return true;
  }

  struct SlabGroup {
    std::uint32_t mem_first;
    std::uint32_t disk_first;
    std::uint32_t count;
  };

  [[nodiscard]] auto AllocateDiskRuns(std::span<const std::uint32_t> slab_ids)
      -> std::vector<SlabGroup> {
    auto groups = std::vector<SlabGroup>{};

    for (auto i = 0uz; i < slab_ids.size();) {
      auto end = i + 1;
      while (end < slab_ids.size() && slab_ids[end] == slab_ids[end - 1] + 1) {
        ++end;
      }

      const auto count = static_cast<std::uint32_t>(end - i);
      if (const auto run = disk_free_queue_.TryAllocate(count); run.has_value()) {
        groups.push_back(SlabGroup{slab_ids[i], run->first, count});
      } else {
        for (auto k = i; k < end; ++k) {
          const auto one = disk_free_queue_.TryAllocate(1);
          if (!one.has_value()) {
            throw std::runtime_error("TreeSeries: Cannot allocate a disk slab");
          }
          groups.push_back(SlabGroup{slab_ids[k], one->first, 1});
        }
      }

      i = end;
    }

    return groups;
  }

  void MoveSlabToDisk(SlabId mem_id, std::uint32_t disk_id) {
    const auto slab = SlabBytes(mem_id);
    const auto used = slab_used_[mem_id];
    auto records = std::vector<DiskInfoEntry>{};

    for (auto slot = 0uz; slot < used; ++slot) {
      if (!IsGroupFirst(slab, slot)) {
        continue;
      }

      const auto head = ReadItemHead(slab.data() + slot * kItemSize);
      auto* entry = tree_index_.Find(head.source_id, head.metric_id, head.start);
      if (entry == nullptr) {
        throw std::runtime_error(std::format("TreeSeries: slab {} item {} is not in the tree index",
                                             static_cast<unsigned>(mem_id), slot));
      }

      records.push_back(DiskInfoEntry{
          head.source_id, head.start, entry->max_start.load(std::memory_order_relaxed),
          head.metric_id, entry->slots.load(std::memory_order_relaxed), 0});
      entry->location.store(kIndexDiskFlag | disk_id, std::memory_order_release);
    }

    disk_info_area_.Append(disk_id, records);
  }

  [[nodiscard]] static auto IsGroupFirst(std::span<std::byte, kSlabSize> slab, std::size_t slot)
      -> bool {
    const auto head = ReadItemHead(slab.data() + slot * kItemSize);

    for (auto i = 0uz; i < slot; ++i) {
      const auto other = ReadItemHead(slab.data() + i * kItemSize);
      if (other.source_id == head.source_id && other.metric_id == head.metric_id) {
        return false;
      }
    }

    return true;
  }

  std::unique_ptr<std::byte, MappingDeleter> slab_buffer_;
  SlabFile& slab_file_;

  // NOTE: I'm not supporting HDD, thus DiskFullQueue and DiskEvictQueue should be eliminated.
  DiskFreeQueue disk_free_queue_;
  DiskInfoArea disk_info_area_;
  MemFreeQueue mem_free_queue_;
  std::unique_ptr<MemFullQueue> mem_full_queue_;
  std::unique_ptr<MemEvictQueue> mem_evict_queue_;

  std::unique_ptr<MetaHash> meta_hash_;
  TreeIndex tree_index_;

  std::uint32_t migration_threshold_;
  std::jthread migrator_;
  std::mutex migrator_mutex_;
  std::condition_variable_any migrator_cv_;
  std::atomic<bool> migrator_busy_{};
  std::atomic<bool> flush_requested_{};
  std::atomic<bool> disk_full_logged_{};

  std::unique_ptr<std::uint32_t[]> slab_used_{std::make_unique<std::uint32_t[]>(kMemSlabCount)};
};
