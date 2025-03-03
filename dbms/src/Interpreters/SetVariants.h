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

#pragma once

#include <Columns/ColumnNullable.h>
#include <Columns/ColumnString.h>
#include <Common/Arena.h>
#include <Common/ColumnsHashing.h>
#include <Common/HashTable/ClearableHashSet.h>
#include <Common/HashTable/HashSet.h>
#include <Interpreters/AggregationCommon.h>


namespace DB
{
/** Methods for different implementations of sets (used in right hand side of IN or for DISTINCT).
  * To use as template parameter.
  */


/// For the case where there is one numeric key.
template <
    typename FieldType,
    typename TData,
    bool use_cache = true> /// UInt8/16/32/64 for any types with corresponding bit width.
struct SetMethodOneNumber
{
    using Data = TData;
    using Key = typename Data::key_type;

    Data data;

    using State = ColumnsHashing::HashMethodOneNumber<typename Data::value_type, VoidMapped, FieldType, use_cache>;
};

/// For the case where there is one string key.
template <typename TData>
struct SetMethodString
{
    using Data = TData;
    using Key = typename Data::key_type;

    Data data;

    using State = ColumnsHashing::HashMethodString<typename Data::value_type, VoidMapped, false>;
};

template <typename TData, bool padding>
struct SetMethodStringBinNoCache
{
    using Data = TData;
    using Key = typename Data::key_type;

    Data data;

    using State = ColumnsHashing::HashMethodStringBin<typename Data::value_type, VoidMapped, padding>;
};

/// For the case when there is one fixed-length string key.
template <typename TData>
struct SetMethodFixedString
{
    using Data = TData;
    using Key = typename Data::key_type;

    Data data;

    using State = ColumnsHashing::HashMethodFixedString<typename Data::value_type, VoidMapped, false>;
};

namespace set_impl
{
/// This class is designed to provide the functionality that is required for
/// supporting nullable keys in SetMethodKeysFixed. If there are
/// no nullable keys, this class is merely implemented as an empty shell.
template <typename Key, bool has_nullable_keys>
class BaseStateKeysFixed;

/// Case where nullable keys are supported.
template <typename Key>
class BaseStateKeysFixed<Key, true>
{
protected:
    void init(const ColumnRawPtrs & key_columns)
    {
        null_maps.reserve(key_columns.size());
        actual_columns.reserve(key_columns.size());

        for (const auto & col : key_columns)
        {
            if (col->isColumnNullable())
            {
                const auto & nullable_col = static_cast<const ColumnNullable &>(*col);
                actual_columns.push_back(&nullable_col.getNestedColumn());
                null_maps.push_back(&nullable_col.getNullMapColumn());
            }
            else
            {
                actual_columns.push_back(col);
                null_maps.push_back(nullptr);
            }
        }
    }

    /// Return the columns which actually contain the values of the keys.
    /// For a given key column, if it is nullable, we return its nested
    /// column. Otherwise we return the key column itself.
    inline const ColumnRawPtrs & getActualColumns() const { return actual_columns; }

    /// Create a bitmap that indicates whether, for a particular row,
    /// a key column bears a null value or not.
    KeysNullMap<Key> createBitmap(size_t row) const
    {
        KeysNullMap<Key> bitmap{};

        for (size_t k = 0; k < null_maps.size(); ++k)
        {
            if (null_maps[k] != nullptr)
            {
                const auto & null_map = static_cast<const ColumnUInt8 &>(*null_maps[k]).getData();
                if (null_map[row] == 1)
                {
                    size_t bucket = k / 8;
                    size_t offset = k % 8;
                    bitmap[bucket] |= UInt8(1) << offset;
                }
            }
        }

        return bitmap;
    }

private:
    ColumnRawPtrs actual_columns;
    ColumnRawPtrs null_maps;
};

/// Case where nullable keys are not supported.
template <typename Key>
class BaseStateKeysFixed<Key, false>
{
protected:
    void init(const ColumnRawPtrs &)
    {
        throw Exception{
            "Internal error: calling init() for non-nullable"
            " keys is forbidden",
            ErrorCodes::LOGICAL_ERROR};
    }

    const ColumnRawPtrs & getActualColumns() const
    {
        throw Exception{
            "Internal error: calling getActualColumns() for non-nullable"
            " keys is forbidden",
            ErrorCodes::LOGICAL_ERROR};
    }

    KeysNullMap<Key> createBitmap(size_t) const
    {
        throw Exception{
            "Internal error: calling createBitmap() for non-nullable keys"
            " is forbidden",
            ErrorCodes::LOGICAL_ERROR};
    }
};

} // namespace set_impl

/// For the case when all keys are of fixed length, and they fit in N (for example, 128) bits.
template <typename TData, bool has_nullable_keys_ = false>
struct SetMethodKeysFixed
{
    using Data = TData;
    using Key = typename Data::key_type;
    static constexpr bool has_nullable_keys = has_nullable_keys_;

    Data data;

    using State
        = ColumnsHashing::HashMethodKeysFixed<typename Data::value_type, Key, VoidMapped, has_nullable_keys, false>;
};

/// For other cases. 128 bit hash from the key.
template <typename TData>
struct SetMethodHashed
{
    using Data = TData;
    using Key = typename Data::key_type;

    Data data;

    using State = ColumnsHashing::HashMethodHashed<typename Data::value_type, VoidMapped>;
};


/** Different implementations of the set.
  */
struct NonClearableSet
{
    /// TODO Use either bit- or byte-set for these two options.
    std::unique_ptr<SetMethodOneNumber<UInt8, HashSet<UInt8, TrivialHash, HashTableFixedGrower<8>>>> key8;
    std::unique_ptr<SetMethodOneNumber<UInt16, HashSet<UInt16, TrivialHash, HashTableFixedGrower<16>>>> key16;

    /** Also for the experiment was tested the ability to use SmallSet,
      *  as long as the number of elements in the set is small (and, if necessary, converted to a full-fledged HashSet).
      * But this experiment showed that there is an advantage only in rare cases.
      */
    std::unique_ptr<SetMethodOneNumber<UInt32, HashSet<UInt32, HashCRC32<UInt32>>>> key32;
    std::unique_ptr<SetMethodOneNumber<UInt64, HashSet<UInt64, HashCRC32<UInt64>>>> key64;
    std::unique_ptr<SetMethodString<HashSetWithSavedHash<StringRef>>> key_string;
    std::unique_ptr<SetMethodStringBinNoCache<HashSetWithSavedHash<StringRef>, true>> key_strbinpadding;
    std::unique_ptr<SetMethodStringBinNoCache<HashSetWithSavedHash<StringRef>, false>> key_strbin;
    std::unique_ptr<SetMethodFixedString<HashSetWithSavedHash<StringRef>>> key_fixed_string;
    std::unique_ptr<SetMethodKeysFixed<HashSet<UInt128, HashCRC32<UInt128>>>> keys128;
    std::unique_ptr<SetMethodKeysFixed<HashSet<UInt256, HashCRC32<UInt256>>>> keys256;
    std::unique_ptr<SetMethodHashed<HashSet<UInt128, TrivialHash>>> hashed;

    /// Support for nullable keys (for DISTINCT implementation).
    std::unique_ptr<SetMethodKeysFixed<HashSet<UInt128, HashCRC32<UInt128>>, true>> nullable_keys128;
    std::unique_ptr<SetMethodKeysFixed<HashSet<UInt256, HashCRC32<UInt256>>, true>> nullable_keys256;
    /** Unlike Aggregator, `concat` method is not used here.
      * This is done because `hashed` method, although slower, but in this case, uses less RAM.
      *  since when you use it, the key values themselves are not stored.
      */
};

struct ClearableSet
{
    /// TODO Use either bit- or byte-set for these two options.
    std::unique_ptr<SetMethodOneNumber<UInt8, ClearableHashSet<UInt8, TrivialHash, HashTableFixedGrower<8>>>> key8;
    std::unique_ptr<SetMethodOneNumber<UInt16, ClearableHashSet<UInt16, TrivialHash, HashTableFixedGrower<16>>>> key16;

    std::unique_ptr<SetMethodOneNumber<UInt32, ClearableHashSet<UInt32, HashCRC32<UInt32>>>> key32;
    std::unique_ptr<SetMethodOneNumber<UInt64, ClearableHashSet<UInt64, HashCRC32<UInt64>>>> key64;
    std::unique_ptr<SetMethodString<ClearableHashSetWithSavedHash<StringRef>>> key_string;
    std::unique_ptr<SetMethodStringBinNoCache<ClearableHashSetWithSavedHash<StringRef>, true>> key_strbinpadding;
    std::unique_ptr<SetMethodStringBinNoCache<ClearableHashSetWithSavedHash<StringRef>, false>> key_strbin;
    std::unique_ptr<SetMethodFixedString<ClearableHashSetWithSavedHash<StringRef>>> key_fixed_string;
    std::unique_ptr<SetMethodKeysFixed<ClearableHashSet<UInt128, HashCRC32<UInt128>>>> keys128;
    std::unique_ptr<SetMethodKeysFixed<ClearableHashSet<UInt256, HashCRC32<UInt256>>>> keys256;
    std::unique_ptr<SetMethodHashed<ClearableHashSet<UInt128, TrivialHash>>> hashed;

    /// Support for nullable keys (for DISTINCT implementation).
    std::unique_ptr<SetMethodKeysFixed<ClearableHashSet<UInt128, HashCRC32<UInt128>>, true>> nullable_keys128;
    std::unique_ptr<SetMethodKeysFixed<ClearableHashSet<UInt256, HashCRC32<UInt256>>, true>> nullable_keys256;
    /** Unlike Aggregator, `concat` method is not used here.
      * This is done because `hashed` method, although slower, but in this case, uses less RAM.
      *  since when you use it, the key values themselves are not stored.
      */
};

template <typename Variant>
struct SetVariantsTemplate : public Variant
{
    Arena string_pool;

#define APPLY_FOR_SET_VARIANTS(M) \
    M(key8)                       \
    M(key16)                      \
    M(key32)                      \
    M(key64)                      \
    M(key_string)                 \
    M(key_strbinpadding)          \
    M(key_strbin)                 \
    M(key_fixed_string)           \
    M(keys128)                    \
    M(keys256)                    \
    M(nullable_keys128)           \
    M(nullable_keys256)           \
    M(hashed)

#define M(NAME) using Variant::NAME;
    APPLY_FOR_SET_VARIANTS(M)
#undef M

    enum class Type
    {
        EMPTY,

#define M(NAME) NAME,
        APPLY_FOR_SET_VARIANTS(M)
#undef M
    };

    Type type = Type::EMPTY;

    bool empty() const { return type == Type::EMPTY; }

    static Type chooseMethod(
        const ColumnRawPtrs & key_columns,
        Sizes & key_sizes,
        const TiDB::TiDBCollators & collators = {});

    void init(Type type_);

    size_t getTotalRowCount() const;
    /// Counts the size in bytes of the Set buffer and the size of the `string_pool`
    size_t getTotalByteCount() const;
};

using SetVariants = SetVariantsTemplate<NonClearableSet>;
using ClearableSetVariants = SetVariantsTemplate<ClearableSet>;

} // namespace DB
