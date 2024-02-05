# LocalDB: persistent uncommitted changes

Tablets may need to store a potentially large amount of data over a potentially long time, and then either commit or rollback all accumulated changes atomically without keeping them in-memory. To support this [LocalDB](https://github.com/ydb-platform/ydb/blob/main/ydb/core/tablet_flat/flat_database.h) allows table changes to be marked with a unique 64-bit transaction id (TxId), which are stored in the given table alongside committed data, but not visible until the given TxId is committed. The commit or rollback itself is atomic and very cheap, with committed data eventually integrated into the table as if written normally in the first place.

This feature is used as a building block for various other features:

* Storing uncommitted changes in long-running YQL transactions and observing them in subsequent queries in the same transaction
* Storing undecided side-effects in upcoming volatile distributed transactions
* Cross-region consistent replication, where table changes are streamed and applied in small batches, and periodically committed as consistent snapshots of the origin database state

## Limitations

Current implementation has the following limitations:

1. Keys may have multiple changes written by different TxIds, however these transactions must be committed in the order of these writes. For example, when key K is updated by tx1 and then by tx2, it is possible to then commit tx1 first and then tx2, and observe both changes. However, if tx2 is committed first, then tx1 must be rolled back.
2. The number of uncommitted transactions and the amount of uncommitted data to a particular key must be limited by upper layers to some reasonably small value.
3. TxIds must not be reused after commit or rollback (even across different shards), and must be globally unique.

## Logging uncommitted changes

Redo log (see [flat_redo_writer.h](https://github.com/ydb-platform/ydb/blob/main/ydb/core/tablet_flat/flat_redo_writer.h)) has the following events related to uncommitted changes:

* [EvUpdateTx](https://github.com/ydb-platform/ydb/blob/ab6222f2deabf1a12b50db13728b68cbd6b59604/ydb/core/tablet_flat/flat_redo_writer.h#L160) stores table changes with an uncommitted TxId. This event is generated by [TDatabase::UpdateTx](https://github.com/ydb-platform/ydb/blob/ab6222f2deabf1a12b50db13728b68cbd6b59604/ydb/core/tablet_flat/flat_database.h#L123) database method.
* [EvRemoveTx](https://github.com/ydb-platform/ydb/blob/ab6222f2deabf1a12b50db13728b68cbd6b59604/ydb/core/tablet_flat/flat_redo_writer.h#L169) is used for removing a given TxId (performing a rollback). This event is generated by [TDatabase::RemoveTx](https://github.com/ydb-platform/ydb/blob/ab6222f2deabf1a12b50db13728b68cbd6b59604/ydb/core/tablet_flat/flat_database.h#L124) database method.
* [EvCommitTx](https://github.com/ydb-platform/ydb/blob/ab6222f2deabf1a12b50db13728b68cbd6b59604/ydb/core/tablet_flat/flat_redo_writer.h#L183) is used for committing a given TxId at the specified MVCC version. This event is generated by [TDatabase::CommitTx](https://github.com/ydb-platform/ydb/blob/ab6222f2deabf1a12b50db13728b68cbd6b59604/ydb/core/tablet_flat/flat_database.h#L125) database method.

## Storing uncommitted changes in MemTables

[MemTable](https://github.com/ydb-platform/ydb/blob/0adff98ae52cb826f7fb9705503e430b9812994f/ydb/core/tablet_flat/flat_mem_warm.h#L180) in LocalDB is a relatively small in-memory sorted tree that maps table keys to values. MemTable value is a chain of MVCC (partial) rows, each tagged with a row version (a pair of Step and TxId which is a global timestamp). Rows are normally pre-merged across the given MemTable. For example, let's suppose there have been the following operations for some key K:

| Version | Operation |
--- | ---
| `v1000/10` | `UPDATE ... SET A = 1` |
| `v2000/11` | `UPDATE ... SET B = 2` |
| `v3000/12` | `UPDATE ... SET C = 3` |

Then the chain of rows for key K in a single MemTable will look like this:

| Version | Row |
--- | ---
| `v3000/12` | `SET A = 1, B = 2, C = 3` |
| `v2000/11` | `SET A = 1, B = 2` |
| `v1000/10` | `SET A = 1` |

However, if the MemTable was split between updates, it may look like this:

| MemTable | Version | Row |
--- | --- | ---
| Epoch 2 | `v3000/12` | `SET B = 2, C = 3` |
| Epoch 2 | `v2000/11` | `SET B = 2` |
| Epoch 1 | `v1000/10` | `SET A = 1` |

Changes are applied to the current MemTable, and uncommitted changes are no exception. However, they are tagged with a special version (where Step is the maximum possible number, as if they are in some "distant" future, and TxId is their uncommitted TxId), without any pre-merging. For example, let's suppose we additionally performed the following operations:

| TxId | Operation |
--- | ---
| 15 | `UPDATE ... SET C = 10` |
| 13 | `UPDATE ... SET B = 20` |

The update chain for our key K will look like this:

| Version | Row |
--- | ---
| `v{max}/13` | `SET B = 20` |
| `v{max}/15` | `SET C = 10` |
| `v3000/12` | `SET A = 1, B = 2, C = 3` |
| `v2000/11` | `SET A = 1, B = 2` |
| `v1000/10` | `SET A = 1` |

When reading [iterator](https://github.com/ydb-platform/ydb/blob/main/ydb/core/tablet_flat/flat_mem_iter.h) performs a lookup for changes with `Step == max` into an [in-memory transaction map](https://github.com/ydb-platform/ydb/blob/0e69bf615395fdd48ecee032faaec81bc468b0b8/ydb/core/tablet_flat/flat_table.h#L359), which maps committed TxIds to their corresponding commit versions, and applies all committed deltas until it finds and applies a pre-merged row with `Step != max`.

Let's suppose we commit tx 13 at `v4000/20`. At that point transaction map is updated with `[13] => v4000/20`, and tx 13 is now committed. Any read afterwards will consult transaction map and apply deltas for tx 13, but skip tx 15, since it was not committed and treated as implicitly rolled back. MemTable chain for key K is not changed however.

Let's suppose we now perform an `UPDATE ... SET A = 30` at version `v5000/21`, the resulting chain will look as follows:

| Version | Row |
--- | ---
| `v5000/21` | `SET A = 30, B = 20, C = 3` |
| `v{max}/13` | `SET B = 20` |
| `v{max}/15` | `SET C = 10` |
| `v3000/12` | `SET A = 1, B = 2, C = 3` |
| `v2000/11` | `SET A = 1, B = 2` |
| `v1000/10` | `SET A = 1` |

Notice how the new record has its state pre-merged, including the previously committed delta for tx 13. Since tx 15 is not committed it was skipped and baked into a pre-merged state for `v5000/21`. It is important that tx 15 is not committed afterwards, and would result in a read anomaly otherwise: some versions would observe it as committed, and some won't.

## Compacting uncommitted changes

Compaction takes some parts from the table, merges them in a sorted order, and writes as a new SST, which replaces compacted data. When compacting MemTable it also implies compacting the relevant redo log, and includes `EvRemoveTx`/`EvCommitTx` events, which affect change visibility and must also end up in persistent storage. LocalDB writes TxStatus blobs (see [flat_page_txstatus.h](https://github.com/ydb-platform/ydb/blob/main/ydb/core/tablet_flat/flat_page_txstatus.h)), which store a list of committed and removed transactions, and replace the compacted redo log in regard to `EvRemoveTx`/`EvCommitTx` events. Compaction uses the latest transaction status maps, but it filters them leaving only those transactions that are mentioned in the relevant MemTables or previous TxStatus pages, so that it matches the compacted redo log.

Data pages (see [flat_page_data.h](https://github.com/ydb-platform/ydb/blob/main/ydb/core/tablet_flat/flat_page_data.h)) store uncommitted deltas from MemTables (or other SSTs) aggregated by their TxId in the same order just before the primary record. Records may have MVCC flags (HasHistory, IsVersioned, IsErased), which specify whether there is MVCC fields and data present. Delta records have an [IsDelta flag](https://github.com/ydb-platform/ydb/blob/0adff98ae52cb826f7fb9705503e430b9812994f/ydb/core/tablet_flat/flat_page_data.h#L98), which is really a HasHistory flag without other MVCC flags. Since it was never used by previous versions (HasHistory flag was only ever used together with IsVersioned flag, you could not have history rows without a verioned record), it clearly identifies record as an uncommitted delta. Delta records have a [TDelta](https://github.com/ydb-platform/ydb/blob/0adff98ae52cb826f7fb9705503e430b9812994f/ydb/core/tablet_flat/flat_page_data.h#L66) info immediately after the fixed record data, which specifies TxId of the uncommitted delta.

One key may have several uncommitted delta records, as well as (optionally) the latest committed record data. Historically, data pages could only have one record (and one record pointer) per key, so the record pointer leads to the top of the delta chain, and other records are available via additional per-record offset table for other records:

| Offset | Description |
--- | ---
| -X*8 | offset of Main |
| ... | ... |
| -16 | offset of Delta 2 |
| -8 | offset of Delta 1 |
| 0 | header of Delta 0 |
| ... | ... |
| offset of Delta 1 | header of Delta 1 |
| ... | ... |
| offset of Main | header of Main |

Having a pointer to Delta 0, other records for the same key are available with the `GetAltRecord(size_t index)` method, where `index` is the record number (which is 1 for Delta 1). The chain of records ends either with a pointer to the record without an IsDelta flag (the Main record), or 0 (when there is no Main record for the key).

Let's suppose that after writing tx 13 above the MemTable was compacted. Entry for the 32-bit key K may look like this (offsets are relative to the record pointer on the table):

| Offset | Value | Description |
--- | --- | ---
| -16 | 58 | offset of Main |
| -8 | 29 | offset of Delta 1 |
| 0 | 0x21 | Delta 0: IsDelta + ERowOp::Upsert |
| 1 | 0x00 | .. key column is not NULL |
| 2 | K | .. key column (32-bit) |
| 6 | 0x00 | .. column A is empty |
| 7 | 0 | .. column A (32-bit) |
| 11 | 0x01 | .. column B = ECellOp::Set |
| 12 | 20 | .. column B (32-bit) |
| 16 | 0x00 | .. column C is empty |
| 17 | 0 | .. column C (32-bit) |
| 21 | 13 | .. TDelta::TxId |
| 29 | 0x21 | Delta 1: IsDelta + ERowOp::Upsert |
| 30 | 0x00 | .. key column is not NULL |
| 31 | K | .. key column (32-bit) |
| 35 | 0x00 | .. column A is empty |
| 36 | 0 | .. column A (32-bit) |
| 40 | 0x00 | .. column B is empty |
| 41 | 0 | .. column B (32-bit) |
| 45 | 0x01 | .. column C = ECellOp::Set |
| 46 | 10 | .. column C (32-bit) |
| 50 | 15 | .. TDelta::TxId |
| 58 | 0x61 | Main: HasHistory + IsVersioned + ERowOp::Upsert |
| 59 | 0x00 | .. key column is not NULL |
| 60 | K | .. key column (32-bit) |
| 64 | 0x01 | .. column A = ECellOp::Set |
| 65 | 1 | .. column A (32-bit) |
| 69 | 0x01 | .. column B = ECellOp::Set |
| 70 | 2 | .. column B (32-bit) |
| 74 | 0x01 | .. column C = ECellOp::Set |
| 75 | 3 | .. column C (32-bit) |
| 79 | 3000 | .. RowVersion.Step |
| 87 | 12 | .. RowVersion.TxId |
| 95 | - | End of record |

The HasHistory flag in the Main record shows that other two records are stored among history data with keys `(RowId, 2000, 11)` and `(RowId, 1000, 10)` respectively.

When compacting iterator runs in a special mode that enumerates all deltas and all versions for each key. The compaction scan implementation (see [flat_ops_compact.h](https://github.com/ydb-platform/ydb/blob/main/ydb/core/tablet_flat/flat_ops_compact.h)) first aggregates all uncommitted deltas by their TxId in the same order (in case changes from different TxIds overlap their order may change arbitrarily, which is OK since such transactions are not supposed to both commit). After uncommitted deltas are aggregated, they are flushed to the resulting SST (see [flat_part_writer.h](https://github.com/ydb-platform/ydb/blob/main/ydb/core/tablet_flat/flat_part_writer.h) and [flat_page_writer.h](https://github.com/ydb-platform/ydb/blob/main/ydb/core/tablet_flat/flat_page_writer.h)), and committed row versions for the same key are enumerated, which are written in decreasing version order.

When iterator positions to the first committed delta (i.e. the IsDelta record which has commit info in the transaction map), the commit info is used as the resulting row versions, with row state combined from all deltas below, including the first committed record from each LSM level participating in compaction. When positioning to the next version iterator skips delta with version at or below the last one and the process is repeated.

With a large number of compacted deltas for a key, when they are later committed with different versions, the process of generating committed records grows quadratically in the number of deltas. For this reason upper levels should control the number of deltas for each key and must not allow them to grow too large (deltas might be merged in the reverse order in the future to side step this problem). Other limitation is that uncommitted deltas for a given key need to all be in memory and on a single page, since each read must walk the entire delta chain and check whether each record is committed. In the future we may want to store deltas across multiple pages, but since they all need to be in memory anyway there is little reason to do so. Removing the requirement for all deltas to be in memory during reads is theoretically possible, but it requires storing them in a different form.

## Uncommitted transaction stats

Optimistically most transactions are eventually committed, but sometimes transactions roll back, even after compaction. Since rollbacks may cause a large amount of data to become unreachable, SSTs store TxIdStats pages (see [flat_page_txidstat.h](https://github.com/ydb-platform/ydb/blob/main/ydb/core/tablet_flat/flat_page_txidstat.h)) with the number of rows and bytes occupied by each TxId. As more and more transactions are rolled back, compaction strategy aggregates the number of unreachable bytes, and eventually runs garbage collecting compactions.

These pages are also used for keeping in-memory transaction maps small. When transaction is eventually committed or rolled back, its status is stored in an in-memory hashmap as long as the specified TxId has deltas anywhere. As SSTs are compacted, uncommitted deltas from committed transactions are rewritten into fully committed records, rolled back deltas are removed entirely, and eventually transactions stop being mentioned in TxIdStats pages of resulting SSTs. When a given TxId is no longer mentioned anywhere, it is safely removed from transaction maps and no longer occupies any memory.

The in-memory transaction map is limited in size by limiting the number of open (which are neither committed nor rolled back) transactions at the datashard level. When compacting SSTs may only generate deltas for currently open transactions, so the total transaction map size is limited by the maximum number of open transactions, multiplied by the number of SSTs.

## Borrowing SSTs with uncommitted changes

When copying tables, and when datashards split or merge they use LocalDB borrowing, where source shard SSTs are merged into destination shard tables. When using uncommitted changes those may contain changes from open transactions, or those which have been committed or rolled back, but not compacted yet. To guarantee that destination shards have the same view of the data as the source shards, TxStatus blobs also need borrowing, modifying destination transaction maps.

Note that transaction may have different status at different shards. Let's review a hypothetical example:

* Transaction writes changes to key K with TxId at shard S, which are compacted into a large SST
* Shard S becomes too large and splits into shards L and R, so that SST is borrowed by both with row filters applied, key K ends up in shard L, but transaction TxId is also phantomly present in shard R, since it is mentioned in the borrowed SST
* Transaction commits changes at shard L, without a commit at shard R (since logically and from the writer's perspective there have been no changes at shard R)
* Since transaction has finished, this TxId would be eventually rolled back at shard R (which doesn't cause any visible side-effects)
* Let's supposed that shards L and R are eventually merged

When merged TxStatus from shard L would specify TxId as committed, but TxStatus from shard R would specify TxId as rolled back. Since transactions are supposed to commit all changes, and phantom rollbacks are possible, when conflicting TxStatus are merged commit "wins" over rollback.

Note that this is purely theoretical, in reality shards currently fully compact before splitting or merging, so conflicting TxStatus should not be possible, this is done purely as an additional safety net and must be taken into account. This means that shards are not allowed to partially commit transaction changes, all changes from a given TxId must be committed. This also means TxIds must never be reused, even across shards, and only globally unique TxIds are safe to use for uncommitted changes.