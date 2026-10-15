#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "treeseries/slab.hpp"
#include "treeseries/slab_file.hpp"
#include "treeseries/treeseries.hpp"

// Financial time series on TreeSeries: a (symbol, metric) pair such as
// ("AAPL", "close") is a series, its samples are doubles, and time is epoch
// seconds. Samples are packed into the engine's 246-byte chunks:
//
//   chunk[0..8)  StartTime, which the engine indexes
//   chunk[8..10) sample count
//   then count x (u32 seconds after the chunk's start, f64 value)
//
// A chunk's samples are ordered by time, so its start is the smallest of them
// and the engine's relaxed ordering holds.

struct Sample {
  Timestamp time;
  double value;
};

constexpr auto kSampleSize = 4uz + 8uz;
constexpr auto kSampleHeaderSize = 8uz + 2uz;
constexpr auto kSamplesPerChunk = (kChunkSize - kSampleHeaderSize) / kSampleSize;

static_assert(kSamplesPerChunk > 0, "a chunk must fit at least one sample");

// A sample stores its time as a u32 offset from its chunk's start.
constexpr auto kSampleDeltaLimit = Timestamp{UINT32_MAX};

// The catalog stands in for PromTCS' metadata backward index (§3.2): the engine
// deals in ids, so names map to them once and the mapping outlives the process.
// It is a plain append-only table, one "metric<TAB>name<TAB>id" or
// "source<TAB>name<TAB>id" line per registration.
class Catalog {
 public:
  explicit Catalog(const std::string& path) : path_{path} {
    auto in = std::ifstream{path};
    auto line = std::string{};
    while (std::getline(in, line)) {
      const auto first = line.find('\t');
      const auto second = line.find('\t', first == std::string::npos ? 0 : first + 1);
      if (first == std::string::npos || second == std::string::npos) {
        continue;
      }

      const auto kind = line.substr(0, first);
      const auto name = line.substr(first + 1, second - first - 1);
      const auto id = std::stoull(line.substr(second + 1));
      if (kind == "source") {
        sources_.insert_or_assign(name, id);
      } else if (kind == "metric") {
        metrics_.insert_or_assign(name, static_cast<std::uint16_t>(id));
      }
    }

    // Ids are handed out from zero and up, so the count is the next one.
    if (metrics_.size() >= 1uz << 16) {
      throw std::runtime_error("Catalog: metric ids are a 16-bit space and it is full");
    }

    out_ = std::ofstream{path, std::ios::app};
    if (!out_) {
      throw std::runtime_error(std::format("Catalog: Cannot append to {}", path));
    }
  }

  // Registration on write, lookup on read: a query for a symbol nobody wrote
  // must not invent an id.
  auto SourceId(std::string_view name) -> std::uint64_t {
    if (const auto found = sources_.find(std::string{name}); found != sources_.end()) {
      return found->second;
    }

    const auto id = sources_.size();
    sources_.emplace(name, id);
    Register("source", name, id);
    return id;
  }

  auto MetricId(std::string_view name) -> std::uint16_t {
    if (const auto found = metrics_.find(std::string{name}); found != metrics_.end()) {
      return found->second;
    }

    if (metrics_.size() >= 1uz << 16) {
      throw std::runtime_error("Catalog: metric ids are a 16-bit space and it is full");
    }

    const auto id = static_cast<std::uint16_t>(metrics_.size());
    metrics_.emplace(name, id);
    Register("metric", name, id);
    return id;
  }

  [[nodiscard]] auto FindSource(std::string_view name) const -> std::optional<std::uint64_t> {
    const auto found = sources_.find(std::string{name});
    return found == sources_.end() ? std::nullopt : std::optional{found->second};
  }

  [[nodiscard]] auto FindMetric(std::string_view name) const -> std::optional<std::uint16_t> {
    const auto found = metrics_.find(std::string{name});
    return found == metrics_.end() ? std::nullopt : std::optional{found->second};
  }

  [[nodiscard]] auto SourceCount() const -> std::size_t { return sources_.size(); }
  [[nodiscard]] auto MetricCount() const -> std::size_t { return metrics_.size(); }

 private:
  void Register(std::string_view kind, std::string_view name, std::uint64_t id) {
    out_ << kind << '\t' << name << '\t' << id << '\n';
    out_.flush();
    if (!out_) {
      throw std::runtime_error(std::format("Catalog: Cannot write to {}", path_));
    }
  }

  std::string path_;
  std::unordered_map<std::string, std::uint64_t> sources_;
  std::unordered_map<std::string, std::uint16_t> metrics_;
  std::ofstream out_;
};

[[nodiscard]] inline auto EncodeChunk(std::span<const Sample> samples) -> Chunk {
  auto chunk = Chunk{};
  const auto start = samples.front().time;
  const auto count = static_cast<std::uint16_t>(samples.size());
  std::memcpy(chunk.data(), &start, sizeof(start));
  std::memcpy(chunk.data() + sizeof(start), &count, sizeof(count));

  auto offset = kSampleHeaderSize;
  for (const auto& sample : samples) {
    const auto delta = static_cast<std::uint32_t>(sample.time - start);
    std::memcpy(chunk.data() + offset, &delta, sizeof(delta));
    std::memcpy(chunk.data() + offset + sizeof(delta), &sample.value, sizeof(sample.value));
    offset += kSampleSize;
  }

  return chunk;
}

[[nodiscard]] inline auto DecodeChunk(const Chunk& chunk) -> std::vector<Sample> {
  auto start = Timestamp{};
  auto count = std::uint16_t{};
  std::memcpy(&start, chunk.data(), sizeof(start));
  std::memcpy(&count, chunk.data() + sizeof(start), sizeof(count));

  auto samples = std::vector<Sample>{};
  samples.reserve(count);
  auto offset = kSampleHeaderSize;
  for (auto i = 0U; i < count; ++i) {
    auto delta = std::uint32_t{};
    auto value = double{};
    std::memcpy(&delta, chunk.data() + offset, sizeof(delta));
    std::memcpy(&value, chunk.data() + offset + sizeof(delta), sizeof(value));
    samples.push_back(Sample{start + delta, value});
    offset += kSampleSize;
  }

  return samples;
}

// The store directory holds the engine's two files and the catalog:
//
//   data.slab    the slab file (sparse, 6 GiB)
//   data.info    the disk slabinfo area
//   catalog.tsv  names to ids
class FinanceStore {
 public:
  explicit FinanceStore(const std::string& directory)
      : directory_{EnsureDirectory(directory)},
        slab_file_{OpenSlabFile(directory_ + "/data.slab")},
        engine_{&slab_file_, directory_ + "/data.info"},
        catalog_{directory_ + "/catalog.tsv"} {}

  FinanceStore(const FinanceStore&) = delete;
  FinanceStore& operator=(const FinanceStore&) = delete;

  ~FinanceStore() { Flush(); }

  // Samples are appended per series, in time order; a chunk goes to the engine
  // as soon as it is full, and Flush() sends the partial ones.
  void Put(std::string_view symbol, std::string_view metric, Timestamp time, double value) {
    const auto source_id = catalog_.SourceId(symbol);
    const auto metric_id = catalog_.MetricId(metric);
    auto& pending = pending_[KeyOf(source_id, metric_id)];

    if (pending.count == 0) {
      pending.source_id = source_id;
      pending.metric_id = metric_id;
    } else {
      if (time < pending.samples[pending.count - 1].time) {
        throw std::runtime_error(
            std::format("FinanceStore: {}.{} went back in time at {} (after {})", symbol, metric,
                        time, pending.samples[pending.count - 1].time));
      }
      if (time - pending.samples[0].time > kSampleDeltaLimit) {
        throw std::runtime_error(
            std::format("FinanceStore: {}.{} has samples more than {} seconds apart", symbol,
                        metric, kSampleDeltaLimit));
      }
    }

    pending.samples[pending.count++] = Sample{time, value};
    if (pending.count == kSamplesPerChunk) {
      WritePending(pending);
    }
  }

  // Everything the store holds, out to the slab file: the partial chunks, and
  // the engine's partially filled slabs behind them.
  void Flush() {
    for (auto& [key, pending] : pending_) {
      WritePending(pending);
    }

    engine_.Flush();
  }

  // Samples of the series with `from <= time <= to`. Chunks go through whole —
  // the engine hands back the one before the range as well, since nothing there
  // knows how long a chunk runs.
  [[nodiscard]] auto Get(std::string_view symbol, std::string_view metric, Timestamp from,
                         Timestamp to) -> std::vector<Sample> {
    Flush();

    const auto source_id = catalog_.FindSource(symbol);
    const auto metric_id = catalog_.FindMetric(metric);
    if (!source_id.has_value() || !metric_id.has_value()) {
      return {};
    }

    auto samples = std::vector<Sample>{};
    for (const auto& chunk : engine_.Get(*source_id, *metric_id, from, to)) {
      for (const auto& sample : DecodeChunk(chunk)) {
        if (sample.time >= from && sample.time <= to) {
          samples.push_back(sample);
        }
      }
    }

    return samples;
  }

  [[nodiscard]] auto Sources() const -> std::size_t { return catalog_.SourceCount(); }
  [[nodiscard]] auto Metrics() const -> std::size_t { return catalog_.MetricCount(); }

 private:
  struct Pending {
    std::array<Sample, kSamplesPerChunk> samples{};
    std::uint32_t count = 0;
    std::uint64_t source_id = 0;
    std::uint16_t metric_id = 0;
  };

  [[nodiscard]] static auto KeyOf(std::uint64_t source_id, std::uint16_t metric_id)
      -> std::uint64_t {
    return (source_id << 16U) | metric_id;
  }

  // The store directory holds everything; it springs into being on first use.
  [[nodiscard]] static auto EnsureDirectory(const std::string& path) -> std::string {
    auto error = std::error_code{};
    std::filesystem::create_directories(path, error);
    if (error) {
      throw std::runtime_error(
          std::format("FinanceStore: Cannot create {}: {}", path, error.message()));
    }

    return path;
  }

  [[nodiscard]] static auto OpenSlabFile(const std::string& path) -> SlabFile {
    return std::filesystem::exists(path) ? SlabFile::Open(path) : SlabFile::Create(path);
  }

  void WritePending(Pending& pending) {
    if (pending.count == 0) {
      return;
    }

    auto chunk = EncodeChunk(std::span<const Sample>{pending.samples}.first(pending.count));
    engine_.Put(pending.source_id, pending.metric_id, chunk);
    pending.count = 0;
  }

  std::string directory_;
  SlabFile slab_file_;
  TreeSeries engine_;
  Catalog catalog_;
  std::unordered_map<std::uint64_t, Pending> pending_;
};
