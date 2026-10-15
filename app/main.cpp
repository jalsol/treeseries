// fin_ts: a small command line over FinanceStore, for putting financial time
// series in and getting them back out.
//
//   fin_ts gen    --out FILE [--symbols N] [--metrics N] [--samples N]
//                 [--interval SEC] [--seed N]
//   fin_ts ingest --store DIR FILE [FILE...]
//   fin_ts query  --store DIR --symbol S --metric M [--from T] [--to T]
//
// Input is CSV, one row per sample: timestamp, symbol, metric, value. A file
// whose header names several value columns (say open, high, low, close) is
// read as one metric per column instead. Timestamps are epoch seconds or
// "YYYY-MM-DD[ HH:MM:SS]" in UTC; output is the same shape, with ISO times.

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "app/finance_store.hpp"

namespace {

constexpr auto kBaseTime = Timestamp{1735689600};  // 2025-01-01T00:00:00Z

auto Trim(std::string_view text) -> std::string_view {
  while (!text.empty() && (text.front() == ' ' || text.front() == '\r')) {
    text.remove_prefix(1);
  }
  while (!text.empty() && (text.back() == ' ' || text.back() == '\r')) {
    text.remove_suffix(1);
  }
  return text;
}

auto Lower(std::string_view text) -> std::string {
  auto out = std::string{text};
  std::ranges::transform(out, out.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

auto SplitCsv(std::string_view line) -> std::vector<std::string_view> {
  auto fields = std::vector<std::string_view>{};
  while (true) {
    const auto comma = line.find(',');
    fields.push_back(Trim(line.substr(0, comma)));
    if (comma == std::string_view::npos) {
      return fields;
    }
    line.remove_prefix(comma + 1);
  }
}

// "1750000000", "2025-06-01", "2025-06-01 10:30:00" or the same with T and Z.
auto ParseTime(std::string_view text) -> Timestamp {
  text = Trim(text);
  while (!text.empty() && (text.back() == 'Z' || text.back() == 'z')) {
    text.remove_suffix(1);
  }

  if (!text.empty() && std::ranges::all_of(text, [](unsigned char c) { return std::isdigit(c); })) {
    auto epoch = Timestamp{};
    if (std::from_chars(text.data(), text.data() + text.size(), epoch).ec != std::errc{}) {
      throw std::runtime_error(std::format("Cannot read time '{}'", text));
    }
    return epoch;
  }

  auto clean = std::string{text};
  std::ranges::replace(clean, 'T', ' ');
  auto seconds = std::chrono::sys_seconds{};
  auto stream = std::istringstream{clean};
  stream >> std::chrono::parse("%F %T", seconds);
  if (stream.fail()) {
    stream.clear();
    stream.seekg(0);
    stream >> std::chrono::parse("%F", seconds);
  }
  if (stream.fail()) {
    throw std::runtime_error(std::format("Cannot read time '{}'", text));
  }

  return static_cast<Timestamp>(seconds.time_since_epoch().count());
}

auto FormatTime(Timestamp time) -> std::string {
  return std::format("{:%F %T}", std::chrono::sys_seconds{std::chrono::seconds{time}});
}

auto ParseNumber(std::string_view text, std::string_view what) -> double {
  auto value = double{};
  if (std::from_chars(text.data(), text.data() + text.size(), value).ec != std::errc{}) {
    throw std::runtime_error(std::format("Cannot read {} '{}'", what, text));
  }
  return value;
}

auto ParseCount(std::string_view text, std::string_view what) -> std::uint64_t {
  auto value = std::uint64_t{};
  if (std::from_chars(text.data(), text.data() + text.size(), value).ec != std::errc{}) {
    throw std::runtime_error(std::format("Cannot read {} '{}'", what, text));
  }
  return value;
}

// Column names financial CSVs show up with.
auto IsTimeName(const std::string& name) -> bool {
  return name == "timestamp" || name == "time" || name == "date" || name == "datetime" ||
         name == "ts";
}

auto IsSymbolName(const std::string& name) -> bool {
  return name == "symbol" || name == "ticker" || name == "source";
}

auto Ingest(FinanceStore& store, const std::string& path) -> std::uint64_t {
  auto in = std::ifstream{path};
  if (!in) {
    throw std::runtime_error(std::format("Cannot open {}", path));
  }

  auto line = std::string{};
  if (!std::getline(in, line)) {
    return 0;
  }

  const auto header = SplitCsv(line);
  auto time_at = std::optional<std::size_t>{};
  auto symbol_at = std::optional<std::size_t>{};
  auto metric_at = std::optional<std::size_t>{};
  auto value_at = std::optional<std::size_t>{};
  auto columns = std::vector<std::pair<std::size_t, std::string>>{};  // wide-format metrics
  for (auto i = 0uz; i < header.size(); ++i) {
    const auto name = Lower(header[i]);
    if (IsTimeName(name)) {
      time_at = i;
    } else if (IsSymbolName(name)) {
      symbol_at = i;
    } else if (name == "metric") {
      metric_at = i;
    } else if (name == "value") {
      value_at = i;
    } else {
      columns.emplace_back(i, name);
    }
  }

  if (!time_at.has_value() || !symbol_at.has_value()) {
    throw std::runtime_error(
        std::format("{}: the header needs a time and a symbol column, got '{}'", path, line));
  }
  const auto wide = !metric_at.has_value() || !value_at.has_value();
  if (wide && columns.empty()) {
    throw std::runtime_error(
        std::format("{}: no value columns (want metric,value or metrics)", path));
  }

  auto rows = std::uint64_t{};
  while (std::getline(in, line)) {
    if (Trim(line).empty() || line.front() == '#') {
      continue;
    }

    const auto fields = SplitCsv(line);
    if (fields.size() < header.size()) {
      throw std::runtime_error(std::format("{}: short row '{}'", path, line));
    }

    const auto time = ParseTime(fields[*time_at]);
    const auto symbol = fields[*symbol_at];
    if (wide) {
      for (const auto& [at, name] : columns) {
        if (!fields[at].empty()) {
          store.Put(symbol, name, time, ParseNumber(fields[at], "value"));
        }
      }
    } else {
      store.Put(symbol, fields[*metric_at], time, ParseNumber(fields[*value_at], "value"));
    }
    ++rows;
  }

  return rows;
}

// Eight symbols and five metrics of random walk, written time-major: every
// series still comes out in time order, which is what the store wants.
void Generate(const std::map<std::string, std::string>& options) {
  const auto out_path = options.contains("out") ? options.at("out") : std::string{"-"};
  const auto symbols = static_cast<std::uint32_t>(
      ParseCount(options.contains("symbols") ? options.at("symbols") : "8", "count"));
  const auto metrics = static_cast<std::uint32_t>(
      ParseCount(options.contains("metrics") ? options.at("metrics") : "5", "count"));
  const auto samples =
      ParseCount(options.contains("samples") ? options.at("samples") : "500", "count");
  const auto interval =
      ParseCount(options.contains("interval") ? options.at("interval") : "60", "interval");
  const auto seed = ParseCount(options.contains("seed") ? options.at("seed") : "1", "seed");

  constexpr auto kMetricNames = std::array{"close", "open", "high", "low", "volume"};
  if (metrics == 0 || symbols == 0 || samples == 0 || interval == 0) {
    throw std::runtime_error("gen: counts and interval must be positive");
  }

  auto output = options.contains("out") ? std::optional<std::ofstream>{std::ofstream{out_path}}
                                        : std::nullopt;
  if (output.has_value() && !*output) {
    throw std::runtime_error(std::format("Cannot write {}", out_path));
  }
  auto& out = output.has_value() ? *output : std::cout;

  auto prices = std::vector<double>(static_cast<std::size_t>(symbols) * metrics);
  for (auto s = 0U; s < symbols; ++s) {
    for (auto m = 0U; m < metrics; ++m) {
      prices[static_cast<std::size_t>(s) * metrics + m] =
          100.0 + static_cast<double>((s * 7U + m * 13U + seed) % 50U);
    }
  }
  auto random = std::mt19937{static_cast<std::uint32_t>(seed)};
  auto step = std::normal_distribution<double>{0.0, 0.002};

  out << "timestamp,symbol,metric,value\n";
  for (auto i = 0uz; i < samples; ++i) {
    const auto time = kBaseTime + static_cast<Timestamp>(i) * interval;
    for (auto s = 0U; s < symbols; ++s) {
      for (auto m = 0U; m < metrics; ++m) {
        auto& price = prices[static_cast<std::size_t>(s) * metrics + m];
        price = std::max(0.01, price * (1.0 + step(random)));
        const auto symbol = std::format("SYM{:04}", s);
        const auto metric =
            m < kMetricNames.size() ? std::string{kMetricNames[m]} : std::format("metric{}", m);
        out << std::format("{},{},{},{}\n", FormatTime(time), symbol, metric, price);
      }
    }
  }
}

void Query(const std::map<std::string, std::string>& options) {
  const auto& store_path = options.at("store");
  const auto symbol = options.at("symbol");
  const auto metric = options.at("metric");
  const auto from = options.contains("from") ? ParseTime(options.at("from")) : Timestamp{};
  const auto to = options.contains("to") ? ParseTime(options.at("to")) : Timestamp{UINT64_MAX};

  auto store = FinanceStore{store_path};
  const auto samples = store.Get(symbol, metric, from, to);

  std::cout << "timestamp,symbol,metric,value\n";
  for (const auto& sample : samples) {
    std::cout << std::format("{},{},{},{}\n", FormatTime(sample.time), symbol, metric,
                             sample.value);
  }

  std::cerr << std::format("{} samples of {}.{}\n", samples.size(), symbol, metric);
}

auto Usage(const char* program) -> std::string {
  return std::format(
      "usage: {} <command> [options]\n"
      "\n"
      "  gen    --out FILE [--symbols N] [--metrics N] [--samples N] [--interval SEC]\n"
      "         [--seed N]                    random walk series as CSV (stdout without --out)\n"
      "  ingest --store DIR FILE [FILE...]    add CSV samples to the store\n"
      "  query  --store DIR --symbol S --metric M [--from T] [--to T]\n"
      "                                       print the series' samples as CSV\n"
      "\n"
      "Time is epoch seconds or 'YYYY-MM-DD[ HH:MM:SS]' UTC.\n",
      program);
}

}  // namespace

auto main(int argc, char** argv) -> int {
  const auto args = std::span{argv, static_cast<std::size_t>(argc)};

  auto options = std::map<std::string, std::string>{};
  auto positional = std::vector<std::string>{};
  for (auto i = 1uz; i < args.size(); ++i) {
    const auto arg = std::string_view{args[i]};
    if (arg.starts_with("--")) {
      const auto name = std::string{arg.substr(2)};
      if (i + 1 < args.size() && !std::string_view{args[i + 1]}.starts_with("--")) {
        options.emplace(name, args[++i]);
      } else {
        options.emplace(name, "true");
      }
    } else {
      positional.emplace_back(arg);
    }
  }

  try {
    if (positional.empty() || positional.front() == "help" || options.contains("help")) {
      std::cerr << Usage(args[0]);
      return positional.empty() && !options.contains("help") ? 1 : 0;
    }

    const auto& command = positional.front();
    if (command == "gen") {
      Generate(options);
    } else if (command == "ingest") {
      if (!options.contains("store") || positional.size() < 2) {
        throw std::runtime_error("ingest needs --store DIR and at least one FILE");
      }
      auto store = FinanceStore{options.at("store")};
      auto rows = std::uint64_t{};
      for (auto file = positional.begin() + 1; file != positional.end(); ++file) {
        const auto taken = Ingest(store, *file);
        rows += taken;
        std::cerr << std::format("{}: {} rows\n", *file, taken);
      }
      store.Flush();
      std::cerr << std::format("{} rows, {} symbols, {} metrics\n", rows, store.Sources(),
                               store.Metrics());
    } else if (command == "query") {
      for (const auto* needed : {"store", "symbol", "metric"}) {
        if (!options.contains(needed)) {
          throw std::runtime_error(std::format("query needs --{}", needed));
        }
      }
      Query(options);
    } else {
      throw std::runtime_error(std::format("unknown command '{}'", command));
    }
  } catch (const std::exception& error) {
    std::cerr << std::format("fin_ts: {}\n", error.what());
    return 1;
  }

  return 0;
}
