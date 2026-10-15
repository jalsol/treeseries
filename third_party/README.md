# third_party

## masstree-beta

Vendored from <https://github.com/kohler/masstree-beta> at `11198427` (`master`),
unmodified. It is the B+ tree the tree index is built on (§3.3.2): masstree keys
are byte strings compared with `memcmp`, and its values are plain pointers.

`masstree/config.h` is generated upstream, not committed there: after updating
the checkout, run `sh bootstrap.sh && sh configure` in `masstree/` — the same
steps its CMake build performs — and keep the result (their `.gitignore` hides
it, so `git add -f`). `BUILD.bazel` compiles the support library the wrapper
needs; the tree itself is header-only.

`masstree_globals.cpp` is ours: masstree declares its mrcu/epoch globals as
`extern` and leaves defining them to the program.

## masstree_wrapper.hpp

Vendored from <https://github.com/rotaki/masstree-wrapper> (`master`, MIT),
unmodified. It wraps the tree with insert/get/update/remove and range scans, and
needs `GLOBAL_VALUE_DEFINE` in exactly one TU, which `masstree_globals.cpp` is.

Two things about it are worked around in `treeseries/tree_index.hpp` rather than
by patching the vendored file:

- `get_version_value()` calls `node->full_version_value()`, which this masstree
  revision only has on `leaf`, not on `node_base`. Call
  `leaf->full_version_value()` directly.
- `rscan()` tests `lkey` for null but builds its start key from `rkey`. Fine as
  long as both bounds are non-null, which is how it is used here.

And one thing to know before writing a scan: masstree hands the callback only
the suffix past the current 8-byte ikey, not the key. Range decisions have to be
taken from the value, and the wrapper's own end-bound comparison is unusable for
keys longer than 8 bytes.
