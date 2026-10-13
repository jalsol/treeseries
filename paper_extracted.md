---
PromTCS paper but TreeSeries part extracted 
---

### 3.2 Two-Dimensional Metadata Management

In this section, we present a two-dimensional metadata model and indexes to reduce overhead and improve scalability.

#### 3.2.1 Two-Dimensional Metadata Model

We propose a new metadata model to reduce metadata redundancy across metrics and data sources, as follows:

**Definition 1** (Two-Dimensional Metadata Model). *Let  $l$  denote the metadata of a time series, represented as a set of labels. We partition  $l$  into two disjoint subsets, the metric label set  $l_m$  and the source label set  $l_s$ , such that*

$$l = l_m \cup l_s, \quad l_m \cap l_s = \emptyset. \quad (1)$$

*We assign a unique MetricID to each distinct  $l_m$  and a unique SourceID to each distinct  $l_s$ , i.e.,*

$$f_m(l_m) = \text{MetricID}, \quad f_s(l_s) = \text{SourceID}, \quad (2)$$

*where  $f_m$  and  $f_s$  are injective. Accordingly, each time series is uniquely identified by the pair (MetricID, SourceID).*

**Example 1.** In Fig. 2, the metadata of a time series is partitioned into a metric label set  $l_m$  (e.g.,  $\{\langle \text{name} = \text{cpu\_usage} \rangle\}$ ) and a source label set  $l_s$  (e.g.,  $\{\langle \text{hostname} = \text{host\_0} \rangle, \dots\}$ ), mapped to a MetricID (*mid*) and a SourceID (*sid*) that uniquely identify the time series as (*mid*, *sid*).

This two-dimensional decomposition is not arbitrary. Performance-monitoring metadata naturally consists of metric and source dimensions: each time series corresponds to a metric (e.g., *cpu\_usage*) on a source (e.g., a server). Since metrics are shared across sources and each source produces multiple metrics, indexing these dimensions separately exploits their inherent reuse, reducing redundancy and indexing overhead while preserving full metadata reconstruction.

#### 3.2.2 Metadata Index

We design efficient and scalable metadata indexes based on the following three data structures:

- **Thmap** [14]. It is a concurrent hash-trie that maps keys to 32-bit *MurmurHash3* [19] hashes and organizes them as a trie for fast lookups. It provides lock-free reads via atomic CAS and uses fine-grained node locks for updates.
- **MergeSet.** It is a simplified single-level LSM-tree in VictoriaMetrics [18] that encodes each key as a concatenation of a tag and a posting ID, so posting lists emerge naturally from the maintained key order.
- **Flattened Array.** It stores entries sequentially in a compact layout, achieving fast address-based access.

Our evaluation shows that ① Thmap outperforms MergeSet on reads and writes, whereas MergeSet has lower memory overhead. ② A flattened array provides better read performance and storage efficiency than Thmap due to direct addressing and no load factor, but it has slower insertions.

**Backward index.** Every read and write must first resolve labels to IDs, so the backward index demands fast, concurrent lookups. We implement the backward index with two Thmaps, the *metric backward index* and the *source backward index*, whose lock-free reads and fine-grained node-level locking suit this high-frequency, multi-threaded access pattern. The former maps metric labels to *MetricIDs*, while the latter maps source labels to *SourceIDs* (see Fig. 6).

**Forward index.** The forward index maps (*MetricID*, *SourceID*) pairs to *TSObjects*. Queries dominate this index (updates occur only when new time series are registered), so

![Flowchart: Architecture of forward and backward index and query process for time series.](f9a14fbfecbd7d059226cc93677d721b_img.jpg)

Labels of Time Series  
< name=cpu\_usage >  
① Split labels  
< hostname =  
host\_0 >  
...  
< arch = x86 >  
Backward Index  
...  
...  
...  
...  
...  
...  
...  
...  
Label Patterns of Query  
① Get MetricID  
< name =  
cpu\_usage >  
< hostname  
= host\_0 >  
② Get IDs  
①  
①  
MemSeries Object  
Metadata  
labels  
Samples  
chunk  
Forward Index  
③  
②  
③ Located Memseries  
② Located Memseries  
MetricID  
SourceID  
① Get SourceID  
Inverted Index  
--> GetOrCreateID -> GetbyID ..... QueryInvertedIndex  
Metric Label (Index)  
Source Label (Index)

A block diagram illustrating the relationship between time series labels, a backward index, a forward index, and an inverted index, detailing the steps to retrieve source labels and metric IDs.

Flowchart: Architecture of forward and backward index and query process for time series.

Figure 6: Operations on metadata indexes.

![Flowchart: Comparison between Forward Index and Inverted Index structures.](e394c2b5c61344f6a12397f430086072_img.jpg)

Thmap Array  
MetricID (m1,m2) Array  
MetricID [1,m]  
SourceID [1,s]  
Flattened Array (version = n)  
...  
...  
SourceID (s1,s2)  
Thmap  
MetricID [1,m]  
SourceID [1,s]  
Flattened Array (version = n+1)  
MemberSeries Object  
Forward Index  
Label Key  
Thmap  
Label Value  
MergSet  
Label Value  
Thmap  
...  
...  
...  
...  
...  
...  
Posting Lists (of SourceID)  
Inverted Index

A diagram contrasting a Forward Index, which maps MetricID and SourceID arrays to a MemberSeries Object, with an Inverted Index, which maps Label Value and Thmap to Posting Lists of SourceID.

Flowchart: Comparison between Forward Index and Inverted Index structures.

Figure 7: Forward index and inverted index.

read performance and storage density are the primary concerns. We therefore split it into a read-optimized *flattened array* for the bulk of entries, which supports direct address-based access without hash-table overhead, and a *Thmap array* that buffers newly added entries with efficient insertion. Periodically, the Thmap entries are merged into the flattened array to reclaim space. As shown in Fig. 7, Thmap array stores entries of newly added time series, *i.e.*,  $MetricID \in (m1, m2]$ ,  $SourceID \in (s1, s2]$ , while the flattened array with version number  $n$  stores entries of historical time series, *i.e.*,  $MetricID \in [1, m1]$ ,  $SourceID \in [1, s1]$ . These are merged into a flattened array with version number  $n + 1$ , *i.e.*,  $MetricID \in [1, m2]$ ,  $SourceID \in [1, s2]$ .

**Inverted index.** It is a two-level inverted index that maps each source label to a posting list, *i.e.*, a sorted list of SourceIDs whose sources contain that label (see Fig. 7). For example, the posting list for  $\langle hostname = server\_1 \rangle$  is  $[s1, s3, s7]$ , meaning these three sources share this hostname. Since  $N_s \ll N$ , storing SourceIDs greatly shrinks posting lists. In the first level, we use a Thmap for label keys, ensuring performance with minimal memory usage given the small cardinality of label keys. Although the number of label values scales with data sources and can be large in practice, only a subset of labels is queried with significant frequency (*e.g.*, *hostname* in TSBS [15]). Thus, in the second level, we use Thmap for hot label values and MergeSet for cold ones. Newly written label values are always inserted into Thmap first for insertion performance. We design a cold-hot awareness mechanism that monitors the access frequency of each label-key leaf node and dynamically migrates the label-value indexes between Thmap and MergeSet according to their access frequency.

#### 3.2.3 Metadata indexing interface and workflow

Locating the *TSObject* via the metadata index is essential for both reads and writes. We next describe the interfaces and walk through a concrete example using the data in Fig. 6.

- **WriteIndex(labels)**  $\rightarrow$  *TSObject*. Following the red arrows, labels are split into metric labels ( $\langle name = cpu\_usage \rangle$ ) and source labels ( $\langle hostname = host\_0 \rangle$ , ...,  $\langle arch = x64 \rangle$ ), mapped to a MetricID and SourceID via the backward indexes, and combined to locate the *TSObject* in the forward index. For new time series, PromTCS assigns new IDs and updates the indexes.
- **ReadIndex(label patterns)**  $\rightarrow$  *vector*  $\langle TSObject \rangle$ . Following the blue arrows, label patterns are split into metric and source parts ( $\langle name = cpu\_usage \rangle$  and  $\langle hostname = host\_0 \rangle$ ). The backward index resolves metric labels to MetricIDs. The inverted index retrieves posting lists of SourceIDs for source labels and intersects them. The resulting MetricIDs and SourceIDs are used to retrieve all matching *TSObjects* from the forward index.

#### 3.2.4 Metadata Storage Cost Study

We analyze PromTCS’s metadata cost and compare it with Prometheus tsdb [11] and TimeUnion [37] (group model). Detailed derivations are provided in the technical report [12]. **Metadata cost model.** Let  $N_s, N_m, N$  be the number of data sources, metrics, and time series, respectively, and  $S_p, S_l, S_h$  be the average size per posting list entry, per label, and per single hash entry, respectively, and  $T$  be the average number of labels per time series. Then the metadata storage cost for PromTCS, denoted as  $Cost_{TCS}$ :

$$Cost_{TCS} = N_m \cdot S_l + (N + N_m + N_s) \cdot S_h + N_s \cdot (T - 1) \cdot (S_l + S_p)$$

**Comparison.** We summarize the corresponding cost for both Prometheus tsdb [11] and TimeUnion [37] in Tab. 2, where  $T_u$  denotes the average number of unique labels per group. From the cost formulas above, PromTCS compares favorably to the baselines. Let  $Cost_{TU}$  and  $Cost_{tsdb}$  be the costs for TimeUnion [37] and Prometheus tsdb [11], respectively:

$$\begin{cases} Cost_{TCS} \leq Cost_{tsdb} & \text{if } N_m \geq 2 \text{ and } N_s \geq 2 \\ Cost_{TCS} \leq Cost_{TU} & \end{cases}$$

PromTCS often yields lower overhead (*e.g.*, DevOps case in TSBS [15]:  $N_m = 100$ ,  $N_s \gg 2$ ).

### 3.3 TreeSeries

In this section, we introduce TreeSeries, a compaction-free time series storage engine designed for multi-core CPUs and NVMe SSDs (see Fig. 8). It serves as a write buffer on NVMe SSDs to mitigate WA and deliver superior performance.

#### 3.3.1 Slab Data Management

The basic unit for managing data in TreeSeries is the slab, which is a contiguous space in memory or SSDs. The size of each slab is 4 KB, the smallest read/write unit in SSDs [26].

**Slab structure.** As shown in Fig. 9, each slab is evenly divided into multiple items. Each item consists of a *MetricID* (2 bytes), a *SourceID* (8 bytes), and a *chunk*, all of which are from a time series. Since queries need to decompress chunks to locate samples in the given time range, a larger chunk size can degrade query performance. Conversely, a smaller chunk size can degrade compression efficiency. To balance these factors, we set the item size to 256 bytes, *i.e.*, each chunk is

Table 2: Metadata Storage Costs

| TSDB      | Storage Cost                                                                               |
|-----------|--------------------------------------------------------------------------------------------|
| tsdb [11] | $N \cdot (T \cdot S_l + 2S_h + T \cdot S_p)$                                               |
| TU [37]   | $(N_s \cdot (T - 1) + N) \cdot S_l + (2N + N_s) \cdot S_h + (N_s \cdot T_u + N) \cdot S_p$ |
| TCS       | $N_m \cdot S_l + (N + N_m + N_s) \cdot S_h + N_s \cdot (T - 1) \cdot (S_l + S_p)$          |

![Flowchart: Architecture and data flow of TreeSeries API using Slab Buffer and Slabinfo Queue.](a7d78d22e465dea388b31d0739f9d0cd_img.jpg)

TreeSeries API  
① locate writeable slab  
① Search Tree Index  
MetaHash  
Tree Index  
③ update Tree Index  
①  
② write to slab  
Slab Buffer  
Mem  
Disk  
Slabinfo Queue  
② read slab  
Free  
Full  
Evict  
migrate  
②  
Mem  
NVMe SSD  
write disk for Slabinfo  
Disk Slabinfo Area  
Slab File

A system diagram showing the interaction between the TreeSeries API, MetaHash, Slabinfo Queue, Slab Buffer, Tree Index, and Slab File, with numbered steps indicating operations like locating slabs, reading, updating, and migrating.

Flowchart: Architecture and data flow of TreeSeries API using Slab Buffer and Slabinfo Queue.

Figure 8: The design of TreeSeries.

![Technical illustration: Data structure mapping an item to chunks with timestamps and IDs.](f6d72d7c790e7f585532140f3971639a_img.jpg)

MetricID  
SourceID  
Chunk  
StartTime (in Chunk)  
Item (256 bytes)  
131  
270  
chunk₁  
2025-01-01T19:00:00Z  
MetricID (2 bytes)  
31  
1270  
chunk₂  
2025-01-01T14:00:00Z  
TS  
SourceID (8 bytes)  
131  
270  
chunk₃  
2025-01-01T19:30:00Z  
TS  
Chunk (246 bytes)  
StartTime (8 bytes)  
131  
270  
chunk₄  
2025-01-01T20:00:00Z  
...  
31  
1270  
chunk₅  
2025-01-01T14:30:00Z

A diagram showing an Item broken down into MetricID, SourceID, Chunk, and StartTime, which maps to a table of chunks with corresponding IDs, chunk numbers, and timestamps, with red and blue arrows indicating TS.

Technical illustration: Data structure mapping an item to chunks with timestamps and IDs.

Figure 9: Illustration of slab structure and relaxed ordering.

246 bytes (the extensive results for other item/chunk sizes can be found in the technical report [12]).

**Slab buffer.** A substantial contiguous block of memory is allocated as the slab buffer, which is subsequently partitioned into multiple slabs. When a chunk is to be stored in TreeSeries, it is first written to a slab buffer rather than directly to the SSD, ensuring a high throughput of insertion.

**Slab file.** We create a large SSD file using ‘truncate’ [17], referred to as the slab file, and divide it into multiple slabs. When the slab buffer reaches a threshold, a background thread flushes data to the slab file. In our experiments, the slab buffer and slab file size are set to 8 GB and 24 GB, respectively, to balance cost and performance; additional settings are evaluated in the technical report [12].

**Slabinfo lock-free queue.** We bind a *slabinfo* to each slab, recording metadata such as *SlabID* (slab ID), *Status* (usage status), and *Mem* (in-memory flag). We use an *atomic\_queue* [2], a lock-free circular-buffer queue supporting multiple producers/consumers, to manage concurrent access to *slabinfo*. PromTCS maintains six queues to track slabs in three statuses (free, full, evict) across memory (*MemFreeQueue*, *MemFullQueue*, *MemEvictQueue*) and disk (*DiskFreeQueue*, *DiskFullQueue*, *DiskEvictQueue*). Fig. 8 illustrates the *slabinfo* migration process: on insertion, a *slabinfo* is dequeued from a free queue to locate a writable slab; when full, it enters the full queue; after migration, it moves to an evict queue. If the free queue is empty, TreeSeries reuses an evict-queue *slabinfo*, overwriting its slab in-place and returning it to the full queue. The detailed operations appear in Alg. 2 and Alg. 3.

#### 3.3.2 Concurrent Index Structures

We introduce a pair of indexes into TreeSeries: a *tree index* for concurrent queries and a *MetaHash* for locating the writable slab on concurrent insertion.

**Tree index.** To enable fast data retrieval, we build a *lockless cache-aware B+ tree* [29] on a per-slab basis. The index key consists of the SourceID, MetricID, and time range of Items belonging to the same time series within a slab. Therefore, a slab corresponds to multiple index entries, since a slab stores  $\geq 1$  chunks of  $\geq 1$  time series.

**MetaHash & Relaxed ordering.** *MetaHash* is a two-dimensional array, where each entry stores a mutex lock *mtx* for fine-grained synchronization and a *SlabID* to locate the slab in the slab buffer. When writing massive data, TreeSeries uses *MetaHash* to distribute it across multiple slabs. The detailed write process is shown in Alg. 1. When a chunk is written, the *MetricID* and *SourceID* of the time series are modded by the row and column counts of *MetaHash* to obtain indices  $i$  and  $j$  (line 2–3, where *GetRowNum* and *GetColNum* return the dimensions of *MetaHash*). The *MetaHash*[ $i$ ][ $j$ ] is then accessed and locked to locate a writable slab. If the *SlabID* is IN\_INVALID, a usable slab is fetched from *MemFreeQueue* or *MemEvictQueue*, and the slab information in *MetaHash*[ $i$ ][ $j$ ] is updated (line 4–11). When a slab is fully written, the corresponding *slabinfo* is stored in *MemFullQueue*, and *MetaHash*[ $i$ ][ $j$ ] is reset to IN\_INVALID (line 12–14). Finally, TreeSeries unlocks *MetaHash*[ $i$ ][ $j$ ] (line 15).

Mapping writable slabs via *MetaHash* brings three advantages. **1** Distributing data from a large number of time series across multiple slabs reduces lock contention among worker threads during insertion. **2** Each slab contains samples from as few time series as possible, which effectively reduces the number of index entries in the *tree index* and the I/O overhead for large time range queries. **3** TreeSeries implements a **relaxed ordering**, and thus avoids the costly compaction for sorting, which is the main cause of WA and write stalls in LSM-trees [41]. In particular, within TreeSeries, items are unordered across different slabs (*resp.*, time series), but within each slab, items belonging to the same time series are stored either contiguously or discretely in time order. As illustrated in Fig. 9, this slab contains data from two time series:  $TS_1$  (*MetricID* = 131, *SourceID* = 270) and  $TS_2$  (*MetricID* = 31, *SourceID* = 1270). There is no order between the items of  $TS_1$  and  $TS_2$ . However, the items within each time series are sorted by StartTime (*e.g.*, *chunk<sub>2</sub>.StartTime* < *chunk<sub>5</sub>.StartTime*).

#### 3.3.3 Disk Slabinfo Area & Crash Recovery

In TreeSeries, crash recovery only needs to restore the slab file and the corresponding items in the tree index. Since the data migration from the slab buffer to the slab file is performed in units of slabs (as discussed in Sec. 3.4), we only need to save the *slabinfo* in the *DiskFullQueue* and its related index items in the tree index to the disk *slabinfo* area (*i.e.*, a file on the NVMe SSD, shown in the left bottom of Fig. 8) each time

#### --- **Algorithm 1:** The Insertion Process of TreeSeries ---

```

1 Function InsertTSeries (MetricID, SourceID, Chunk) :
2   i = MetricID % GetRowNum (MetaHash) ;
3   j = SourceID % GetColNum (MetaHash) ;
4   MetaHash[i][j].Lock() ;
5   if MetaHash[i][j].SlabID == IN_INVALID then
6     if MemFreeQueue.Empty() then
7       MetaHash[i][j].SlabID=MemEvictQueue.TryPop() ;
8     else
9       MetaHash[i][j].SlabID=MemFreeQueue.Pop() ;
10  msinfo = GetSlabInfo (MetaHash[i][j].SlabID) ;
11  WriteSlab (msinfo, Chunk) ;
12  if IsFullSlab (msinfo) then
13    MemFullQueue.Push(msinfo) ;
14    MetaHash[i][j].SlabID = IN_INVALID ;
15  MetaHash[i][j].Unlock() ;

```

---

data is migrated. During crash recovery, TreeSeries reads the data from the disk slabinfo area to restore the *slabinfo* in the *DiskFullQueue* and rebuild the tree index.

#### 3.3.4 TreeSeries Interface and workflow

We implement the following interface to simplify its usage:

- Put(SourceID, MetricID, chunk). It inserts data (*i.e.*, chunk) into TreeSeries. Fig. 8 (red arrows) illustrates the process: data is written into a writable slab located via the *MetaHash* according to Alg. 1, followed by an update to the tree index.
- Get(SourceID, MetricID, time range) -> vector<slab>. It returns a group of slabs containing samples within the given time range. The scan process is illustrated in Fig. 8 (blue arrows). TreeSeries forms a range search key,  $[s_1 + m_1 + t_{\text{start}}, s_1 + m_1 + t_{\text{end}}]$ , from the SourceID ( $s_1$ ), MetricID ( $m_1$ ), and time range ( $[t_{\text{start}}, t_{\text{end}}]$ ), then looks up the tree index and returns the matching slabs.

### 3.4 Data Migration

The bandwidth disparity across storage devices can cause data to accumulate on high-bandwidth tiers, degrading write performance. To better exploit heterogeneous bandwidth, we employ device-aware write strategies for NVMe SSDs and HDDs. We further introduce early data migration and lazy flushing to utilize I/O resources more effectively and prevent buildup on fast storage without impacting query performance.

**Batch slabs random writing to NVMe SSDs.** We next describe migration from DRAM (the slab buffer) to NVMe SSD (the slab file) in TreeSeries. Leveraging NVMe’s high concurrency [27, 42], TreeSeries performs batched, slab-granularity parallel I/O using an I/O thread pool to avoid per-migration thread overhead. When the full-slab ratio in the slab buffer reaches threshold  $k_1$  and the slab file has sufficient free space, a background thread triggers migration (Alg. 2). It dequeues *slabinfos* from *MemFullQueue* (*msinfos*), sorts them by *SlabID*, and groups adjacent slabs into *slab groups* (line 5);

#### --- **Algorithm 2:** Data Migration from DRAM to NVMe ---

```

1 # msinfos: Slabinfos from MemFullQueue ;
2 # dsinfos: Slabinfos from DiskFreeQueue ;
3 # pool: The thread pool for I/O ;
4 Function OptBatchWriteToNSSD (msinfos, dsinfos) :
5   SlabGroups = PartitionSlabGroup (msinfos) ;
6   i = 0 ;
7   foreach sgroup  $\in$  SlabGroups do
8     flag = CheckSeqSpace (dsinfos[i], sgroup.count);
9     if flag == true then
10      pool.Submit(SeqWrite (dsinfos[i], sgroup));
11    else
12      pool.Submit(RandWrite (dsinfo[i], sgroup)) ;
13    i += sgroup.count ;
14  pool.Wait() ;
15  Move(MemFullQueue, MemEvictQueue, msinfos);
16  Move(DiskFreeQueue, DiskFullQueue, dsinfos) ;
17  WriteDiskInfoArea () ;
18  UpdateCleanTxn () ;

```

---

the  $O(n \log n)$  cost is negligible compared with I/O. For each group, TreeSeries checks *dsinfos* for contiguous free space: if available, it writes the group sequentially; otherwise, it falls back to per-slab random writes (lines 7–13). After I/O completion, it updates the lock-free queues (lines 14–16), persists the *DiskFullQueue slabinfo* and related index entries for crash recovery (line 17, Sec. 3.3.3), and advances *TSObject.cleantxn* to enable log cleanup (line 18, Sec. 3.5). Writing in slab groups ❶ reduces syscall overhead and ❷ exploits NVMe’s higher sequential bandwidth than random I/O [42].

**Batch slabs sequential writing to HDDs.** To migrate data from NVMe SSDs (slab file in TreeSeries) to HDDs (SSTable in time-partitioned LSM-tree [38]), we aim to leverage HDDs’ high sequential I/O bandwidth. Thus, it reads slabs from the slab file, materializes them into a large *MemTable*, and flushes it to HDD. Since slabs are ordered by *SlabID*, the thread can read a batch of slabs sequentially with only one or two syscalls (line 3). For each slab, it sorts items and groups them into *ItemGroups* by time series, with each group sorted by timestamp (line 5). Inserting *ItemGroups* into the *MemTable* as units reduces redundant keys composed of IDs and timestamps (lines 6–7). Afterward, it updates the lock-free queues (lines 8–9). To avoid memory buildup, TreeSeries flushes each *MemTable* to HDD during the migration process (line 10).

**Early Migration & Lazy Flushing Mechanism.** Due to the I/O bandwidth gap between DRAM, NVMe SSDs, and HDDs, data can accumulate on high-bandwidth storage devices. We set low migration thresholds ( $k_1 = \frac{1}{6}$ ,  $k_2 = \frac{1}{10}$ ) to trigger early data migration, which helps mitigate data piling and improve insertion throughput but may negatively impact query workloads. Thus, we developed a lazy flush mechanism: data migrates early to maximize I/O resources, while old data is kept in the original (fast) storage devices for queries. More

#### --- **Algorithm 3:** Data Migration from NVMe to HDD ---

```

1 # dsinfos: Slabinfos from DiskFullQueue ;
2 Function OptSeqWriteToHDD (dsinfos) :
3   Slabs = ReadSlabFile (dsinfos) ;
4   foreach slab ∈ Slabs do
5     ItemGroups = GroupByTimeSeries (slab) ;
6     foreach igroup ∈ ItemGroups do
7        WriteToMemTables (igroup) ;
8   DiskFullQueue.Pop(dsinfos) ;
9   DiskEvictQueue.Push(dsinfos) ;
10   Flush (MemTables) ;

```

---

results for  $k_1$  and  $k_2$  are provided in the technical report [12].

### 3.5 Meta-driven Parallel WAL

We propose a metadata-driven parallel WAL to cut synchronization and I/O overhead, a log cleanup policy to prevent log accumulation, and parallel recovery for fast crash recovery.

**Metadata-based grouped logging.** A global WAL limits concurrent writes due to synchronization on the WAL lock (see Fig. 3). Using multiple WALs, however, raises concerns about random disk I/O and transaction dependencies. To address this, we design a metadata-driven parallel WAL.

In PromTCS, logs are split into *series logs* (a *log group*) and *sample logs* (multiple *log groups*): the former records time series metadata and IDs, while the latter records samples and IDs (see Fig. 10). Each log group consists of a ring lock-free log buffer in memory and a log directory on disk. We assign one *log thread* to each log group to perform *group commit* [23, 25, 30], i.e., batch-writing log records to disk. PromTCS dispatches sample records to sample log groups by hashing their *MetricIDs* (modulo the number of groups). Since each sample log group contains records from different time series, it does not need to preserve a global order across groups. Notably, we group by *MetricID* rather than *MetricID+SourceID*. Grouping by metric co-locates more sample logs of the same metric within the same WAL group, achieving higher compression efficiency because values of the same metric are more similar. On a real-world dataset (see Sec. 4.1), grouping by *MetricID* achieves up to 36.2% higher compression efficiency than *MetricID+SourceID*. Moreover, we ensure reliability by verifying log persistence per time series. Before *TSObject* inserts a chunk into TreeSeries, it checks whether the log record of the last sample in the chunk has been flushed. This is done by comparing the sample sequence number *txn*, i.e., a monotonically increasing value for samples within a time series. In our experiments, the sample log group size is set to 5 to balance CPU and I/O overhead (see the technical report for additional parameter evaluations).

**Log cleanup.** To prevent log file buildup, we periodically clean up *sample log records*, since series logs are much smaller and only store metadata for recovery. A *sample log record* becomes cleanable once its corresponding samples

![Flowchart: Architecture of workers writing to series and sample ring lock-free buffers.](e1a0d046fbe7f28f5e93a47091851747_img.jpg)

Worker0  
Worker1  
Worker2  
Worker3  
Series Ring Lock-free Buffer  
seq  
labels  
SourceID  
MetricID  
1  
labels1  
1  
1  
2  
labels2  
2  
2  
Metadata  
Log Record  
Worker4  
Worker5  
Worker6  
Worker7  
Worker8  
Worker9  
Allocate  
by  
MetricID  
Sample Ring Lock-free Buffer 1  
seq  
SourceID  
MetricID  
timestamp  
value  
txn  
1  
1  
1  
16800  
10.1  
1  
2  
1  
1  
18000  
12.4  
2  
Sample  
Log  
Record  
Sample Ring Lock-free Buffer N  
seq  
SourceID  
MetricID  
timestamp  
value  
txn  
1  
2  
2  
16800  
12.0  
1  
2  
3  
2  
16800  
10.5  
1

A diagram showing workers 0-3 and 4-9 routing through switches into a Series Ring Lock-free Buffer and multiple Sample Ring Lock-free Buffers, with annotations indicating metadata and sample log records.

Flowchart: Architecture of workers writing to series and sample ring lock-free buffers.

Figure 10: Illustration of meta-driven parallel WAL.

have been persisted to disk. For each *sample log group*, a cleaner thread periodically scans the log files under its *log directory*. When TreeSeries flushes samples to the NVMe SSD, it updates the corresponding *TSObject*’s cleanable sequence number (*clean\_txn*, line 18 of Alg. 2). A *sample log record* is removed if its *txn* does not exceed the corresponding *clean\_txn*; otherwise, it is rewritten to the log file.

**Parallel crash recovery.** Since each *log group* contains data from different time series, multiple threads can replay logs from different *log directories* in parallel without synchronization. Recovery first rebuilds the metadata index from the series log using stored *labels*, *MetricID*, and *SourceID*. Each thread then replays one log directory, reinserting data into the corresponding time series and updating sequence numbers. Log recording and cleanup remain suspended until recovery completes. Meanwhile, TreeSeries and the time-partitioned LSM-tree recover by reading only a small amount of metadata (i.e., the *disk slabinfo area* on NVMe SSDs and the *CURRENT* and *MANIFEST* files [7] on HDDs), making their overhead much lower than log replay. Parallel recovery effectively reduces recovery time [12].
