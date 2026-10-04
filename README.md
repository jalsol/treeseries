# treeseries

C++26 implementation of TreeSeries from *PromTCS: An Efficient
Two-Dimensional Metadata Management and Tailored Hybrid Storage Architecture
for Performance Monitoring Time Series* ([FAST '27][paper]).

## Build

```sh
bazel test //...                 # build + test
bazel test --config=ci //...     # release + -Werror, what CI runs
bazel test --config=asan //...   # also: ubsan, release, native, gcc
```

Requires a compiler with C++26 support (gcc 16+). Bazel version is pinned in
`.bazelversion`.

## Layout

```
treeseries/         library sources; includes are "treeseries/foo.hpp"
treeseries/tests/   catch2 tests, one cc_test per file
```

[paper]: https://www.usenix.org/system/files/conference/fast27/fast27spring_prepub-kong.pdf
