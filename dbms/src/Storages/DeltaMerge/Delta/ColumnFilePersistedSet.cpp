// Copyright 2023 PingCAP, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <Functions/FunctionHelpers.h>
#include <IO/Buffer/MemoryReadWriteBuffer.h>
#include <IO/ReadHelpers.h>
#include <Storages/DeltaMerge/ColumnFile/ColumnFileSchema.h>
#include <Storages/DeltaMerge/DMContext.h>
#include <Storages/DeltaMerge/Delta/ColumnFilePersistedSet.h>
#include <Storages/DeltaMerge/DeltaIndex/DeltaIndexManager.h>
#include <Storages/DeltaMerge/WriteBatchesImpl.h>
#include <Storages/Page/V3/Universal/UniversalPageStorage.h>
#include <Storages/PathPool.h>

#include <ext/scope_guard.h>

namespace DB::DM
{

inline UInt64 serializeColumnFilePersisteds(WriteBuffer & buf, const ColumnFilePersisteds & persisted_files)
{
    serializeSavedColumnFiles(buf, persisted_files);
    return buf.count();
}

inline void serializeColumnFilePersisteds(
    WriteBatches & wbs,
    PageIdU64 id,
    const ColumnFilePersisteds & persisted_files)
{
    MemoryWriteBuffer buf(0, COLUMN_FILE_SERIALIZE_BUFFER_SIZE);
    auto data_size = serializeColumnFilePersisteds(buf, persisted_files);
    wbs.meta.putPage(id, 0, buf.tryGetReadBuffer(), data_size);
}

void ColumnFilePersistedSet::updateColumnFileStats()
{
    size_t new_rows = 0;
    size_t new_bytes = 0;
    size_t new_deletes = 0;
    for (auto & file : persisted_files)
    {
        new_rows += file->getRows();
        new_bytes += file->getBytes();
        new_deletes += file->getDeletes();
    }
    persisted_files_count = persisted_files.size();
    rows = new_rows;
    bytes = new_bytes;
    deletes = new_deletes;
}

void ColumnFilePersistedSet::checkColumnFiles(const ColumnFilePersisteds & new_column_files)
{
    if constexpr (!DM_RUN_CHECK)
        return;
    size_t new_rows = 0;
    size_t new_deletes = 0;
    for (const auto & file : new_column_files)
    {
        new_rows += file->getRows();
        new_deletes += file->isDeleteRange();
    }

    RUNTIME_CHECK_MSG(
        new_rows == rows && new_deletes == deletes,
        "Rows and deletes check failed. Actual: rows[{}], deletes[{}]. Expected: rows[{}], deletes[{}]. Current column "
        "files: {}, new column files: {}.", //
        new_rows,
        new_deletes,
        rows.load(),
        deletes.load(),
        ColumnFile::filesToString(persisted_files),
        ColumnFile::filesToString(new_column_files));
}

ColumnFilePersistedSet::ColumnFilePersistedSet( //
    PageIdU64 metadata_id_,
    const ColumnFilePersisteds & persisted_column_files)
    : metadata_id(metadata_id_)
    , persisted_files(persisted_column_files)
    , log(Logger::get())
{
    updateColumnFileStats();
}

ColumnFilePersistedSetPtr ColumnFilePersistedSet::restore( //
    DMContext & context,
    const RowKeyRange & segment_range,
    PageIdU64 id)
{
    Page page = context.storage_pool->metaReader()->read(id);
    ReadBufferFromMemory buf(page.data.begin(), page.data.size());
    return ColumnFilePersistedSet::restore(context, segment_range, buf, id);
}

ColumnFilePersistedSetPtr ColumnFilePersistedSet::restore( //
    DMContext & context,
    const RowKeyRange & segment_range,
    ReadBuffer & buf,
    PageIdU64 id)
{
    auto column_files = deserializeSavedColumnFiles(context, segment_range, buf);
    return std::make_shared<ColumnFilePersistedSet>(id, column_files);
}

ColumnFilePersistedSetPtr ColumnFilePersistedSet::createFromCheckpoint( //
    const LoggerPtr & parent_log,
    DMContext & context,
    UniversalPageStoragePtr temp_ps,
    const RowKeyRange & segment_range,
    PageIdU64 delta_id,
    WriteBatches & wbs)
{
    auto delta_page_id = UniversalPageIdFormat::toFullPageId(
        UniversalPageIdFormat::toFullPrefix(context.keyspace_id, StorageType::Meta, context.physical_table_id),
        delta_id);
    auto meta_page = temp_ps->read(delta_page_id);
    ReadBufferFromMemory meta_buf(meta_page.data.begin(), meta_page.data.size());
    auto column_files = createColumnFilesFromCheckpoint(parent_log, context, segment_range, meta_buf, temp_ps, wbs);
    auto new_persisted_set = std::make_shared<ColumnFilePersistedSet>(delta_id, column_files);
    return new_persisted_set;
}

void ColumnFilePersistedSet::saveMeta(WriteBatches & wbs) const
{
    serializeColumnFilePersisteds(wbs, metadata_id, persisted_files);
}

void ColumnFilePersistedSet::saveMeta(WriteBuffer & buf) const
{
    serializeColumnFilePersisteds(buf, persisted_files);
}

void ColumnFilePersistedSet::recordRemoveColumnFilesPages(WriteBatches & wbs) const
{
    for (const auto & file : persisted_files)
        file->removeData(wbs);
}

ColumnFilePersisteds ColumnFilePersistedSet::diffColumnFiles(const ColumnFiles & previous_column_files) const
{
    // It should not be not possible that files in the snapshots are removed when calling this
    // function. So we simply expect there are more column files now.
    // Major compaction and minor compaction are segment updates, which should be blocked by
    // the for_update snapshot.
    // TODO: We'd better enforce user to specify a for_update snapshot in the args, to ensure
    //       that this function is called under a for_update snapshot context.
    RUNTIME_CHECK(previous_column_files.size() <= getColumnFileCount());

    auto it_1 = previous_column_files.begin();
    auto it_2 = persisted_files.begin();
    bool check_success = true;
    if (likely(previous_column_files.size() <= persisted_files_count.load()))
    {
        while (it_1 != previous_column_files.end() && it_2 != persisted_files.end())
        {
            // We allow passing unflushed memtable files to `previous_column_files`, these heads will be skipped anyway.
            if (!(*it_2)->mayBeFlushedFrom(&(**it_1)) && !(*it_2)->isSame(&(**it_1)))
            {
                check_success = false;
                break;
            }
            if ((*it_1)->getRows() != (*it_2)->getRows() || (*it_1)->getBytes() != (*it_2)->getBytes())
            {
                check_success = false;
                break;
            }
            it_1++;
            it_2++;
        }
    }
    else
    {
        check_success = false;
    }

    if (unlikely(!check_success))
    {
        LOG_ERROR(
            log,
            "{}, Delta Check head failed, unexpected size. head column files: {}, persisted column files: {}",
            info(),
            ColumnFile::filesToString(previous_column_files),
            detailInfo());
        throw Exception("Check head failed, unexpected size", ErrorCodes::LOGICAL_ERROR);
    }

    ColumnFilePersisteds tail;
    while (it_2 != persisted_files.end())
    {
        const auto & column_file = *it_2;
        tail.push_back(column_file);
        it_2++;
    }

    return tail;
}

bool ColumnFilePersistedSet::checkAndIncreaseFlushVersion(size_t task_flush_version)
{
    if (task_flush_version != flush_version)
    {
        LOG_DEBUG(log, "{} Stop flush because structure got updated", simpleInfo());
        return false;
    }
    flush_version += 1;
    return true;
}

bool ColumnFilePersistedSet::appendPersistedColumnFiles(const ColumnFilePersisteds & column_files, WriteBatches & wbs)
{
    ColumnFilePersisteds new_persisted_files{persisted_files};
    new_persisted_files.insert(new_persisted_files.end(), column_files.begin(), column_files.end());
    // Save the new metadata of column files to disk.
    serializeColumnFilePersisteds(wbs, metadata_id, new_persisted_files);
    wbs.writeMeta();

    // Commit updates in memory.
    persisted_files.swap(new_persisted_files);
    updateColumnFileStats();
    LOG_DEBUG(
        log,
        "{}, after append {} column files, persisted column files: {}",
        info(),
        column_files.size(),
        detailInfo());

    return true;
}

bool ColumnFilePersistedSet::updatePersistedColumnFilesAfterAddingIndex(
    const ColumnFilePersisteds & new_persisted_files,
    WriteBatches & wbs)
{
    // Save the new metadata of column files to disk.
    serializeColumnFilePersisteds(wbs, metadata_id, new_persisted_files);
    wbs.writeMeta();

    // Commit updates in memory.
    persisted_files = std::move(new_persisted_files);
    // After adding index, the stats of column files will not change.
    return true;
}

MinorCompactionPtr ColumnFilePersistedSet::pickUpMinorCompaction(size_t delta_small_column_file_rows)
{
    // Every time we try to compact all column files.
    // For ColumnFileTiny, we will try to combine small `ColumnFileTiny`s to a bigger one.
    // For ColumnFileDeleteRange and ColumnFileBig, we keep them intact.
    // And only if there exists some small `ColumnFileTiny`s which can be combined, we will actually do the compaction.
    if (!persisted_files.empty())
    {
        auto compaction = std::make_shared<MinorCompaction>(minor_compaction_version);
        bool is_all_trivial_move = true;
        MinorCompaction::Task cur_task;
        auto pack_up_cur_task = [&]() {
            bool is_trivial_move = compaction->packUpTask(std::move(cur_task));
            is_all_trivial_move = is_all_trivial_move && is_trivial_move;
            cur_task = {};
        };
        size_t index = 0;
        for (auto & file : persisted_files)
        {
            if (auto * t_file = file->tryToTinyFile(); t_file)
            {
                bool cur_task_full = cur_task.total_rows >= delta_small_column_file_rows;
                bool small_column_file = t_file->getRows() < delta_small_column_file_rows;
                bool schema_ok = cur_task.to_compact.empty();

                if (!schema_ok)
                {
                    if (auto * last_t_file = cur_task.to_compact.back()->tryToTinyFile(); last_t_file)
                        schema_ok = t_file->getSchema() == last_t_file->getSchema();
                }

                if (cur_task_full || !small_column_file || !schema_ok)
                    pack_up_cur_task();

                cur_task.addColumnFile(file, index);
            }
            else
            {
                pack_up_cur_task();
                cur_task.addColumnFile(file, index);
            }

            ++index;
        }
        pack_up_cur_task();

        if (!is_all_trivial_move)
            return compaction;
    }
    return nullptr;
}

bool ColumnFilePersistedSet::installCompactionResults(const MinorCompactionPtr & compaction, WriteBatches & wbs)
{
    if (compaction->getCompactionVersion() != minor_compaction_version)
    {
        LOG_WARNING(log, "Structure has been updated during compact");
        return false;
    }
    minor_compaction_version += 1;
    LOG_DEBUG(log, "{}, before commit compaction, persisted column files: {}", info(), detailInfo());
    ColumnFilePersisteds new_persisted_files;
    for (const auto & task : compaction->getTasks())
    {
        if (task.is_trivial_move)
            new_persisted_files.push_back(task.to_compact[0]);
        else
            new_persisted_files.push_back(task.result);
    }
    auto old_persisted_files_iter = persisted_files.begin();
    for (const auto & task : compaction->getTasks())
    {
        for (const auto & file : task.to_compact)
        {
            if (unlikely(
                    old_persisted_files_iter == persisted_files.end()
                    || (file->getId() != (*old_persisted_files_iter)->getId())
                    || (file->getRows() != (*old_persisted_files_iter)->getRows())))
            {
                throw Exception(
                    ErrorCodes::LOGICAL_ERROR,
                    "Compaction algorithm broken, "
                    "compaction={{{}}} persisted_files={} "
                    "old_persisted_files_iter.is_end={} "
                    "file->getId={} old_persist_files->getId={} file->getRows={} old_persist_files->getRows={}",
                    compaction->info(),
                    detailInfo(),
                    old_persisted_files_iter == persisted_files.end(),
                    file->getId(),
                    old_persisted_files_iter == persisted_files.end() ? -1 : (*old_persisted_files_iter)->getId(),
                    file->getRows(),
                    old_persisted_files_iter == persisted_files.end() ? -1 : (*old_persisted_files_iter)->getRows());
            }
            old_persisted_files_iter++;
        }
    }
    while (old_persisted_files_iter != persisted_files.end())
    {
        new_persisted_files.emplace_back(*old_persisted_files_iter);
        old_persisted_files_iter++;
    }

    checkColumnFiles(new_persisted_files);

    /// Save the new metadata of column files to disk.
    serializeColumnFilePersisteds(wbs, metadata_id, new_persisted_files);
    wbs.writeMeta();

    /// Commit updates in memory.
    persisted_files.swap(new_persisted_files);
    updateColumnFileStats();
    LOG_DEBUG(log, "{}, after commit compaction, persisted column files: {}", info(), detailInfo());

    return true;
}

ColumnFileSetSnapshotPtr ColumnFilePersistedSet::createSnapshot(const IColumnFileDataProviderPtr & data_provider)
{
    size_t total_rows = 0;
    size_t total_deletes = 0;
    ColumnFiles column_files;
    column_files.reserve(persisted_files.size());
    for (const auto & file : persisted_files)
    {
        column_files.push_back(file);
        total_rows += file->getRows();
        total_deletes += file->getDeletes();
    }

    if (unlikely(total_rows != rows || total_deletes != deletes))
    {
        LOG_ERROR(
            log,
            "Rows and deletes check failed. Actual: rows[{}], deletes[{}]. Expected: rows[{}], deletes[{}].",
            total_rows,
            total_deletes,
            rows.load(),
            deletes.load());
        throw Exception("Rows and deletes check failed.", ErrorCodes::LOGICAL_ERROR);
    }

    return std::make_shared<ColumnFileSetSnapshot>(data_provider, std::move(column_files), rows, bytes, deletes);
}

} // namespace DB::DM
