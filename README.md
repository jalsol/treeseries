how the fuck did this paper get published? if anyone wants to implement
database papers, maybe look into those from TU Munich. They are goated.

# treeseries

C++26 implementation of TreeSeries from *PromTCS: An Efficient
Two-Dimensional Metadata Management and Tailored Hybrid Storage Architecture
for Performance Monitoring Time Series* ([FAST '27][paper]).

At first, this implementation was done manually (AI only involved in planning
and enforcing tests). However, once I dug deeper into the finer details, I
basically gave up and use AI to accelerate the completion of this project.

Some stuff are stripped because I'm not interested (e.g. HDD support). Some
stuff are simplified because the authors were straight up stupid.

## Build

```sh
bazel test //...                 # build + test
bazel test --config=ci //...     # release + -Werror, what CI runs
bazel test --config=asan //...   # also: ubsan, release, native, gcc
```

Requires a compiler with C++26 support (gcc 16+). Bazel version is pinned in
`.bazelversion`.

## Application

`app/fin_ts` drives the engine with financial time series: a series is a
`(symbol, metric)` pair such as `AAPL.close`, its samples are doubles, and its
chunks are packed into the engine's 246 bytes. Names map to the engine's ids
through a catalog file — what PromTCS' metadata index would hold — and both
commands reopen the store from the disk slabinfo area, so a `query` sees what a
previous `ingest` flushed before it exited.

```sh
bazel run //app:fin_ts -- gen --out /tmp/demo.csv --symbols 8 --metrics 5 --samples 500
bazel run //app:fin_ts -- ingest --store /tmp/demo /tmp/demo.csv
bazel run //app:fin_ts -- query --store /tmp/demo --symbol SYM0000 --metric close \
  --from 2025-01-01 --to "2025-01-01 02:00:00"
```

## Layout

```
treeseries/         library sources; includes are "treeseries/foo.hpp"
treeseries/tests/   catch2 tests, one cc_test per file
app/                financial time series command line over the library
third_party/        masstree-beta and its wrapper; see third_party/README.md
```

[paper]: https://www.usenix.org/system/files/conference/fast27/fast27spring_prepub-kong.pdf
