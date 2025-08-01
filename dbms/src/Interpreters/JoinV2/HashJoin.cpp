// Copyright 2024 PingCAP, Inc.
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

#include <Columns/ColumnUtils.h>
#include <Common/Exception.h>
#include <Common/FailPoint.h>
#include <Common/Stopwatch.h>
#include <Core/ColumnsWithTypeAndName.h>
#include <DataStreams/materializeBlock.h>
#include <DataTypes/DataTypeNullable.h>
#include <Interpreters/JoinUtils.h>
#include <Interpreters/JoinV2/HashJoin.h>
#include <Interpreters/JoinV2/HashJoinProbe.h>
#include <Interpreters/NullableUtils.h>
#include <Interpreters/Settings.h>

#include <ext/scope_guard.h>
#include <magic_enum.hpp>
#include <memory>

namespace DB
{
namespace FailPoints
{
extern const char random_join_prob_failpoint[];
extern const char exception_mpp_hash_build[];
extern const char exception_mpp_hash_probe[];
extern const char force_join_v2_probe_enable_lm[];
extern const char force_join_v2_probe_disable_lm[];
} // namespace FailPoints

namespace
{
struct KeyColumn
{
    const IColumn * column_ptr;
    bool is_nullable;
};
std::vector<KeyColumn> getKeyColumns(const Names & key_names, const Block & block)
{
    size_t keys_size = key_names.size();
    std::vector<KeyColumn> key_columns(keys_size);

    for (size_t i = 0; i < keys_size; ++i)
    {
        key_columns[i].column_ptr = block.getByName(key_names[i]).column.get();

        /// We will join only keys, where all components are not NULL.
        if (key_columns[i].column_ptr->isColumnNullable())
        {
            key_columns[i].column_ptr
                = &static_cast<const ColumnNullable &>(*key_columns[i].column_ptr).getNestedColumn();
            key_columns[i].is_nullable = true;
        }
    }

    return key_columns;
}

bool canAsColumnString(const IColumn * column)
{
    return typeid_cast<const ColumnString *>(column)
        || (column->isColumnConst()
            && typeid_cast<const ColumnString *>(&static_cast<const ColumnConst *>(column)->getDataColumn()));
}

enum class StringCollatorKind
{
    StringBinary,
    StringBinaryPadding,
    String,
};

StringCollatorKind getStringCollatorKind(const TiDB::TiDBCollators & collators)
{
    if (collators.empty() || !collators[0])
        return StringCollatorKind::StringBinary;

    switch (collators[0]->getCollatorType())
    {
    case TiDB::ITiDBCollator::CollatorType::UTF8MB4_BIN:
    case TiDB::ITiDBCollator::CollatorType::UTF8_BIN:
    case TiDB::ITiDBCollator::CollatorType::LATIN1_BIN:
    case TiDB::ITiDBCollator::CollatorType::ASCII_BIN:
    {
        return StringCollatorKind::StringBinaryPadding;
    }
    case TiDB::ITiDBCollator::CollatorType::BINARY:
    {
        return StringCollatorKind::StringBinary;
    }
    default:
    {
        // for CI COLLATION, use original way
        return StringCollatorKind::String;
    }
    }
}

} // namespace

const DataTypePtr HashJoin::match_helper_type = makeNullable(std::make_shared<DataTypeInt8>());

HashJoin::HashJoin(
    const Names & key_names_left_,
    const Names & key_names_right_,
    ASTTableJoin::Kind kind_,
    const String & req_id,
    const NamesAndTypes & output_columns_,
    const TiDB::TiDBCollators & collators_,
    const JoinNonEqualConditions & non_equal_conditions_,
    const Settings & settings_,
    const String & match_helper_name_)
    : kind(kind_)
    , join_req_id(req_id)
    , key_names_left(key_names_left_)
    , key_names_right(key_names_right_)
    , collators(collators_)
    , non_equal_conditions(non_equal_conditions_)
    , settings(settings_)
    , match_helper_name(match_helper_name_)
    , log(Logger::get(join_req_id))
    , has_other_condition(non_equal_conditions.other_cond_expr != nullptr)
    , output_columns(output_columns_)
{
    RUNTIME_ASSERT(key_names_left.size() == key_names_right.size());
    output_block = Block(output_columns);
}

void HashJoin::initRowLayoutAndHashJoinMethod()
{
    size_t keys_size = key_names_right.size();
    if (keys_size == 0)
    {
        method = HashJoinKeyMethod::Cross;
        return;
    }

    auto key_columns = getKeyColumns(key_names_right, right_sample_block);
    RUNTIME_ASSERT(key_columns.size() == keys_size);

    bool is_all_key_fixed = true;
    bool has_decimal_256 = false;
    for (size_t i = 0; i < keys_size; ++i)
    {
        if (typeid_cast<const ColumnDecimal<Decimal256> *>(key_columns[i].column_ptr))
        {
            has_decimal_256 = true;
            continue;
        }
        if (key_columns[i].column_ptr->valuesHaveFixedSize())
            row_layout.key_column_fixed_size += key_columns[i].column_ptr->sizeOfValueIfFixed();
        else
            is_all_key_fixed = false;
    }
    if (has_decimal_256)
    {
        method = HashJoinKeyMethod::KeySerialized;
    }
    else if (is_all_key_fixed)
    {
        method = findFixedSizeJoinKeyMethod(keys_size, row_layout.key_column_fixed_size);
    }
    else if (keys_size == 1 && canAsColumnString(key_columns[0].column_ptr))
    {
        switch (getStringCollatorKind(collators))
        {
        case StringCollatorKind::StringBinary:
            method = HashJoinKeyMethod::OneKeyStringBin;
            break;
        case StringCollatorKind::StringBinaryPadding:
            method = HashJoinKeyMethod::OneKeyStringBinPadding;
            break;
        case StringCollatorKind::String:
            method = HashJoinKeyMethod::OneKeyString;
            break;
        }
    }
    else
    {
        method = HashJoinKeyMethod::KeySerialized;
    }

    std::unordered_set<size_t> raw_required_key_index_set;
    if (method != HashJoinKeyMethod::KeySerialized)
    {
        /// Move all raw required join key column to the end of the join key.
        Names new_key_names_left, new_key_names_right;
        BoolVec raw_required_key_flag(keys_size);
        for (size_t i = 0; i < keys_size; ++i)
        {
            bool is_raw_required = false;
            if (key_columns[i].column_ptr->valuesHaveFixedSize())
            {
                if (right_sample_block_pruned.has(key_names_right[i]))
                    is_raw_required = true;
            }
            else
            {
                if (canAsColumnString(key_columns[i].column_ptr)
                    && getStringCollatorKind(collators) == StringCollatorKind::StringBinary
                    && right_sample_block_pruned.has(key_names_right[i]))
                {
                    is_raw_required = true;
                }
            }
            if (is_raw_required)
            {
                size_t index = right_sample_block_pruned.getPositionByName(key_names_right[i]);
                /// If this index has already existed in set, do not move it to the end of the join key.
                if (!raw_required_key_index_set.contains(index))
                {
                    raw_required_key_flag[i] = true;
                    raw_required_key_index_set.insert(index);
                    row_layout.raw_key_column_indexes.push_back({index, key_columns[i].is_nullable});
                    continue;
                }
            }
            new_key_names_left.emplace_back(key_names_left[i]);
            new_key_names_right.emplace_back(key_names_right[i]);
        }

        for (size_t i = 0; i < keys_size; ++i)
        {
            if (raw_required_key_flag[i])
            {
                new_key_names_left.emplace_back(key_names_left[i]);
                new_key_names_right.emplace_back(key_names_right[i]);
            }
        }
        key_names_left.swap(new_key_names_left);
        key_names_right.swap(new_key_names_right);
    }

    row_layout.other_column_count_for_other_condition = 0;
    size_t columns = right_sample_block_pruned.columns();
    BoolVec required_columns_flag(columns);
    for (size_t i = 0; i < columns; ++i)
    {
        if (raw_required_key_index_set.contains(i))
        {
            required_columns_flag[i] = true;
            continue;
        }
        auto & c = right_sample_block_pruned.getByPosition(i);
        if (required_columns_names_set_for_other_condition.contains(c.name))
        {
            ++row_layout.other_column_count_for_other_condition;
            required_columns_flag[i] = true;
            if (c.column->valuesHaveFixedSize())
            {
                row_layout.other_column_fixed_size += c.column->sizeOfValueIfFixed();
                row_layout.other_column_indexes.push_back({i, true});
            }
            else
            {
                row_layout.other_column_indexes.push_back({i, false});
            }
        }
    }
    for (size_t i = 0; i < columns; ++i)
    {
        if (required_columns_flag[i])
            continue;
        auto & c = right_sample_block_pruned.getByPosition(i);
        if (c.column->valuesHaveFixedSize())
        {
            row_layout.other_column_fixed_size += c.column->sizeOfValueIfFixed();
            row_layout.other_column_indexes.push_back({i, true});
        }
        else
        {
            row_layout.other_column_indexes.push_back({i, false});
        }
        RUNTIME_CHECK_MSG(
            output_block_after_finalize.has(c.name),
            "output_block_after_finalize does not contain {}",
            c.name);
    }
    RUNTIME_CHECK(row_layout.raw_key_column_indexes.size() + row_layout.other_column_indexes.size() == columns);
    for (auto [column_index, is_nullable] : row_layout.raw_key_column_indexes)
        RUNTIME_CHECK(
            right_sample_block_pruned.safeGetByPosition(column_index).column->isColumnNullable() == is_nullable);
}

void HashJoin::initBuild(const Block & sample_block, size_t build_concurrency_)
{
    RUNTIME_CHECK_MSG(!build_initialized, "Logical error: Join build has been initialized");
    RUNTIME_CHECK_MSG(isFinalize(), "join should be finalized first");

    right_sample_block = materializeBlock(sample_block);

    /// In case of LEFT and FULL joins, convert joined columns to Nullable.
    if (isLeftOuterJoin(kind) || kind == ASTTableJoin::Kind::Full)
    {
        size_t columns = right_sample_block.columns();
        for (size_t i = 0; i < columns; ++i)
            convertColumnToNullable(right_sample_block.getByPosition(i));
    }

    right_sample_block_pruned = right_sample_block;
    removeUselessColumn(right_sample_block_pruned);

    initRowLayoutAndHashJoinMethod();

    build_concurrency = build_concurrency_;
    active_build_worker = build_concurrency;
    build_workers_data.resize(build_concurrency);
    for (size_t i = 0; i < build_concurrency; ++i)
        build_workers_data[i].key_getter = createHashJoinKeyGetter(method, collators);
    for (size_t i = 0; i < JOIN_BUILD_PARTITION_COUNT + 1; ++i)
        multi_row_containers.emplace_back(std::make_unique<MultipleRowContainer>());

    build_initialized = true;
}

void HashJoin::initProbe(const Block & sample_block, size_t probe_concurrency_)
{
    RUNTIME_CHECK_MSG(build_initialized, "join build should be initialized first");
    RUNTIME_CHECK_MSG(!probe_initialized, "Logical error: Join probe has been initialized");
    RUNTIME_CHECK_MSG(isFinalize(), "join should be finalized first");

    left_sample_block = materializeBlock(sample_block);

    /// In case of RIGHT and FULL joins, convert left columns to Nullable.
    if (getFullness(kind))
    {
        size_t columns = left_sample_block.columns();
        for (size_t i = 0; i < columns; ++i)
            convertColumnToNullable(left_sample_block.getByPosition(i));
    }

    left_sample_block_pruned = left_sample_block;
    removeUselessColumn(left_sample_block_pruned);

    all_sample_block_pruned = left_sample_block_pruned.cloneEmpty();
    size_t right_columns = right_sample_block_pruned.columns();
    for (size_t i = 0; i < right_columns; ++i)
    {
        ColumnWithTypeAndName new_column = right_sample_block_pruned.safeGetByPosition(i).cloneEmpty();
        RUNTIME_CHECK_MSG(
            !all_sample_block_pruned.has(new_column.name),
            "block from probe side has a column with the same name: {} as a column in right_sample_block_pruned",
            new_column.name);

        all_sample_block_pruned.insert(std::move(new_column));
    }

    size_t all_columns = all_sample_block_pruned.columns();
    output_column_indexes.reserve(all_columns);
    size_t output_columns = 0;
    for (size_t i = 0; i < all_columns; ++i)
    {
        ssize_t output_index = -1;
        const auto & name = all_sample_block_pruned.safeGetByPosition(i).name;
        if (output_block_after_finalize.has(name))
        {
            output_index = output_block_after_finalize.getPositionByName(name);
            ++output_columns;
        }
        output_column_indexes.push_back(output_index);
    }
    if (isLeftOuterSemiFamily(kind))
    {
        RUNTIME_CHECK_MSG(
            output_columns + 1 == output_block_after_finalize.columns(),
            "output columns {} in all_sample_block_pruned + 1 != columns {} in output_block_after_finalize",
            output_columns,
            output_block_after_finalize.columns());
        RUNTIME_CHECK_MSG(
            output_block_after_finalize.has(match_helper_name),
            "output_block_after_finalize does not have {} for join kind {}",
            match_helper_name,
            magic_enum::enum_name(kind));

        RUNTIME_CHECK(output_block_after_finalize.getByName(match_helper_name).type->equals(*match_helper_type));
    }
    else
    {
        RUNTIME_CHECK_MSG(
            output_columns == output_block_after_finalize.columns(),
            "output columns {} in all_sample_block_pruned != columns {} in output_block_after_finalize",
            output_columns,
            output_block_after_finalize.columns());
    }

    if (has_other_condition)
    {
        left_required_flag_for_other_condition.resize(left_sample_block_pruned.columns());
        for (const auto & name : required_columns_names_set_for_other_condition)
        {
            RUNTIME_CHECK_MSG(
                all_sample_block_pruned.has(name),
                "all_sample_block_pruned should have {} in required_columns_names_set_for_other_condition",
                name);
            if (!left_sample_block_pruned.has(name))
                continue;

            left_required_flag_for_other_condition[left_sample_block_pruned.getPositionByName(name)] = true;
        }
    }

    probe_concurrency = probe_concurrency_;
    active_probe_worker = probe_concurrency;
    probe_workers_data.resize(probe_concurrency);

    probe_initialized = true;
}

bool HashJoin::finishOneBuildRow(size_t stream_index)
{
    auto & wd = build_workers_data[stream_index];
    LOG_DEBUG(
        log,
        "{} insert block to row containers cost {}ms, row count {}, padding size {}({:.2f}% of all size {})",
        stream_index,
        wd.build_time,
        wd.row_count,
        wd.padding_size,
        100.0 * wd.padding_size / wd.all_size,
        wd.all_size);
    if (active_build_worker.fetch_sub(1) == 1)
    {
        FAIL_POINT_TRIGGER_EXCEPTION(FailPoints::exception_mpp_hash_build);
        workAfterBuildRowFinish();
        return true;
    }
    return false;
}

bool HashJoin::finishOneProbe(size_t stream_index)
{
    auto & wd = probe_workers_data[stream_index];
    LOG_DEBUG(
        log,
        "{} probe handle {} rows, cost {}ms(hash_table {}ms + replicate {}ms + other condition {}ms), collision {}",
        stream_index,
        wd.probe_handle_rows,
        wd.probe_time / 1000000UL,
        wd.probe_hash_table_time / 1000000UL,
        wd.replicate_time / 1000000UL,
        wd.other_condition_time / 1000000UL,
        wd.collision);
    if (active_probe_worker.fetch_sub(1) == 1)
    {
        FAIL_POINT_TRIGGER_EXCEPTION(FailPoints::exception_mpp_hash_probe);
        return true;
    }
    return false;
}

void HashJoin::workAfterBuildRowFinish()
{
    size_t all_build_row_count = 0;
    for (size_t i = 0; i < build_concurrency; ++i)
        all_build_row_count += build_workers_data[i].row_count;

    bool enable_tagged_pointer = settings.enable_tagged_pointer;
    for (size_t i = 0; i < build_concurrency; ++i)
        enable_tagged_pointer &= build_workers_data[i].enable_tagged_pointer;

    pointer_table.init(
        method,
        all_build_row_count,
        getHashValueByteSize(method),
        settings.probe_enable_prefetch_threshold,
        enable_tagged_pointer,
        false);

    /// Conservative threshold: trigger late materialization when lm_row_size average >= 16 bytes.
    constexpr size_t trigger_lm_row_size_threshold = 16;
    bool late_materialization = false;
    size_t avg_lm_row_size = 0;
    if (has_other_condition
        && row_layout.other_column_count_for_other_condition < row_layout.other_column_indexes.size())
    {
        size_t total_lm_row_size = 0;
        size_t total_lm_row_count = 0;
        for (size_t i = 0; i < build_concurrency; ++i)
        {
            total_lm_row_size += build_workers_data[i].lm_row_size;
            total_lm_row_count += build_workers_data[i].lm_row_count;
        }
        avg_lm_row_size = total_lm_row_count == 0 ? 0 : total_lm_row_size / total_lm_row_count;
        late_materialization = avg_lm_row_size >= trigger_lm_row_size_threshold;
    }
    fiu_do_on(FailPoints::force_join_v2_probe_enable_lm, { late_materialization = true; });
    fiu_do_on(FailPoints::force_join_v2_probe_disable_lm, { late_materialization = false; });

    if (SemiJoinProbeHelper::isSupported(kind, has_other_condition))
        semi_join_probe_helper = std::make_unique<SemiJoinProbeHelper>(this);
    else
        join_probe_helper = std::make_unique<JoinProbeHelper>(this, late_materialization);

    LOG_INFO(
        log,
        "finish build row and allocate pointer table, rows {}, pointer table size {}, enable (prefetch {}, tagged "
        "pointer {}, lm {}(avg size {}))",
        all_build_row_count,
        pointer_table.getPointerTableSize(),
        pointer_table.enableProbePrefetch(),
        pointer_table.enableTaggedPointer(),
        late_materialization,
        avg_lm_row_size);
}

void HashJoin::buildRowFromBlock(const Block & b, size_t stream_index)
{
    RUNTIME_ASSERT(stream_index < build_concurrency);
    RUNTIME_CHECK_MSG(build_initialized, "Logical error: Join build was not initialized");

    if unlikely (b.rows() == 0)
        return;

    Stopwatch watch;

    Block block = b;
    size_t rows = block.rows();

    /// Rare case, when keys are constant. To avoid code bloat, simply materialize them.
    /// Note: this variable can't be removed because it will take smart pointers' lifecycle to the end of this function.
    Columns materialized_columns;
    ColumnRawPtrs key_columns = extractAndMaterializeKeyColumns(block, materialized_columns, key_names_right);

    /// We will insert to the map only keys, where all components are not NULL.
    ColumnPtr null_map_holder;
    ConstNullMapPtr null_map{};
    extractNestedColumnsAndNullMap(key_columns, null_map_holder, null_map);
    /// Reuse null_map to record the filtered rows, the rows contains NULL or does not
    /// match the join filter will not insert to the maps
    recordFilteredRows(block, non_equal_conditions.right_filter_column, null_map_holder, null_map);
    /// Some useless columns maybe key columns and filter column so they must be removed after extracting key columns and filter column.
    removeUselessColumn(block);

    /// Rare case, when joined columns are constant. To avoid code bloat, simply materialize them.
    block = materializeBlock(block);

    /// In case of LEFT and FULL joins, convert joined columns to Nullable.
    if (isLeftOuterJoin(kind) || kind == ASTTableJoin::Kind::Full)
    {
        size_t columns = block.columns();
        for (size_t i = 0; i < columns; ++i)
            convertColumnToNullable(block.getByPosition(i));
    }

    assertBlocksHaveEqualStructure(block, right_sample_block_pruned, "Join Build");

    bool check_lm_row_size = has_other_condition
        && row_layout.other_column_count_for_other_condition < row_layout.other_column_indexes.size();
    insertBlockToRowContainers(
        method,
        needRecordNotInsertRows(kind),
        block,
        rows,
        key_columns,
        null_map,
        row_layout,
        multi_row_containers,
        build_workers_data[stream_index],
        check_lm_row_size);

    build_workers_data[stream_index].build_time += watch.elapsedMilliseconds();
}

bool HashJoin::buildPointerTable(size_t stream_index)
{
    bool is_end;
    switch (method)
    {
#define M(METHOD)                                                                          \
    case HashJoinKeyMethod::METHOD:                                                        \
        using KeyGetterType##METHOD = HashJoinKeyGetterForType<HashJoinKeyMethod::METHOD>; \
        if constexpr (KeyGetterType##METHOD::Type::joinKeyCompareHashFirst())              \
            is_end = pointer_table.build<KeyGetterType##METHOD::HashValueType>(            \
                build_workers_data[stream_index],                                          \
                multi_row_containers,                                                      \
                settings.max_block_size);                                                  \
        else                                                                               \
            is_end = pointer_table.build<void>(                                            \
                build_workers_data[stream_index],                                          \
                multi_row_containers,                                                      \
                settings.max_block_size);                                                  \
        break;
        APPLY_FOR_HASH_JOIN_VARIANTS(M)
#undef M

    default:
        throw Exception(
            fmt::format("Unknown JOIN keys variant {}.", magic_enum::enum_name(method)),
            ErrorCodes::UNKNOWN_SET_DATA_VARIANT);
    }

    if (is_end)
    {
        auto & wd = build_workers_data[stream_index];
        LOG_DEBUG(
            log,
            "{} build pointer table finish cost {}ms, build rows {}",
            stream_index,
            wd.build_pointer_table_time,
            wd.build_pointer_table_size);
    }
    return is_end;
}

Block HashJoin::probeBlock(JoinProbeContext & ctx, size_t stream_index)
{
    RUNTIME_ASSERT(stream_index < probe_concurrency);
    RUNTIME_CHECK_MSG(probe_initialized, "Logical error: Join probe was not initialized");

    Stopwatch all_watch;
    SCOPE_EXIT({ probe_workers_data[stream_index].probe_time += all_watch.elapsedFromLastTime(); });

    const NameSet & probe_output_name_set = has_other_condition
        ? output_columns_names_set_for_other_condition_after_finalize
        : output_column_names_set_after_finalize;
    ctx.prepareForHashProbe(
        method,
        kind,
        has_other_condition,
        !non_equal_conditions.other_eq_cond_from_in_name.empty(),
        key_names_left,
        non_equal_conditions.left_filter_column,
        probe_output_name_set,
        left_sample_block_pruned,
        collators,
        row_layout);

    FAIL_POINT_TRIGGER_EXCEPTION(FailPoints::random_join_prob_failpoint);

    auto & wd = probe_workers_data[stream_index];
    Block res;
    if (semi_join_probe_helper)
        res = semi_join_probe_helper->probe(ctx, wd);
    else
        res = join_probe_helper->probe(ctx, wd);
    if (ctx.isAllFinished())
        wd.probe_handle_rows += ctx.rows;
    return res;
}

Block HashJoin::probeLastResultBlock(size_t stream_index)
{
    auto & wd = probe_workers_data[stream_index];
    if (has_other_condition)
        return std::move(wd.result_block_for_other_condition);

    if (wd.result_block)
    {
        auto res_block = removeUselessColumnForOutput(wd.result_block);
        wd.result_block = {};
        return res_block;
    }
    return {};
}

void HashJoin::removeUselessColumn(Block & block) const
{
    const NameSet & probe_output_name_set = has_other_condition
        ? output_columns_names_set_for_other_condition_after_finalize
        : output_column_names_set_after_finalize;
    for (size_t pos = 0; pos < block.columns();)
    {
        if (!probe_output_name_set.contains(block.getByPosition(pos).name))
            block.erase(pos);
        else
            ++pos;
    }
}

Block HashJoin::removeUselessColumnForOutput(const Block & block) const
{
    RUNTIME_CHECK(probe_initialized);
    RUNTIME_CHECK(block.columns() == all_sample_block_pruned.columns());
    Block output_block = output_block_after_finalize.cloneEmpty();
    size_t columns = block.columns();
    for (size_t i = 0; i < columns; ++i)
    {
        if (output_column_indexes[i] == -1)
            continue;
        output_block.safeGetByPosition(output_column_indexes[i]) = block.safeGetByPosition(i);
    }
    return output_block;
}

void HashJoin::initOutputBlock(Block & block) const
{
    if (!block)
    {
        size_t output_columns = output_block_after_finalize.columns();
        for (size_t i = 0; i < output_columns; ++i)
        {
            ColumnWithTypeAndName new_column = output_block_after_finalize.getByPosition(i).cloneEmpty();
            new_column.column->assumeMutable()->reserveAlign(settings.max_block_size, FULL_VECTOR_SIZE_AVX2);
            block.insert(std::move(new_column));
        }
    }
}

void HashJoin::finalize(const Names & parent_require)
{
    if unlikely (finalized)
        return;
    /// finalize will do 3 things
    /// 1. update expected_output_schema
    /// 2. set expected_output_schema_for_other_condition
    /// 3. generated needed input columns
    NameSet required_names_set;
    for (const auto & name : parent_require)
        required_names_set.insert(name);
    if unlikely (!match_helper_name.empty() && !required_names_set.contains(match_helper_name))
    {
        /// should only happen in some tests
        required_names_set.insert(match_helper_name);
    }
    for (const auto & name_and_type : output_columns)
    {
        if (required_names_set.contains(name_and_type.name))
        {
            output_columns_after_finalize.push_back(name_and_type);
            output_column_names_set_after_finalize.insert(name_and_type.name);
        }
    }
    RUNTIME_CHECK_MSG(
        output_column_names_set_after_finalize.size() == output_columns_after_finalize.size(),
        "Logical error, the output of join contains duplicated columns");

    output_block_after_finalize = Block(output_columns_after_finalize);
    Names updated_require;
    if (match_helper_name.empty())
        updated_require = parent_require;
    else
    {
        required_names_set.erase(match_helper_name);
        for (const auto & name : required_names_set)
            updated_require.push_back(name);
    }
    if (!non_equal_conditions.null_aware_eq_cond_name.empty())
    {
        updated_require.push_back(non_equal_conditions.null_aware_eq_cond_name);
    }
    if (!non_equal_conditions.other_eq_cond_from_in_name.empty())
        updated_require.push_back(non_equal_conditions.other_eq_cond_from_in_name);
    if (!non_equal_conditions.other_cond_name.empty())
        updated_require.push_back(non_equal_conditions.other_cond_name);
    /// join will reuse the input columns so need to let finalize keep the input columns
    if (non_equal_conditions.null_aware_eq_cond_expr != nullptr)
    {
        non_equal_conditions.null_aware_eq_cond_expr->finalize(updated_require, true);
        updated_require = non_equal_conditions.null_aware_eq_cond_expr->getRequiredColumns();
    }
    if (non_equal_conditions.other_cond_expr != nullptr)
    {
        non_equal_conditions.other_cond_expr->finalize(updated_require, true);
        updated_require = non_equal_conditions.other_cond_expr->getRequiredColumns();
    }

    if (non_equal_conditions.other_cond_expr != nullptr || non_equal_conditions.null_aware_eq_cond_expr != nullptr)
    {
        output_columns_names_set_for_other_condition_after_finalize = output_column_names_set_after_finalize;
        for (const auto & name : updated_require)
            output_columns_names_set_for_other_condition_after_finalize.insert(name);
        if (!match_helper_name.empty())
            output_columns_names_set_for_other_condition_after_finalize.insert(match_helper_name);

        auto update_required_columns_names_set = [&](const ExpressionActionsPtr & expr) {
            for (const auto & action : expr->getActions())
            {
                Names needed_columns = action.getNeededColumns();
                for (const auto & name : needed_columns)
                {
                    if (output_columns_names_set_for_other_condition_after_finalize.contains(name))
                        required_columns_names_set_for_other_condition.insert(name);
                }
            }
        };

        if (non_equal_conditions.other_cond_expr != nullptr)
            update_required_columns_names_set(non_equal_conditions.other_cond_expr);

        if (non_equal_conditions.null_aware_eq_cond_expr != nullptr)
            update_required_columns_names_set(non_equal_conditions.null_aware_eq_cond_expr);
    }

    /// remove duplicated column
    required_names_set.clear();
    for (const auto & name : updated_require)
        required_names_set.insert(name);
    /// add some internal used columns
    if (!non_equal_conditions.left_filter_column.empty())
        required_names_set.insert(non_equal_conditions.left_filter_column);
    if (!non_equal_conditions.right_filter_column.empty())
        required_names_set.insert(non_equal_conditions.right_filter_column);
    /// add join key to required_columns
    for (const auto & name : key_names_right)
        required_names_set.insert(name);
    for (const auto & name : key_names_left)
        required_names_set.insert(name);

    for (const auto & name : required_names_set)
        required_columns.push_back(name);
    finalized = true;
}

} // namespace DB
