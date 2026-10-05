# TreeSeries implementation plan

## Context

Goal: `Put`/`Get` over a DRAM slab buffer and an NVMe slab file with slab-granular migration
and crash recovery, plus two hand-built primitives the paper relies on: a bounded lock-free
MPMC queue (paper [2], atomic_queue) and an optimistic B+ tree (paper [29] = Masstree node
techniques).

- `SlabState`: `kMemFree, kMemFull, kMemEvict, kDiskFree, kDiskFull, kDiskEvict, kOpen`
  (`kOpen` = owned by a MetaHash cell, in no queue — needed for the "exactly one queue"
  invariant to be checkable).
- `SlabInfo` (flat `vector`, indexed by SlabId): `atomic<SlabState> state;
  atomic<bool> in_memory; atomic<uint32_t> generation; atomic<SlabId> disk_slab;`
  `generation` bumps on reuse; `disk_slab` is the file slot this memory slab was copied to.
  All atomic because Get reads them while a migrator thread writes.
- `SlabBuffer`: anonymous `mmap(MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE)` + `MADV_HUGEPAGE`,
  `slab(id) → span<byte, kSlabSize>`, munmap in dtor. `SlabFile` later gets the *same shape*
  (that's what makes migration a memcpy and Get tier-agnostic).
- Six queues (`info_queue.hpp`): `mem_free, mem_full, mem_evict, disk_free, disk_full,
  disk_evict`; entries are `SlabId`. M1–M3 use a throwaway mutex+deque behind the interface;
  M4 swaps in the real ring.
- MetaHash cell: `{mutex mtx; SlabId slab; uint32_t used; uint32_t gen;}` — the cell is the
  slab's single writer while open. Defaults: rows 256, cols 16384 (MetricID%rows, SourceID%cols).

## M1 — storage running in memory (1–1.5 days)

**Build order inside the milestone**

1. `common.hpp`: constants + `static_assert`s; `EncodeItem(span<byte,256>, metric, source,
   start, payload)`, `DecodeItem` returning a view, `EncodeKey`/`CompareKey` (BE, 18B).
2. `SlabBuffer` as above.
3. `SlabInfo` table + six queues + init (all memory slabs → `mem_free`, all future file slabs
   → `disk_free`) + a debug `CheckQueues()` helper that walks all six queues and asserts
   each slab appears exactly once and its state matches.
4. MetaHash; `Put(SourceId, MetricId, span<const byte, kChunkSize>)` = Alg. 1:
   - lock cell; if `cell.slab == kInvalidSlabId`: if `mem_free` empty → `mem_evict.TryPop()`
     (M1: should not happen; see below) else `mem_free.Pop()`; set `used = 0`,
     `gen = ++info[id].generation`, `state = kOpen`.
   - write item at `slab(used)`; `used++`.
   - index upsert: key `(source, metric, start)`, value `{slab, gen, max = max(max, start)}`.
     Do it under the cell lock — each entry is only ever touched by one cell, so the interim
     index needs no extra coordination.
   - if `used == 16`: `state = kMemFull`, `mem_full.Push(slab)`, `cell.slab = kInvalidSlabId`.
   - unlock.
   Edge (decide now, revisit M2): `mem_evict` empty and `mem_free` empty → the buffer is full;
   M1 has no migrator, so **block** (condition variable, woken by migration in M2); do not hold
   the cell lock while blocked.
5. Index + `Get(SourceId, MetricId, t1, t2) → vector<SlabId>`:
   - interim index: `std::map<Key18, Entry>` + mutex, `Entry = {SlabId, gen, max}`.
   - query algorithm (this survives the later tree swap, so get it right):
     seek to the first entry ≥ `(series, t1)`; **step back one entry** (the predecessor may be
     a slab whose range straddles t1 — per-series slab ranges are disjoint and time-ordered,
     so one step is enough); scan forward while key prefix is that series and `min ≤ t2`,
     keeping entries with `max ≥ t1`.
   - for each hit: skip if `info[slab].generation != entry.gen` (reused slab, stale entry);
     else read `slab(id)` and return the SlabId. Get returns whole slabs — the caller scans
     items and decompresses; do not decode payloads in Get.
   Edge (decide now): index `Entry.max` is updated per Put under the cell lock; the map's
   `operator[]`/find handles insert-vs-extend.

**Why generation instead of deleting entries on reuse**: no erase path needed at all — reuse
bumps `generation` and stale entries are filtered at query time. Ceiling: stale entries
accumulate and are never pruned (documented; erase can be added later if the tree bloats).

## M2 — mmap slab file + migration (1 day)

1. `SlabFile`: `open(O_RDWR|O_CREAT)`, `ftruncate(24GB)`, `mmap(MAP_SHARED, PROT_READ|WRITE,
   ...)`, same `slab(id)` shape; munmap+close in dtor. Sparse is fine; `fallocate` is a knob.
2. Migrator thread + quiescence gate (below).
3. Migration batch (Alg. 2, single-threaded):
   - wake when `mem_full` size ≥ `k1 * mem_slab_count` (k1 = 1/16); batch size B (start 4096,
     tune in M6); require `disk_free` size ≥ B, else shrink batch / wait (no HDD in scope:
     file-full = backpressure, and `disk_evict` stays unused — say so in a comment).
   - drain ≤ B from `mem_full`; sort by SlabId; coalesce adjacent ids into runs.
   - per run: `memcpy` buffer→file mapping (`MADV_SEQUENTIAL` hint), then `msync(MS_SYNC)`.
     Pop one `disk_free` id per slab; record `info[mem_id].disk_slab = disk_id`;
     `info[mem_id].in_memory = true` (still true — buffer holds valid data until reuse).
   - then, and only then, make the batch reusable: `state = kMemEvict`, `mem_evict.Push`;
     `state = kDiskFull`, `disk_full.Push(disk_id)`.
   - **Durability order matters**: data msync → in-memory bookkeeping → M3's info-area record
     (fsync) → allow reuse. A crash between msync and the record means the data is simply not
     recoverable (slab silently dropped) — never the reverse (record without data).
4. Get resolves tiers via `SlabInfo`: `in_memory` → buffer (valid until reuse), else →
   `file.slab(info[id].disk_slab)`. Gen check still guards currency, so a reused slab's old
   entries can never resolve to the new occupant. A `kMemEvict` slab still reads correctly
   from the buffer.
5. **Quiescence gate** (simple, correct, coarse — mark as such): global
   `atomic<uint32_t> active_readers`; Get increments on entry and decrements on exit, holding
   the count across the whole slab read. The migrator enqueues slabs into `mem_evict` only
   when `active_readers == 0`. This guarantees no reader is mid-slab while a slab becomes
   reusable; cost is that migration waits for readers to drain (they're short). Per-thread
   epochs/hazard pointers are the upgrade if this ever shows in profiles.
6. Wake blocked Puts (M1 step 4) on the condition variable after `mem_evict` grows.

## M3 — disk slabinfo area + recovery (½–1 day)

1. Info area = one append-only file (`slabinfo.log`), opened at engine start.
2. Record layout: `magic u64 | epoch u64 | batch_len u32 | crc32 | entries[]`, where each
   entry is `mem_slab u32 | disk_slab u32 | series_count u16 | (key18, max u64, gen u32)[]`
   — i.e. per migrated slab: its file slot and all its index entries (≤16). Write with a
   single `pwrite` append, then `fsync` the file (after the data msync — M2 order).
3. Recovery (`Open` on an existing store): read records from the start, stop at the first
   bad magic/crc/length (torn tail is expected), and for each valid record:
   `info[mem_id] = {kDiskFull, in_memory=false, disk_slab, gen}`; `disk_full.Push(disk_id)`;
   reinsert index entries into a fresh tree. All buffer slabs → `mem_free`; `mem_evict`,
   `mem_full` empty; unaffected disk slabs stay `disk_free`. `mem_free` is *not* reconstructed
   from disk — in-buffer data is intentionally lost without a WAL (§3.5, out of scope).
4. Epoch: a fresh store writes a new epoch header; recovery only accepts records of the
   current epoch, so a stale file never resurrects an old run. Deleting/recreating on a fresh
   start is the lazy equivalent — pick one and comment it.

## M4 — lock-free MPMC queue (1 day)

Vyukov bounded ring, one template header (`mpmc_queue.hpp`), `T = SlabId`:

- `Cell { atomic<size_t> seq; T data; }` `alignas(64)`; `array<Cell, N>` (N = power of two);
  `head_`/`tail_` `atomic<size_t>` each on their own cache line.
- `TryPush`: `pos = tail.load(relaxed)`; loop: `cell = cells[pos & mask]`;
  `s = cell.seq.load(acquire)`; if `s == pos` → `tail.CAS(pos, pos+1, relaxed)` and on
  success store data, `cell.seq.store(pos+1, release)`; if `s < pos` → full, return false;
  else refresh `pos`. `Pop` symmetric (`s == pos+1` valid, store `pos+N` on release).
  `Push`/`Pop` = loop on Try with `cpu_relax`/`yield`; `Empty()` = `head == tail` snapshot.
- Capacity: next pow2 above each state's max live slabs — 2M for mem queues, 8M for disk
  queues (6M rounded). Memory: padded cells cost 64B → ≈1.9GB total. Knobs, in order:
  drop padding (16B cells → ≈480MB, false sharing) or shrink rings + blocking push.
- Swap: delete the interim deque implementation; the six-queue API is unchanged, so the
  engine and migrator don't move.

## M5 — lock-free B+ tree (2+ days, riskiest)

Single tree, key = 18-byte BE composite + `Entry {SlabId slab; uint32_t gen; uint64_t max;}`.
Insert-only + in-place value update; **no erase, no reclamation** (nodes come from a bump
arena and are never freed) — this is what makes it tractable. Ceiling: stale entries from
reuse are filtered by gen but never removed; nodes grow monotonically. Erase/GC later.

**Node layout** (256B, one cache line set; 512B → 13 slots is the tuning knob):

| field | size | notes |
|---|---|---|
| `version` | 8B | `[obsolete:1][locked:1][counter:62]` |
| `count` | 2B | slots in use |
| `next` | 8B | B-link right sibling (tagged leaf/internal in low bit) |
| `keys[6]` | 6×18B | sorted, `memcmp` order |
| `vals[6]` | 6×16B | `{slab, gen, max}`, only in leaves |

**Version protocol** (Masstree-style): readers `v1 = version.load(acquire)`; if locked → retry;
read; `v2 = version.load(acquire)`; accept iff `v1 == v2 && !(v1 & locked)`. Writers lock via
CAS `v → v|locked`, mutate, store `v+1` with release. Note for TSan: readers copy slot bytes
with plain loads validated by the version word — standard optimistic concurrency, but TSan may
flag the slot races. Either wrap slot words in `std::atomic_ref` (relaxed) or suppress those
functions; decide when the first TSan run complains, not before.

**Algorithms**
- *Lookup(series, t)*: descend from root. At an internal node: optimistic read, binary search
  (or linear over 6) for the last key ≤ t; if t is greater than the node's last key, follow
  `next` (B-link) rather than descending into a stale child. At the leaf: find exact/position;
  if past the last key, follow `next`. Retry the whole descent on version failure.
- *Upsert(series, t, {slab, gen, max})* (per Put, under the cell lock): descend as above.
  At the leaf: lock version; if the key exists → update `max` (monotonic `std::max`) and
  unlock-bump; else insert into the sorted slot array (shift the tail). If the leaf is full:
  allocate a sibling, move the upper half, set `next` (release), then walk back up the
  recorded descent path locking each parent: insert separator + child pointer, splitting
  parents the same way; a root split allocates a new root and bumps the height. Between the
  leaf split and the parent update, readers reach the old leaf, miss the key, and follow
  `next` — never lost. This "descend recording the path, ascend locked" is simpler than lock
  coupling and correct with B-link.
- *RangeQuery(series, t1, t2)*: same as M1's query, but each read is version-validated; on a
  version change, re-descend. Return `vector<Entry>`, filter gen in the engine.
- Writers to one entry are serialized by the MetaHash cell, so the in-place `max` update is a
  single writer; readers see it via the node version bump.

## Deferred / knobs

| Deferred | Trigger |
|---|---|
| O_DIRECT `pwritev` path | mmap writeback bandwidth disappoints in M6 |
| Per-thread epochs instead of `active_readers` gate | migration stalls show in profiles |
| Multi-threaded migration pool (Alg. 2 `pool.Submit`) | single-threaded migrator correct first |
| Tree erase + node reclamation; stale-entry pruning | tree size/replay time grows |
| Info-area snapshot compaction | append replay time shows |
| 512B nodes, queue padding trade-off | M6 numbers |
| Masstree trie layer | only if keys become variable-length |
| WAL, HDD tier, metadata index | other paper sections, other repos |
