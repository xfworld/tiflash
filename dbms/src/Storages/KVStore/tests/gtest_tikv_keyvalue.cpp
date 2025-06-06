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

#include <Common/config.h> // for ENABLE_NEXT_GEN
#include <Debug/MockKVStore/MockUtils.h>
#include <IO/VarInt.h>
#include <Storages/KVStore/Decode/TiKVHelper.h>
#include <Storages/KVStore/Decode/TiKVRange.h>
#include <Storages/KVStore/Region.h>
#include <Storages/KVStore/TiKVHelpers/TiKVRecordFormat.h>
#include <Storages/KVStore/Types.h>
#include <TestUtils/TiFlashTestBasic.h>
#include <TiDB/Schema/TiDB.h>

using namespace DB;

using RangeRef = std::pair<const TiKVKey &, const TiKVKey &>;

inline bool checkTableInvolveRange(const TableID table_id, const RangeRef & range)
{
    const TiKVKey start_key = RecordKVFormat::genKey(table_id, std::numeric_limits<HandleID>::min());
    const TiKVKey end_key = RecordKVFormat::genKey(table_id, std::numeric_limits<HandleID>::max());
    // clang-format off
    return !(end_key < range.first|| (!range.second.empty() && start_key >= range.second)); // NOLINT(readability-simplify-boolean-expr)
    // clang-format on
}

inline TiKVKey genIndex(const TableID tableId, const Int64 id)
{
    String key(19, 0);
    memcpy(key.data(), &RecordKVFormat::TABLE_PREFIX, 1);
    auto big_endian_table_id = RecordKVFormat::encodeInt64(tableId);
    memcpy(key.data() + 1, reinterpret_cast<const char *>(&big_endian_table_id), 8);
    memcpy(key.data() + 1 + 8, "_i", 2);
    auto big_endian_handle_id = RecordKVFormat::encodeInt64(id);
    memcpy(key.data() + 1 + 8 + 2, reinterpret_cast<const char *>(&big_endian_handle_id), 8);
    return RecordKVFormat::encodeAsTiKVKey(key);
}

TEST(TiKVKeyValueTest, KeyFormat)
{
    Timestamp prewrite_ts = 5;
    {
        std::string short_value(128, 'F');
        auto v = RecordKVFormat::encodeWriteCfValue(
            RecordKVFormat::CFModifyFlag::PutFlag,
            prewrite_ts,
            short_value,
            false);
        auto decoded = RecordKVFormat::decodeWriteCfValue(v);
        ASSERT_TRUE(decoded.has_value());
        ASSERT_EQ(decoded->write_type, RecordKVFormat::CFModifyFlag::PutFlag);
        ASSERT_EQ(decoded->prewrite_ts, prewrite_ts);
        ASSERT_NE(decoded->short_value, nullptr);
        ASSERT_EQ(*decoded->short_value, short_value);
    }
#if ENABLE_NEXT_GEN
    {
        // For serverless branch, the short_value length use varUInt
        std::string short_value(1025, 'F');
        auto v = RecordKVFormat::encodeWriteCfValue(
            RecordKVFormat::CFModifyFlag::PutFlag,
            prewrite_ts,
            short_value,
            false);
        auto decoded = RecordKVFormat::decodeWriteCfValue(v);
        ASSERT_TRUE(decoded.has_value());
        ASSERT_EQ(decoded->write_type, RecordKVFormat::CFModifyFlag::PutFlag);
        ASSERT_EQ(decoded->prewrite_ts, prewrite_ts);
        ASSERT_NE(decoded->short_value, nullptr);
        ASSERT_EQ(*decoded->short_value, short_value);
    }
#endif
}

TEST(TiKVKeyValueTest, PortedTests)
{
    {
        ASSERT_TRUE(RecordKVFormat::genKey(100, 2) < RecordKVFormat::genKey(100, 3));
        ASSERT_TRUE(RecordKVFormat::genKey(100, 2) < RecordKVFormat::genKey(101, 2));
        ASSERT_TRUE(RecordKVFormat::genKey(100, 2) <= RecordKVFormat::genKey(100, 2));
        ASSERT_TRUE(RecordKVFormat::genKey(100, 2) <= RecordKVFormat::genKey(100, 2, 233));
        ASSERT_TRUE(RecordKVFormat::genKey(100, 2) < RecordKVFormat::genKey(100, 3, 233));
        ASSERT_TRUE(RecordKVFormat::genKey(100, 3) > RecordKVFormat::genKey(100, 2, 233));
        ASSERT_TRUE(RecordKVFormat::genKey(100, 2, 2) < RecordKVFormat::genKey(100, 3));
    }

    {
        auto key = RecordKVFormat::genKey(2222, 123, 992134);
        ASSERT_EQ(2222, RecordKVFormat::getTableId(key));
        ASSERT_EQ(123, RecordKVFormat::getHandle(key));
        ASSERT_EQ(992134, RecordKVFormat::getTs(key));

        auto bare_key = RecordKVFormat::truncateTs(key);
        ASSERT_EQ(key, RecordKVFormat::appendTs(bare_key, 992134));
    }

    {
        std::string shor_value = "value";
        auto lock_for_update_ts = 7777, txn_size = 1;
        const std::vector<std::string> & async_commit = {"s1", "s2"};
        const std::vector<uint64_t> & rollback = {3, 4};
        auto lock_value = DB::RegionBench::encodeFullLockCfValue(
            Region::DelFlag,
            "primary key",
            421321,
            std::numeric_limits<UInt64>::max(),
            &shor_value,
            66666,
            lock_for_update_ts,
            txn_size,
            async_commit,
            rollback);
        auto ori_key = std::make_shared<const TiKVKey>(RecordKVFormat::genKey(1, 88888));
        auto lock = RecordKVFormat::DecodedLockCFValue(ori_key, std::make_shared<TiKVValue>(std::move(lock_value)));
        {
            auto & lock_info = lock;
            ASSERT_TRUE(ori_key == lock_info.key);
            lock_info.withInner([&](const auto & in) {
                ASSERT_TRUE(kvrpcpb::Op::Del == in.lock_type);
                ASSERT_TRUE("primary key" == in.primary_lock);
                ASSERT_TRUE(421321 == in.lock_version);
                ASSERT_TRUE(std::numeric_limits<UInt64>::max() == in.lock_ttl);
                ASSERT_TRUE(66666 == in.min_commit_ts);
                ASSERT_EQ(lock_for_update_ts, in.lock_for_update_ts);
                ASSERT_EQ(txn_size, in.txn_size);
                ASSERT_EQ(true, in.use_async_commit);
            });
        }
        {
            auto lock_info = lock.intoLockInfo();
            ASSERT_TRUE(kvrpcpb::Op::Del == lock_info->lock_type());
            ASSERT_TRUE("primary key" == lock_info->primary_lock());
            ASSERT_TRUE(421321 == lock_info->lock_version());
            ASSERT_TRUE(std::numeric_limits<UInt64>::max() == lock_info->lock_ttl());
            ASSERT_TRUE(66666 == lock_info->min_commit_ts());
            ASSERT_TRUE(RecordKVFormat::decodeTiKVKey(*ori_key) == lock_info->key());
            ASSERT_EQ(true, lock_info->use_async_commit());
            ASSERT_EQ(lock_for_update_ts, lock_info->lock_for_update_ts());
            ASSERT_EQ(txn_size, lock_info->txn_size());
            {
                auto secondaries = lock_info->secondaries();
                ASSERT_EQ(2, secondaries.size());
                ASSERT_EQ(secondaries[0], async_commit[0]);
                ASSERT_EQ(secondaries[1], async_commit[1]);
            }
        }

        {
            RegionLockCFData d;
            auto k1 = RecordKVFormat::genKey(1, 123);
            auto k2 = RecordKVFormat::genKey(1, 124);
            d.insert(
                TiKVKey::copyFrom(k1),
                RecordKVFormat::encodeLockCfValue(
                    Region::PutFlag,
                    "primary key",
                    8765,
                    std::numeric_limits<UInt64>::max(),
                    nullptr,
                    66666));
            d.insert(
                TiKVKey::copyFrom(k2),
                RecordKVFormat::encodeLockCfValue(RecordKVFormat::LockType::Lock, "", 8, 20));
            d.insert(
                TiKVKey::copyFrom(k2),
                RecordKVFormat::encodeLockCfValue(RecordKVFormat::LockType::Pessimistic, "", 8, 20));
            d.insert(
                TiKVKey::copyFrom(k2),
                RecordKVFormat::encodeLockCfValue(
                    Region::DelFlag,
                    "primary key",
                    5678,
                    std::numeric_limits<UInt64>::max(),
                    nullptr,
                    66666));
            ASSERT_TRUE(d.getSize() == 2);

            std::get<2>(d.getData()
                            .find(RegionLockCFDataTrait::Key{nullptr, std::string_view(k2.data(), k2.dataSize())})
                            ->second)
                ->withInner([&](const auto & in) { ASSERT_EQ(in.lock_version, 5678); });

            d.remove(RegionLockCFDataTrait::Key{nullptr, std::string_view(k1.data(), k1.dataSize())}, true);
            ASSERT_TRUE(d.getSize() == 1);
            d.remove(RegionLockCFDataTrait::Key{nullptr, std::string_view(k2.data(), k2.dataSize())}, true);
            ASSERT_TRUE(d.getSize() == 0);
        }
    }

    {
        auto write_value
            = RecordKVFormat::encodeWriteCfValue(Region::DelFlag, std::numeric_limits<UInt64>::max(), "value");
        auto write_record = RecordKVFormat::decodeWriteCfValue(write_value);
        ASSERT_TRUE(write_record);
        ASSERT_TRUE(Region::DelFlag == write_record->write_type);
        ASSERT_TRUE(std::numeric_limits<UInt64>::max() == write_record->prewrite_ts);
        ASSERT_TRUE("value" == *write_record->short_value);
        RegionWriteCFData d;
        d.insert(RecordKVFormat::genKey(1, 2, 3), RecordKVFormat::encodeWriteCfValue(Region::PutFlag, 4, "value"));
        ASSERT_TRUE(d.getSize() == 1);

        ASSERT_TRUE(
            d.insert(
                 RecordKVFormat::genKey(1, 2, 3),
                 RecordKVFormat::encodeWriteCfValue(Region::PutFlag, 4, "value", true))
                .payload
            == 0);
        ASSERT_TRUE(d.getSize() == 1);

        ASSERT_TRUE(
            d.insert(
                 RecordKVFormat::genKey(1, 2, 3),
                 RecordKVFormat::encodeWriteCfValue(RecordKVFormat::UselessCFModifyFlag::LockFlag, 4, "value"))
                .payload
            == 0);
        ASSERT_TRUE(d.getSize() == 1);

        auto pk = RecordKVFormat::getRawTiDBPK(RecordKVFormat::genRawKey(1, 2));
        d.remove(RegionWriteCFData::Key{pk, 3});
        ASSERT_TRUE(d.getSize() == 0);
    }

    {
        auto write_value = RecordKVFormat::encodeWriteCfValue(Region::DelFlag, std::numeric_limits<UInt64>::max());
        auto write_record = RecordKVFormat::decodeWriteCfValue(write_value);
        ASSERT_TRUE(write_record);
        ASSERT_TRUE(Region::DelFlag == write_record->write_type);
        ASSERT_TRUE(std::numeric_limits<UInt64>::max() == write_record->prewrite_ts);
        ASSERT_TRUE(nullptr == write_record->short_value);
    }

    {
        auto write_value
            = RecordKVFormat::encodeWriteCfValue(RecordKVFormat::UselessCFModifyFlag::RollbackFlag, 8888, "test");
        auto write_record = RecordKVFormat::decodeWriteCfValue(write_value);
        ASSERT_TRUE(!write_record);
    }

    {
        auto write_value = RecordKVFormat::encodeWriteCfValue(Region::PutFlag, 8888, "qwer", true);
        auto write_record = RecordKVFormat::decodeWriteCfValue(write_value);
        ASSERT_TRUE(!write_record);
    }

    {
        UInt64 a = 13241432453554;
        Crc32 crc32;
        crc32.put(&a, sizeof(a));
        ASSERT_TRUE(crc32.checkSum() == 3312221216);
    }

    {
        TiKVKey start_key = RecordKVFormat::genKey(200, 123);
        TiKVKey end_key = RecordKVFormat::genKey(300, 124);

        ASSERT_TRUE(checkTableInvolveRange(200, RangeRef{start_key, end_key}));
        ASSERT_TRUE(checkTableInvolveRange(250, RangeRef{start_key, end_key}));
        ASSERT_TRUE(checkTableInvolveRange(300, RangeRef{start_key, end_key}));
        ASSERT_TRUE(!checkTableInvolveRange(400, RangeRef{start_key, end_key}));
    }
    {
        TiKVKey start_key = RecordKVFormat::genKey(200, std::numeric_limits<HandleID>::min());
        TiKVKey end_key = RecordKVFormat::genKey(200, 100);

        ASSERT_TRUE(checkTableInvolveRange(200, RangeRef{start_key, end_key}));
        ASSERT_TRUE(!checkTableInvolveRange(100, RangeRef{start_key, end_key}));
    }
    {
        TiKVKey start_key;
        TiKVKey end_key;

        ASSERT_TRUE(checkTableInvolveRange(200, RangeRef{start_key, end_key}));
        ASSERT_TRUE(checkTableInvolveRange(250, RangeRef{start_key, end_key}));
        ASSERT_TRUE(checkTableInvolveRange(300, RangeRef{start_key, end_key}));
        ASSERT_TRUE(checkTableInvolveRange(400, RangeRef{start_key, end_key}));
    }

    {
        TiKVKey start_key = genIndex(233, 111);
        TiKVKey end_key = RecordKVFormat::genKey(300, 124);
        auto begin = TiKVRange::getRangeHandle<true>(start_key, 233);
        auto end = TiKVRange::getRangeHandle<false>(end_key, 233);
        ASSERT_TRUE(begin == begin.normal_min);
        ASSERT_TRUE(end == end.max);
    }

    {
        TiKVKey start_key = genIndex(233, 111);
        TiKVKey end_key = RecordKVFormat::genKey(300, 124);
        auto begin = TiKVRange::getRangeHandle<true>(start_key, 300);
        auto end = TiKVRange::getRangeHandle<false>(end_key, 300);
        ASSERT_TRUE(begin == begin.normal_min);
        ASSERT_TRUE(Int64{124} == end);
    }

    {
        using HandleInt64 = TiKVHandle::Handle<Int64>;
        Int64 int64_min = std::numeric_limits<Int64>::min();
        Int64 int64_max = std::numeric_limits<Int64>::max();
        ASSERT_TRUE(HandleInt64(int64_min) < HandleInt64(int64_max));
        ASSERT_TRUE(HandleInt64(int64_min) <= HandleInt64(int64_max));
        ASSERT_TRUE(HandleInt64(int64_max) > HandleInt64(int64_min));
        ASSERT_TRUE(HandleInt64(int64_max) >= HandleInt64(int64_min));
        ASSERT_TRUE(HandleInt64(int64_min) == HandleInt64(int64_min));
        ASSERT_TRUE(HandleInt64(int64_max) == HandleInt64(int64_max));

        ASSERT_TRUE(int64_min < HandleInt64(int64_max));
        ASSERT_TRUE(int64_min <= HandleInt64(int64_max));
        ASSERT_TRUE(int64_max > HandleInt64(int64_min));
        ASSERT_TRUE(int64_max >= HandleInt64(int64_min));
        ASSERT_TRUE(int64_min == HandleInt64(int64_min));
        ASSERT_TRUE(int64_max == HandleInt64(int64_max));

        ASSERT_TRUE(int64_max < HandleInt64::max);
        ASSERT_TRUE(int64_max <= HandleInt64::max);

        ASSERT_TRUE(HandleInt64::max > int64_max);
        ASSERT_TRUE(HandleInt64::max >= int64_max);

        ASSERT_TRUE(HandleInt64::max == HandleInt64::max);
    }

    {
        ASSERT_TRUE(TiKVRange::getRangeHandle<true>(TiKVKey(""), 1000) == TiKVRange::Handle::normal_min);
        ASSERT_TRUE(TiKVRange::getRangeHandle<false>(TiKVKey(""), 1000) == TiKVRange::Handle::max);
    }

    {
        TiKVKey start_key = RecordKVFormat::genKey(123, std::numeric_limits<Int64>::min());
        TiKVKey end_key = RecordKVFormat::genKey(123, std::numeric_limits<Int64>::max());
        ASSERT_TRUE(
            TiKVRange::getRangeHandle<true>(start_key, 123) == TiKVRange::Handle(std::numeric_limits<Int64>::min()));
        ASSERT_TRUE(
            TiKVRange::getRangeHandle<false>(end_key, 123) == TiKVRange::Handle(std::numeric_limits<Int64>::max()));

        ASSERT_TRUE(TiKVRange::getRangeHandle<true>(start_key, 123) >= TiKVRange::Handle::normal_min);
        ASSERT_TRUE(TiKVRange::getRangeHandle<false>(end_key, 123) < TiKVRange::Handle::max);

        start_key = RecordKVFormat::encodeAsTiKVKey(RecordKVFormat::decodeTiKVKey(start_key) + "123");
        ASSERT_TRUE(
            TiKVRange::getRangeHandle<true>(start_key, 123)
            == TiKVRange::Handle(std::numeric_limits<Int64>::min() + 1));
        ASSERT_TRUE(RecordKVFormat::genKey(123, std::numeric_limits<Int64>::min() + 2) >= start_key);
        ASSERT_TRUE(RecordKVFormat::genKey(123, std::numeric_limits<Int64>::min()) < start_key);

        end_key = RecordKVFormat::encodeAsTiKVKey(RecordKVFormat::decodeTiKVKey(end_key) + "123");
        ASSERT_TRUE(TiKVRange::getRangeHandle<false>(end_key, 123) == TiKVRange::Handle::max);

        auto s = RecordKVFormat::genRawKey(123, -1);
        s.resize(17);
        ASSERT_TRUE(s.size() == 17);
        start_key = RecordKVFormat::encodeAsTiKVKey(s);
        auto o1 = TiKVRange::getRangeHandle<true>(start_key, 123);

        s = RecordKVFormat::genRawKey(123, -1);
        s[17] = s[18] = 0;
        ASSERT_TRUE(s.size() == 19);
        auto o2 = RecordKVFormat::getHandle(s);
        ASSERT_TRUE(o2 == o1);
    }

    {
        std::string s = "1234";
        s[0] = static_cast<char>(1);
        s[3] = static_cast<char>(111);
        const auto & key = TiKVKey(s.data(), s.size());
        ASSERT_EQ(key.toDebugString(), "0132336F");
    }

    {
        std::string s(12, 1);
        s[8] = s[9] = s[10] = 0;
        ASSERT_TRUE(RecordKVFormat::checkKeyPaddingValid(s.data() + 1, 1));
        ASSERT_TRUE(RecordKVFormat::checkKeyPaddingValid(s.data() + 2, 2));
        ASSERT_TRUE(RecordKVFormat::checkKeyPaddingValid(s.data() + 3, 3));
        for (auto i = 1; i <= 8; ++i)
            ASSERT_TRUE(!RecordKVFormat::checkKeyPaddingValid(s.data() + 4, i));
    }

    {
        RegionRangeKeys range(RecordKVFormat::genKey(1, 2, 3), RecordKVFormat::genKey(2, 4, 100));
        ASSERT_TRUE(RecordKVFormat::getTs(range.comparableKeys().first.key) == 3);
        ASSERT_TRUE(RecordKVFormat::getTs(range.comparableKeys().second.key) == 100);
        ASSERT_TRUE(RecordKVFormat::getTableId(*range.rawKeys().first) == 1);
        ASSERT_TRUE(RecordKVFormat::getTableId(*range.rawKeys().second) == 2);
        ASSERT_TRUE(RecordKVFormat::getHandle(*range.rawKeys().first) == 2);
        ASSERT_TRUE(RecordKVFormat::getHandle(*range.rawKeys().second) == 4);

        ASSERT_TRUE(range.comparableKeys().first.state == TiKVRangeKey::NORMAL);
        ASSERT_TRUE(range.comparableKeys().second.state == TiKVRangeKey::NORMAL);

        auto range2 = RegionRangeKeys::makeComparableKeys(TiKVKey{}, TiKVKey{});
        ASSERT_TRUE(range2.first.state == TiKVRangeKey::MIN);
        ASSERT_TRUE(range2.second.state == TiKVRangeKey::MAX);

        ASSERT_TRUE(range2.first.compare(range2.second) < 0);
        ASSERT_TRUE(range2.first.compare(range.comparableKeys().second) < 0);
        ASSERT_TRUE(range.comparableKeys().first.compare(range.comparableKeys().second) < 0);
        ASSERT_TRUE(range.comparableKeys().second.compare(range2.second) < 0);

        ASSERT_TRUE(range.comparableKeys().first.compare(RecordKVFormat::genKey(1, 2, 3)) == 0);
    }

    {
        const Int64 table_id = 2333;
        const Timestamp ts = 66666;
        std::string key(RecordKVFormat::RAW_KEY_NO_HANDLE_SIZE, 0);
        memcpy(key.data(), &RecordKVFormat::TABLE_PREFIX, 1);
        auto big_endian_table_id = RecordKVFormat::encodeInt64(table_id);
        memcpy(key.data() + 1, reinterpret_cast<const char *>(&big_endian_table_id), 8);
        memcpy(key.data() + 1 + 8, RecordKVFormat::RECORD_PREFIX_SEP, 2);
        std::string pk = "12345678...";
        key += pk;
        auto tikv_key = RecordKVFormat::encodeAsTiKVKey(key);
        RecordKVFormat::appendTs(tikv_key, ts);
        {
            auto decoded_key = RecordKVFormat::decodeTiKVKey(tikv_key);
            ASSERT_EQ(RecordKVFormat::getTableId(decoded_key), table_id);
            auto tidb_pk = RecordKVFormat::getRawTiDBPK(decoded_key);
            ASSERT_EQ(*tidb_pk, pk);
        }
    }
}

TEST(TiKVKeyValueTest, ParseLockValue)
try
{
    // prepare
    std::string shor_value = "value";
    auto lock_for_update_ts = 7777, txn_size = 1;
    const std::vector<std::string> & async_commit = {"s1", "s2"};
    const std::vector<uint64_t> & rollback = {3, 4};
    auto lock_value = DB::RegionBench::encodeFullLockCfValue(
        Region::DelFlag,
        "primary key",
        421321,
        std::numeric_limits<UInt64>::max(),
        &shor_value,
        66666,
        lock_for_update_ts,
        txn_size,
        async_commit,
        rollback);

    // parse
    auto ori_key = std::make_shared<const TiKVKey>(RecordKVFormat::genKey(1, 88888));
    auto lock = RecordKVFormat::DecodedLockCFValue(ori_key, std::make_shared<TiKVValue>(std::move(lock_value)));

    // check the parsed result
    {
        auto & lock_info = lock;
        ASSERT_TRUE(ori_key == lock_info.key);
        ASSERT_FALSE(lock_info.isLargeTxn());

        lock_info.withInner([&](const auto & in) {
            ASSERT_TRUE(kvrpcpb::Op::Del == in.lock_type);
            ASSERT_TRUE("primary key" == in.primary_lock);
            ASSERT_TRUE(421321 == in.lock_version);
            ASSERT_TRUE(std::numeric_limits<UInt64>::max() == in.lock_ttl);
            ASSERT_TRUE(66666 == in.min_commit_ts);
            ASSERT_EQ(lock_for_update_ts, in.lock_for_update_ts);
            ASSERT_EQ(txn_size, in.txn_size);
            ASSERT_EQ(true, in.use_async_commit);
        });
    }

    auto lock_value2 = DB::RegionBench::encodeFullLockCfValue(
        Region::DelFlag,
        "primary key",
        421321,
        std::numeric_limits<UInt64>::max(),
        &shor_value,
        66666,
        lock_for_update_ts,
        txn_size,
        async_commit,
        rollback,
        1111);

    auto lock2 = RecordKVFormat::DecodedLockCFValue(ori_key, std::make_shared<TiKVValue>(std::move(lock_value2)));
    ASSERT_TRUE(lock2.isLargeTxn());
}
CATCH


TEST(TiKVKeyValueTest, Redact)
try
{
    String table_info_json
        = R"json({"cols":[{"comment":"","default":null,"default_bit":null,"id":1,"name":{"L":"a","O":"a"},"offset":0,"origin_default":null,"state":5,"type":{"Charset":"utf8mb4","Collate":"utf8mb4_bin","Decimal":0,"Elems":null,"Flag":3,"Flen":10,"Tp":15}},{"comment":"","default":null,"default_bit":null,"id":2,"name":{"L":"b","O":"b"},"offset":1,"origin_default":null,"state":5,"type":{"Charset":"utf8mb4","Collate":"utf8mb4_bin","Decimal":0,"Elems":null,"Flag":3,"Flen":20,"Tp":15}},{"comment":"","default":null,"default_bit":null,"id":3,"name":{"L":"c","O":"c"},"offset":2,"origin_default":null,"state":5,"type":{"Charset":"binary","Collate":"binary","Decimal":0,"Elems":null,"Flag":0,"Flen":11,"Tp":3}}],"comment":"","id":49,"index_info":[{"id":1,"idx_cols":[{"length":-1,"name":{"L":"a","O":"a"},"offset":0},{"length":-1,"name":{"L":"b","O":"b"},"offset":1}],"idx_name":{"L":"primary","O":"primary"},"index_type":1,"is_global":false,"is_invisible":false,"is_primary":true,"is_unique":true,"state":5,"tbl_name":{"L":"","O":""}}],"is_common_handle":true,"name":{"L":"pt","O":"pt"},"partition":null,"pk_is_handle":false,"schema_version":25,"state":5,"update_timestamp":421444995366518789})json";
    TiDB::TableInfo table_info(table_info_json, NullspaceID);
    ASSERT_TRUE(table_info.is_common_handle);

    TiKVKey start, end;
    {
        start
            = RecordKVFormat::genKey(table_info, std::vector{Field{"aaa", strlen("aaa")}, Field{"abc", strlen("abc")}});
        end = RecordKVFormat::genKey(table_info, std::vector{Field{"bbb", strlen("bbb")}, Field{"abc", strlen("abc")}});
    }
    RegionRangeKeys range(std::move(start), std::move(end));
    const auto & raw_keys = range.rawKeys();
    ASSERT_EQ(RecordKVFormat::getTableId(*raw_keys.first), 49);
    ASSERT_EQ(RecordKVFormat::getTableId(*raw_keys.second), 49);

    auto raw_pk1 = RecordKVFormat::getRawTiDBPK(*raw_keys.first);
    auto raw_pk2 = RecordKVFormat::getRawTiDBPK(*raw_keys.second);

    Redact::setRedactLog(RedactMode::Disable);
    // These will print the value
    EXPECT_EQ(raw_pk1.toDebugString(), "02066161610206616263");
    EXPECT_EQ(raw_pk2.toDebugString(), "02066262620206616263");
    EXPECT_EQ(
        RecordKVFormat::DecodedTiKVKeyRangeToDebugString(raw_keys),
        "[02066161610206616263, 02066262620206616263)");

    Redact::setRedactLog(RedactMode::Enable);
    // These will print '?' instead of value
    EXPECT_EQ(raw_pk1.toDebugString(), "?");
    EXPECT_EQ(raw_pk2.toDebugString(), "?");
    EXPECT_EQ(RecordKVFormat::DecodedTiKVKeyRangeToDebugString(raw_keys), "[?, ?)");

    // print values with marker
    Redact::setRedactLog(RedactMode::Marker);
    EXPECT_EQ(raw_pk1.toDebugString(), "‹02066161610206616263›");
    EXPECT_EQ(raw_pk2.toDebugString(), "‹02066262620206616263›");
    EXPECT_EQ(
        RecordKVFormat::DecodedTiKVKeyRangeToDebugString(raw_keys),
        "[‹02066161610206616263›, ‹02066262620206616263›)");

    Redact::setRedactLog(RedactMode::Disable); // restore flags
}
CATCH

namespace
{
// In python, we can convert a test case from `s`
// 'range = parseTestCase({{{}}});\nASSERT_EQ(range, expected_range);'.format(','.join(map(lambda x: '{{{}}}'.format(','.join(map(lambda y: '0x{:02x}'.format(int(y, 16)), x.strip('[').strip(']').split()))), s.split(','))))

HandleRange<HandleID> parseTestCase(std::vector<std::vector<u_char>> && seq)
{
    std::string start_key_s, end_key_s;
    for (const auto ch : seq[0])
        start_key_s += ch;
    for (const auto ch : seq[1])
        end_key_s += ch;
    RegionRangeKeys range{RecordKVFormat::encodeAsTiKVKey(start_key_s), RecordKVFormat::encodeAsTiKVKey(end_key_s)};
    return getHandleRangeByTable(range.rawKeys(), 45);
}

HandleRange<HandleID> parseTestCase2(std::vector<std::vector<u_char>> && seq)
{
    std::string start_key_s, end_key_s;
    for (const auto ch : seq[0])
        start_key_s += ch;
    for (const auto ch : seq[1])
        end_key_s += ch;
    RegionRangeKeys range{TiKVKey::copyFrom(start_key_s), TiKVKey::copyFrom(end_key_s)};
    return getHandleRangeByTable(range.rawKeys(), 45);
}

std::string rangeToString(const HandleRange<HandleID> & r)
{
    std::stringstream ss;
    ss << "[" << r.first.toString() << "," << r.second.toString() << ")";
    return ss.str();
}

} // namespace

TEST(RegionRangeTest, DISABLED_GetHandleRangeByTableID)
try
{
    HandleRange<HandleID> range;
    HandleRange<HandleID> expected_range;

    // clang-format off
    range = parseTestCase({{0x74,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x2D,},{}});
    expected_range = {TiKVHandle::Handle<HandleID>::normal_min, TiKVHandle::Handle<HandleID>::max};
    EXPECT_EQ(range, expected_range) << rangeToString(range) << " <-> " << rangeToString(expected_range);

    range = parseTestCase({{0x74,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x2d,0x5f,0x69,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x03,0x80,0x00,0x00,0x00,0x00,0x5a,0x0f,0x00,0x03,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x02},{0x74,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x2d,0x5f,0x72,0x80,0x00,0x00,0x00,0x00,0x00,0xaa,0x40}});
    expected_range = {TiKVHandle::Handle<HandleID>::normal_min, 43584};
    EXPECT_EQ(range, expected_range) << rangeToString(range) << " <-> " << rangeToString(expected_range);

    range = parseTestCase({{0x74,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x2d,0x5f,0x72,0x80,0x00,0x00,0x00,0x00,0x00,0xaa,0x40},{0x74,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x2d,0x5f,0x72,0x80,0x00,0x00,0x00,0x00,0x02,0x21,0x40}});
    expected_range = {43584, 139584};
    EXPECT_EQ(range, expected_range) << rangeToString(range) << " <-> " << rangeToString(expected_range);

    range = parseTestCase({{0x74,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x2d,0x5f,0x72,0x80,0x00,0x00,0x00,0x00,0x10,0xc7,0x40},{0x74,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x2d,0x5f,0x72,0x80,0x00,0x00,0x00,0x00,0x12,0x3e,0x40}});
    expected_range = {1099584, 1195584};
    EXPECT_EQ(range, expected_range) << rangeToString(range) << " <-> " << rangeToString(expected_range);


    // [74 80 0 0 0 0 0 0 ff 2d 5f 69 80 0 0 0 0 ff 0 0 1 3 80 0 0 0 ff 0 5a cf 64 3 80 0 0 ff 0 0 0 0 2 0 0 0 fc],[74 80 0 0 0 0 0 0 ff 2d 5f 72 80 0 0 0 0 ff 0 b8 b 0 0 0 0 0 fa]
    range = parseTestCase2({{0x74,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0xff,0x2d,0x5f,0x69,0x80,0x00,0x00,0x00,0x00,0xff,0x00,0x00,0x01,0x03,0x80,0x00,0x00,0x00,0xff,0x00,0x5a,0xcf,0x64,0x03,0x80,0x00,0x00,0xff,0x00,0x00,0x00,0x00,0x02,0x00,0x00,0x00,0xfc},{0x74,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0xff,0x2d,0x5f,0x72,0x80,0x00,0x00,0x00,0x00,0xff,0x00,0xb8,0x0b,0x00,0x00,0x00,0x00,0x00,0xfa}});
    expected_range = {TiKVHandle::Handle<HandleID>::normal_min, 47115};
    EXPECT_EQ(range, expected_range) << rangeToString(range) << " <-> " << rangeToString(expected_range);

    // clang-format on
}
CATCH
