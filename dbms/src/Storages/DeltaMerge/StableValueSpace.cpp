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

#include <Interpreters/SharedContexts/Disagg.h>
#include <Storages/DeltaMerge/ConcatSkippableBlockInputStream.h>
#include <Storages/DeltaMerge/DMVersionFilterBlockInputStream.h>
#include <Storages/DeltaMerge/File/DMFile.h>
#include <Storages/DeltaMerge/File/DMFileBlockInputStream.h>
#include <Storages/DeltaMerge/Remote/DataStore/DataStore.h>
#include <Storages/DeltaMerge/RestoreDMFile.h>
#include <Storages/DeltaMerge/RowKeyFilter.h>
#include <Storages/DeltaMerge/RowKeyRange.h>
#include <Storages/DeltaMerge/StableValueSpace.h>
#include <Storages/Page/V3/Universal/UniversalPageStorage.h>


namespace DB::ErrorCodes
{
extern const int LOGICAL_ERROR;
}

namespace DB::DM
{
void StableValueSpace::setFiles(const DMFiles & files_, const RowKeyRange & range, const DMContext * dm_context)
{
    UInt64 rows = 0;
    UInt64 bytes = 0;

    if (range.all())
    {
        for (const auto & file : files_)
        {
            rows += file->getRows();
            bytes += file->getBytes();
        }
    }
    else if (dm_context != nullptr)
    {
        for (const auto & file : files_)
        {
            auto match = DMFilePackFilter::loadValidRowsAndBytes(
                *dm_context,
                file,
                /*set_cache_if_miss*/ true,
                {range});
            rows += match.match_rows;
            bytes += match.match_bytes;
        }
    }

    this->valid_rows = rows;
    this->valid_bytes = bytes;
    this->files = files_;
}

void StableValueSpace::saveMeta(WriteBatchWrapper & meta_wb)
{
    MemoryWriteBuffer buf(0, 8192);
    // The method must call `buf.count()` to get the last seralized size before `buf.tryGetReadBuffer`
    auto data_size = serializeMetaToBuf(buf);
    meta_wb.putPage(id, 0, buf.tryGetReadBuffer(), data_size);
}

UInt64 StableValueSpace::serializeMetaToBuf(WriteBuffer & buf) const
{
    writeIntBinary(STORAGE_FORMAT_CURRENT.stable, buf);
    if (likely(STORAGE_FORMAT_CURRENT.stable == StableFormat::V1))
    {
        writeIntBinary(valid_rows, buf);
        writeIntBinary(valid_bytes, buf);
        writeIntBinary(static_cast<UInt64>(files.size()), buf);
        for (const auto & f : files)
        {
            RUNTIME_CHECK_MSG(
                f->metaVersion() == 0,
                "StableFormat::V1 cannot persist meta_version={}",
                f->metaVersion());
            writeIntBinary(f->pageId(), buf);
        }
    }
    else if (STORAGE_FORMAT_CURRENT.stable == StableFormat::V2)
    {
        dtpb::StableLayerMeta meta;
        meta.set_valid_rows(valid_rows);
        meta.set_valid_bytes(valid_bytes);
        for (const auto & f : files)
        {
            auto * mf = meta.add_files();
            mf->set_page_id(f->pageId());
            mf->set_meta_version(f->metaVersion());
        }

        auto data = meta.SerializeAsString();
        writeStringBinary(data, buf);
    }
    else
    {
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Unexpected version: {}", STORAGE_FORMAT_CURRENT.stable);
    }
    return buf.count();
}

namespace
{
dtpb::StableLayerMeta derializeMetaV1FromBuf(ReadBuffer & buf)
{
    dtpb::StableLayerMeta meta;
    UInt64 valid_rows, valid_bytes, size;
    readIntBinary(valid_rows, buf);
    readIntBinary(valid_bytes, buf);
    readIntBinary(size, buf);
    meta.set_valid_rows(valid_rows);
    meta.set_valid_bytes(valid_bytes);
    for (size_t i = 0; i < size; ++i)
    {
        UInt64 page_id;
        readIntBinary(page_id, buf);
        meta.add_files()->set_page_id(page_id);
    }
    return meta;
}

dtpb::StableLayerMeta derializeMetaV2FromBuf(ReadBuffer & buf)
{
    dtpb::StableLayerMeta meta;
    String data;
    readStringBinary(data, buf);
    RUNTIME_CHECK_MSG(
        meta.ParseFromString(data),
        "Failed to parse StableLayerMeta from string: {}",
        Redact::keyToHexString(data.data(), data.size()));
    return meta;
}

dtpb::StableLayerMeta derializeMetaFromBuf(ReadBuffer & buf)
{
    UInt64 version;
    readIntBinary(version, buf);
    if (version == StableFormat::V1)
        return derializeMetaV1FromBuf(buf);
    else if (version == StableFormat::V2)
        return derializeMetaV2FromBuf(buf);
    else
        throw Exception(ErrorCodes::LOGICAL_ERROR, "Unexpected version: {}", version);
}
} // namespace

std::string StableValueSpace::serializeMeta() const
{
    WriteBufferFromOwnString wb;
    serializeMetaToBuf(wb);
    return wb.releaseStr();
}

StableValueSpacePtr StableValueSpace::restore(DMContext & dm_context, PageIdU64 id)
{
    // read meta page
    Page page = dm_context.storage_pool->metaReader()->read(id); // not limit restore
    ReadBufferFromMemory buf(page.data.begin(), page.data.size());
    return StableValueSpace::restore(dm_context, buf, id);
}

StableValueSpacePtr StableValueSpace::restore(DMContext & dm_context, ReadBuffer & buf, PageIdU64 id)
{
    auto stable = std::make_shared<StableValueSpace>(id);

    auto metapb = derializeMetaFromBuf(buf);
    auto remote_data_store = dm_context.global_context.getSharedContextDisagg()->remote_data_store;
    for (int i = 0; i < metapb.files().size(); ++i)
    {
        UInt64 page_id = metapb.files(i).page_id();
        UInt64 meta_version = metapb.files(i).meta_version();
        auto dmfile = remote_data_store
            ? restoreDMFileFromRemoteDataSource(dm_context, remote_data_store, page_id, meta_version)
            : restoreDMFileFromLocal(dm_context, page_id, meta_version);
        stable->files.push_back(dmfile);
    }

    stable->valid_rows = metapb.valid_rows();
    stable->valid_bytes = metapb.valid_bytes();

    return stable;
}

StableValueSpacePtr StableValueSpace::createFromCheckpoint( //
    [[maybe_unused]] const LoggerPtr & parent_log,
    DMContext & dm_context,
    UniversalPageStoragePtr temp_ps,
    PageIdU64 stable_id,
    WriteBatches & wbs)
{
    auto stable = std::make_shared<StableValueSpace>(stable_id);

    auto stable_page_id = UniversalPageIdFormat::toFullPageId(
        UniversalPageIdFormat::toFullPrefix(dm_context.keyspace_id, StorageType::Meta, dm_context.physical_table_id),
        stable_id);
    auto page = temp_ps->read(stable_page_id);
    ReadBufferFromMemory buf(page.data.begin(), page.data.size());

    // read stable meta info
    auto metapb = derializeMetaFromBuf(buf);
    auto remote_data_store = dm_context.global_context.getSharedContextDisagg()->remote_data_store;
    for (int i = 0; i < metapb.files().size(); ++i)
    {
        UInt64 page_id = metapb.files(i).page_id();
        UInt64 meta_version = metapb.files(i).meta_version();
        auto dmfile = restoreDMFileFromCheckpoint(dm_context, remote_data_store, temp_ps, wbs, page_id, meta_version);
        stable->files.push_back(dmfile);
    }

    stable->valid_rows = metapb.valid_rows();
    stable->valid_bytes = metapb.valid_bytes();

    return stable;
}

size_t StableValueSpace::getRows() const
{
    return valid_rows;
}

size_t StableValueSpace::getBytes() const
{
    return valid_bytes;
}

size_t StableValueSpace::getDMFilesBytesOnDisk() const
{
    size_t bytes = 0;
    for (const auto & file : files)
        bytes += file->getBytesOnDisk();
    return bytes;
}

size_t StableValueSpace::getDMFilesPacks() const
{
    size_t packs = 0;
    for (const auto & file : files)
        packs += file->getPacks();
    return packs;
}

size_t StableValueSpace::getDMFilesRows() const
{
    size_t rows = 0;
    for (const auto & file : files)
        rows += file->getRows();
    return rows;
}

size_t StableValueSpace::getDMFilesBytes() const
{
    size_t bytes = 0;
    for (const auto & file : files)
        bytes += file->getBytes();
    return bytes;
}

String StableValueSpace::getDMFilesString()
{
    return DMFile::info(files);
}

void StableValueSpace::enableDMFilesGC(DMContext & dm_context)
{
    if (auto data_store = dm_context.global_context.getSharedContextDisagg()->remote_data_store; !data_store)
    {
        for (auto & file : files)
            file->enableGC();
    }
    else
    {
        auto delegator = dm_context.path_pool->getStableDiskDelegator();
        for (auto & file : files)
            delegator.enableGCForRemoteDTFile(file->fileId());
    }
}

void StableValueSpace::recordRemovePacksPages(WriteBatches & wbs) const
{
    for (const auto & file : files)
    {
        // Here we should remove the ref id instead of file_id.
        // Because a dmfile could be used by several segments, and only after all ref_ids are removed, then the file_id removed.
        wbs.removed_data.delPage(file->pageId());
    }
}

void StableValueSpace::calculateStableProperty(
    const DMContext & dm_context,
    const RowKeyRange & rowkey_range,
    bool is_common_handle)
{
    property.gc_hint_version = std::numeric_limits<UInt64>::max();
    property.num_versions = 0;
    property.num_puts = 0;
    property.num_rows = 0;
    for (auto & file : files)
    {
        const auto & pack_stats = file->getPackStats();
        const auto & pack_properties = file->getPackProperties();
        if (pack_stats.empty())
            continue;
        // if PackProperties of this DMFile is empty, this must be an old format file generated by previous version.
        // so we need to create file property for this file.
        // but to keep dmfile immutable, we just cache the result in memory.
        //
        // `new_pack_properties` is the temporary container for the calculation result of this StableValueSpace's pack property.
        // Note that `pack_stats` stores the stat of the whole underlying DTFile,
        // and this Segment may share this DTFile with other Segment. So `pack_stats` may be larger than `new_pack_properties`.
        DMFileMeta::PackProperties new_pack_properties;
        if (pack_properties.property_size() == 0)
        {
            LOG_DEBUG(log, "Try to calculate StableProperty from column data for stable {}", id);
            ColumnDefines read_columns;
            read_columns.emplace_back(getExtraHandleColumnDefine(is_common_handle));
            read_columns.emplace_back(getVersionColumnDefine());
            read_columns.emplace_back(getTagColumnDefine());
            // Note we `RowKeyRange::newAll` instead of `segment_range`,
            // because we need to calculate StableProperty based on the whole DTFile,
            // and then use related info for this StableValueSpace.
            //
            // If we pass `segment_range` instead,
            // then the returned stream is a `SkippableBlockInputStream` which will complicate the implementation
            DMFileBlockInputStreamBuilder builder(dm_context.global_context);
            BlockInputStreamPtr data_stream
                = builder
                      .setRowsThreshold(std::numeric_limits<UInt64>::max()) // because we just read one pack at a time
                      .onlyReadOnePackEveryTime()
                      .setTracingID(fmt::format("{}-calculateStableProperty", dm_context.tracing_id))
                      .build(file, read_columns, RowKeyRanges{rowkey_range}, dm_context.scan_context);
            auto mvcc_stream = std::make_shared<DMVersionFilterBlockInputStream<DMVersionFilterMode::COMPACT>>(
                data_stream,
                read_columns,
                0,
                is_common_handle);
            mvcc_stream->readPrefix();
            while (true)
            {
                size_t last_effective_num_rows = mvcc_stream->getEffectiveNumRows();

                Block block = mvcc_stream->read();
                if (!block)
                    break;
                if (!block.rows())
                    continue;

                size_t cur_effective_num_rows = mvcc_stream->getEffectiveNumRows();
                size_t gc_hint_version = mvcc_stream->getGCHintVersion();
                auto * pack_property = new_pack_properties.add_property();
                pack_property->set_num_rows(cur_effective_num_rows - last_effective_num_rows);
                pack_property->set_gc_hint_version(gc_hint_version);
                pack_property->set_deleted_rows(mvcc_stream->getDeletedRows());
            }
            mvcc_stream->readSuffix();
        }
        auto pack_filter = DMFilePackFilter::loadFrom(
            dm_context,
            file,
            /*set_cache_if_miss*/ false,
            {rowkey_range},
            EMPTY_RS_OPERATOR,
            {});
        const auto & pack_res = pack_filter->getPackRes();
        size_t new_pack_properties_index = 0;
        const bool use_new_pack_properties = pack_properties.property_size() == 0;
        if (use_new_pack_properties)
        {
            const size_t use_packs_count = pack_filter->countUsePack();

            RUNTIME_CHECK_MSG(
                static_cast<size_t>(new_pack_properties.property_size()) == use_packs_count,
                "size doesn't match, new_pack_properties_size={} use_packs_size={}",
                new_pack_properties.property_size(),
                use_packs_count);
        }
        for (size_t pack_id = 0; pack_id < pack_res.size(); ++pack_id)
        {
            if (!pack_res[pack_id].isUse())
                continue;
            property.num_versions += pack_stats[pack_id].rows;
            property.num_puts += pack_stats[pack_id].rows - pack_stats[pack_id].not_clean;
            if (use_new_pack_properties)
            {
                const auto & pack_property = new_pack_properties.property(new_pack_properties_index);
                property.num_rows += pack_property.num_rows();
                property.gc_hint_version = std::min(property.gc_hint_version, pack_property.gc_hint_version());
                new_pack_properties_index += 1;
            }
            else
            {
                const auto & pack_property = pack_properties.property(pack_id);
                property.num_rows += pack_property.num_rows();
                property.gc_hint_version = std::min(property.gc_hint_version, pack_property.gc_hint_version());
            }
        }
    }
    is_property_cached.store(true, std::memory_order_release);
}


// ================================================
// StableValueSpace::Snapshot
// ================================================

using Snapshot = StableValueSpace::Snapshot;
using SnapshotPtr = std::shared_ptr<Snapshot>;

SnapshotPtr StableValueSpace::createSnapshot()
{
    auto snap = std::make_shared<Snapshot>(this->shared_from_this());
    snap->id = id;
    snap->valid_rows = valid_rows;
    snap->valid_bytes = valid_bytes;

    for (size_t i = 0; i < files.size(); i++)
    {
        auto column_cache = std::make_shared<ColumnCache>();
        snap->column_caches.emplace_back(column_cache);
    }

    return snap;
}

void StableValueSpace::drop(const FileProviderPtr & file_provider)
{
    for (auto & file : files)
    {
        file->remove(file_provider);
    }
}

template <bool need_row_id>
ConcatSkippableBlockInputStreamPtr<need_row_id> StableValueSpace::Snapshot::getInputStream(
    const DMContext & dm_context,
    const ColumnDefines & read_columns,
    const RowKeyRanges & rowkey_ranges,
    UInt64 max_data_version,
    size_t expected_block_size,
    bool enable_handle_clean_read,
    ReadTag read_tag,
    const DMFilePackFilterResults & pack_filter_results,
    bool is_fast_scan,
    bool enable_del_clean_read,
    const std::vector<IdSetPtr> & read_packs,
    std::function<void(DMFileBlockInputStreamBuilder &)> additional_builder_opt)
{
    LOG_DEBUG(
        log,
        "StableVS getInputStream"
        " start_ts={} enable_handle_clean_read={} is_fast_mode={} enable_del_clean_read={}",
        max_data_version,
        enable_handle_clean_read,
        is_fast_scan,
        enable_del_clean_read);
    SkippableBlockInputStreams streams;
    std::vector<size_t> rows;
    streams.reserve(stable->files.size());
    rows.reserve(stable->files.size());

    for (size_t i = 0; i < stable->files.size(); ++i)
    {
        DMFileBlockInputStreamBuilder builder(dm_context.global_context);
        builder.enableCleanRead(enable_handle_clean_read, is_fast_scan, enable_del_clean_read, max_data_version)
            .enableColumnCacheLongTerm(dm_context.pk_col_id)
            .setDMFilePackFilterResult(pack_filter_results.size() > i ? pack_filter_results[i] : nullptr)
            .setColumnCache(column_caches[i])
            .setTracingID(dm_context.tracing_id)
            .setRowsThreshold(expected_block_size)
            .setReadPacks(read_packs.size() > i ? read_packs[i] : nullptr)
            .setReadTag(read_tag);
        if (additional_builder_opt)
            additional_builder_opt(builder);
        streams.push_back(builder.build(stable->files[i], read_columns, rowkey_ranges, dm_context.scan_context));
        rows.push_back(stable->files[i]->getRows());
    }

    return ConcatSkippableBlockInputStream<need_row_id>::create(
        std::move(streams),
        std::move(rows),
        dm_context.scan_context);
}

template ConcatSkippableBlockInputStreamPtr<false> StableValueSpace::Snapshot::getInputStream(
    const DMContext & dm_context,
    const ColumnDefines & read_columns,
    const RowKeyRanges & rowkey_ranges,
    UInt64 max_data_version,
    size_t expected_block_size,
    bool enable_handle_clean_read,
    ReadTag read_tag,
    const DMFilePackFilterResults & pack_filter_results,
    bool is_fast_scan,
    bool enable_del_clean_read,
    const std::vector<IdSetPtr> & read_packs,
    std::function<void(DMFileBlockInputStreamBuilder &)> additional_builder_opt);

template ConcatSkippableBlockInputStreamPtr<true> StableValueSpace::Snapshot::getInputStream(
    const DMContext & dm_context,
    const ColumnDefines & read_columns,
    const RowKeyRanges & rowkey_ranges,
    UInt64 max_data_version,
    size_t expected_block_size,
    bool enable_handle_clean_read,
    ReadTag read_tag,
    const DMFilePackFilterResults & pack_filter_results,
    bool is_fast_scan,
    bool enable_del_clean_read,
    const std::vector<IdSetPtr> & read_packs,
    std::function<void(DMFileBlockInputStreamBuilder &)> additional_builder_opt);

RowsAndBytes StableValueSpace::Snapshot::getApproxRowsAndBytes(const DMContext & dm_context, const RowKeyRange & range)
    const
{
    // Avoid unnecessary reading IO
    if (valid_rows == 0 || range.none())
        return {0, 0};

    size_t match_packs = 0;
    size_t total_match_rows = 0;
    size_t total_match_bytes = 0;
    // Usually, this method will be called for some "cold" key ranges.
    // Loading the index into cache may pollute the cache and make the hot index cache invalid.
    // So don't refill the cache if the index does not exist.
    constexpr bool set_cache_if_miss = false;
    for (auto & f : stable->files)
    {
        auto match = DMFilePackFilter::loadValidRowsAndBytes(dm_context, f, set_cache_if_miss, {range});
        match_packs += match.match_packs;
        total_match_rows += match.match_rows;
        total_match_bytes += match.match_bytes;
    }
    if (!total_match_rows || !match_packs)
        return {0, 0};
    Float64 avg_pack_rows = static_cast<Float64>(total_match_rows) / match_packs;
    Float64 avg_pack_bytes = static_cast<Float64>(total_match_bytes) / match_packs;
    // By average, the first and last pack are only half covered by the range.
    // And if this range only covers one pack, then return the pack's stat.
    size_t approx_rows = std::max(avg_pack_rows, total_match_rows - avg_pack_rows / 2);
    size_t approx_bytes = std::max(avg_pack_bytes, total_match_bytes - avg_pack_bytes / 2);
    return {approx_rows, approx_bytes};
}

StableValueSpace::Snapshot::AtLeastRowsAndBytesResult //
StableValueSpace::Snapshot::getAtLeastRowsAndBytes(const DMContext & dm_context, const RowKeyRange & range) const
{
    AtLeastRowsAndBytesResult ret{};

    // Usually, this method will be called for some "cold" key ranges.
    // Loading the index into cache may pollute the cache and make the hot index cache invalid.
    // So don't refill the cache if the index does not exist.
    constexpr bool set_cache_if_miss = false;
    for (size_t file_idx = 0; file_idx < stable->files.size(); ++file_idx)
    {
        const auto & file = stable->files[file_idx];
        auto filter
            = DMFilePackFilter::loadFrom(dm_context, file, set_cache_if_miss, {range}, RSOperatorPtr{}, IdSetPtr{});
        const auto & handle_filter_result = filter->getHandleRes();
        if (file_idx == 0)
        {
            // TODO: this check may not be correct when support multiple files in a stable, let's just keep it now for simplicity
            if (handle_filter_result.empty())
                ret.first_pack_intersection = RSResult::None;
            else
                ret.first_pack_intersection = handle_filter_result.front();
        }
        if (file_idx == stable->files.size() - 1)
        {
            // TODO: this check may not be correct when support multiple files in a stable, let's just keep it now for simplicity
            if (handle_filter_result.empty())
                ret.last_pack_intersection = RSResult::None;
            else
                ret.last_pack_intersection = handle_filter_result.back();
        }

        const auto & pack_stats = file->getPackStats();
        for (size_t pack_idx = 0; pack_idx < pack_stats.size(); ++pack_idx)
        {
            // Only count packs that are fully contained by the range.
            if (handle_filter_result[pack_idx] == RSResult::All)
            {
                ret.rows += pack_stats[pack_idx].rows;
                ret.bytes += pack_stats[pack_idx].bytes;
            }
        }
    }

    return ret;
}

static size_t defaultValueBytes(const Field & f)
{
    switch (f.getType())
    {
    case Field::Types::Decimal32:
        return 4;
    case Field::Types::UInt64:
    case Field::Types::Int64:
    case Field::Types::Float64:
    case Field::Types::Decimal64:
        return 8;
    case Field::Types::UInt128:
    case Field::Types::Int128:
    case Field::Types::Decimal128:
        return 16;
    case Field::Types::Int256:
    case Field::Types::Decimal256:
        return 32;
    case Field::Types::String:
        return f.get<String>().size();
    default: // Null, Array, Tuple. In fact, it should not be Array or Tuple here.
        // But we don't throw exceptions here because it is not the critical path.
        return 1;
    }
}

size_t StableValueSpace::avgRowBytes(const ColumnDefines & read_columns)
{
    size_t avg_bytes = 0;
    if (likely(!files.empty()))
    {
        const auto & file = files.front();
        for (const auto & col : read_columns)
        {
            if (file->isColumnExist(col.id))
            {
                const auto & stat = file->getColumnStat(col.id);
                avg_bytes += stat.avg_size;
            }
            else
            {
                avg_bytes += defaultValueBytes(col.default_value);
            }
        }
    }
    return avg_bytes;
}

} // namespace DB::DM
