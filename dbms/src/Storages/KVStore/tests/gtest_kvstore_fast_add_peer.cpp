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
#include <Debug/MockKVStore/MockRaftStoreProxy.h>
#include <Debug/TiFlashTestEnv.h>
#include <Interpreters/SharedContexts/Disagg.h>
#include <Storages/DeltaMerge/Filter/PushDownExecutor.h>
#include <Storages/DeltaMerge/ReadThread/SegmentReadTaskScheduler.h>
#include <Storages/KVStore/FFI/ProxyFFI.h>
#include <Storages/KVStore/MultiRaft/Disagg/FastAddPeer.h>
#include <Storages/KVStore/MultiRaft/Disagg/FastAddPeerCache.h>
#include <Storages/KVStore/MultiRaft/Disagg/FastAddPeerContext.h>
#include <Storages/KVStore/Types.h>
#include <Storages/KVStore/Utils/AsyncTasks.h>
#include <Storages/KVStore/tests/kvstore_helper.h>
#include <Storages/Page/V3/Universal/UniversalPageStorage.h>
#include <Storages/Page/V3/Universal/UniversalPageStorageService.h>
#include <Storages/S3/CheckpointManifestS3Set.h>
#include <Storages/S3/S3Common.h>
#include <TestUtils/InputStreamTestUtils.h>
#include <aws/s3/model/CreateBucketRequest.h>
#include <common/logger_useful.h>

#include <optional>

using raft_serverpb::RaftApplyState;
using raft_serverpb::RegionLocalState;

namespace DB
{
namespace FailPoints
{
extern const char force_fap_worker_throw[];
extern const char force_set_fap_candidate_store_id[];
extern const char force_not_clean_fap_on_destroy[];
extern const char force_checkpoint_dump_throw_datafile[];
extern const char pause_when_building_fap_segments[];
} // namespace FailPoints

namespace tests
{

struct FAPTestOpt
{
    bool mock_add_new_peer = false;
    bool persist_empty_segment = false;
    bool second_region = false;
};

class RegionKVStoreTestFAP : public KVStoreTestBase
{
public:
    void SetUp() override
    {
        // Need S3 for S3 lock client, otherwise UniversalPageStorage::write would block waiting.
        DB::tests::TiFlashTestEnv::enableS3Config();
        test_path = TiFlashTestEnv::getTemporaryPath("/region_kvs_fap_test");
        auto & global_context = TiFlashTestEnv::getGlobalContext();
        global_context.getTMTContext().initS3GCManager(nullptr);
        // clean data and create path pool instance
        path_pool = TiFlashTestEnv::createCleanPathPool(test_path);

        initStorages();

        // Must be called before `initializeWriteNodePageStorageIfNeed` to have S3 lock services registered.
        DB::tests::TiFlashTestEnv::enableS3Config();
        auto s3_client = S3::ClientFactory::instance().sharedTiFlashClient();
        ASSERT_TRUE(::DB::tests::TiFlashTestEnv::createBucketIfNotExist(*s3_client));

        orig_disagg_mode = global_context.getSharedContextDisagg()->disaggregated_mode;
        global_context.getSharedContextDisagg()->disaggregated_mode = DisaggregatedMode::Storage;
        if (global_context.getWriteNodePageStorage() == nullptr)
        {
            already_initialize_write_ps = false;
            orig_mode = global_context.getPageStorageRunMode();
            global_context.setPageStorageRunMode(PageStorageRunMode::UNI_PS);
            global_context.tryReleaseWriteNodePageStorageForTest();
            global_context.initializeWriteNodePageStorageIfNeed(*path_pool);
        }
        else
        {
            // It will currently happen in `initStorages` when we call `getContext`.
            already_initialize_write_ps = true;
        }

        if (global_context.getSharedContextDisagg()->remote_data_store == nullptr)
        {
            already_initialize_data_store = false;
            global_context.getSharedContextDisagg()->initRemoteDataStore(
                global_context.getFileProvider(),
                /*s3_enabled*/ true);
            ASSERT_TRUE(global_context.getSharedContextDisagg()->remote_data_store != nullptr);
        }
        else
        {
            already_initialize_data_store = true;
        }

        global_context.getSharedContextDisagg()->initFastAddPeerContext(25);
        proxy_instance = std::make_unique<MockRaftStoreProxy>();
        proxy_instance->proxy_config_string = R"({"raftstore":{"snap-handle-pool-size":3}})";
        proxy_helper = proxy_instance->generateProxyHelper();
        KVStoreTestBase::reloadKVSFromDisk(false);
        {
            auto store = metapb::Store{};
            store.set_id(1234);
            kvstore->setStore(store);
            ASSERT_EQ(kvstore->getStoreID(), store.id());
        }
        LOG_INFO(log, "Finished setup");
    }

    void TearDown() override
    {
        auto & global_context = TiFlashTestEnv::getGlobalContext();
        KVStoreTestBase::TearDown();
        global_context.getSharedContextDisagg()->fap_context->shutdown();
        if (!already_initialize_data_store)
        {
            global_context.getSharedContextDisagg()->remote_data_store = nullptr;
        }
        global_context.getSharedContextDisagg()->disaggregated_mode = orig_disagg_mode;
        if (!already_initialize_write_ps)
        {
            global_context.tryReleaseWriteNodePageStorageForTest();
            global_context.setPageStorageRunMode(orig_mode);
        }
        auto s3_client = S3::ClientFactory::instance().sharedTiFlashClient();
        ::DB::tests::TiFlashTestEnv::deleteBucket(*s3_client);
        DB::tests::TiFlashTestEnv::disableS3Config();
    }

    std::vector<CheckpointRegionInfoAndData> prepareForRestart(FAPTestOpt);

protected:
    void dumpCheckpoint()
    {
        auto & global_context = TiFlashTestEnv::getGlobalContext();
        auto temp_dir = TiFlashTestEnv::getTemporaryPath() + "/";
        auto page_storage = global_context.getWriteNodePageStorage();
        KVStore & kvs = getKVS();
        auto store_id = kvs.getStore().store_id.load();
        LOG_DEBUG(log, "dumpCheckpoint for checkpoint {}", store_id);
        auto wi = PS::V3::CheckpointProto::WriterInfo();
        {
            wi.set_store_id(store_id);
        }

        auto remote_store = global_context.getSharedContextDisagg()->remote_data_store;
        assert(remote_store != nullptr);
        UniversalPageStorage::DumpCheckpointOptions opts{
            .data_file_id_pattern = S3::S3Filename::newCheckpointDataNameTemplate(store_id, upload_sequence),
            .data_file_path_pattern = temp_dir + "dat_{seq}_{index}",
            .manifest_file_id_pattern = S3::S3Filename::newCheckpointManifestNameTemplate(store_id),
            .manifest_file_path_pattern = temp_dir + "mf_{seq}",
            .writer_info = wi,
            .must_locked_files = {},
            .persist_checkpoint = CheckpointUploadFunctor{
                .store_id = store_id,
                // Note that we use `upload_sequence` but not `snapshot.sequence` for
                // the S3 key.
                .sequence = upload_sequence,
                .remote_store = remote_store,
            },
            .override_sequence = upload_sequence, // override by upload_sequence
        };
        page_storage->dumpIncrementalCheckpoint(opts);
    }

protected:
    UInt64 upload_sequence = 1000;
    UInt64 table_id{};

private:
    ContextPtr context;
    bool already_initialize_data_store = false;
    bool already_initialize_write_ps = false;
    DB::PageStorageRunMode orig_mode = PageStorageRunMode::UNI_PS;
    DisaggregatedMode orig_disagg_mode = DisaggregatedMode::None;
};

void persistAfterWrite(
    Context & ctx,
    KVStore & kvs,
    std::unique_ptr<MockRaftStoreProxy> & proxy_instance,
    UniversalPageStoragePtr page_storage,
    uint64_t region_id,
    uint64_t index)
{
    MockRaftStoreProxy::FailCond cond;
    proxy_instance->doApply(kvs, ctx.getTMTContext(), cond, region_id, index);
    auto region = proxy_instance->getRegion(region_id);
    auto wb = region->persistMeta();
    page_storage->write(std::move(wb));
    // There shall be data to flush.
    ASSERT_EQ(kvs.needFlushRegionData(region_id, ctx.getTMTContext()), true);
    ASSERT_EQ(kvs.tryFlushRegionData(region_id, false, false, ctx.getTMTContext(), 0, 0, 0, 0), true);
}

template <typename F>
void eventuallyThrow(F f)
{
    using namespace std::chrono_literals;
    bool thrown = false;
    for (int i = 0; i < 5; i++)
    {
        try
        {
            f();
        }
        catch (...)
        {
            thrown = true;
            break;
        }
        std::this_thread::sleep_for(500ms);
    }
    ASSERT_TRUE(thrown);
}

template <typename F, typename FP>
void eventuallyPredicateEx(F f, FP fp)
{
    using namespace std::chrono_literals;
    for (int i = 0; i < 10; i++)
    {
        if (f())
            return;
        std::this_thread::sleep_for(500ms);
    }
    fp();
}

template <typename F>
void eventuallyPredicate(F f)
{
    eventuallyPredicateEx(f, []() { throw Exception("not meet"); });
}

void assertNoSegment(
    TMTContext & tmt,
    const RegionPtr & region,
    const FastAddPeerProto::CheckpointIngestInfoPersisted & ingest_info_persisted)
{
    auto & storages = tmt.getStorages();
    auto keyspace_id = region->getKeyspaceID();
    auto table_id = region->getMappedTableID();
    auto storage = storages.get(keyspace_id, table_id);
    RUNTIME_CHECK(storage && storage->engineType() == TiDB::StorageEngine::DT);
    auto dm_storage = std::dynamic_pointer_cast<StorageDeltaMerge>(storage);
    auto dm_context = dm_storage->getStore()->newDMContext(tmt.getContext(), tmt.getContext().getSettingsRef());
    for (const auto & seg_persisted : ingest_info_persisted.segments())
    {
        ReadBufferFromString buf(seg_persisted.segment_meta());
        DM::Segment::SegmentMetaInfo segment_info;
        readSegmentMetaInfo(buf, segment_info);

        // Delta layer is persisted with `CheckpointIngestInfoPersisted`.
        ReadBufferFromString buf_stable(seg_persisted.stable_meta());
        EXPECT_THROW(DM::StableValueSpace::restore(*dm_context, buf_stable, segment_info.stable_id), Exception);
    }
}

TEST_F(RegionKVStoreTestFAP, RestoreRaftState)
try
{
    auto & global_context = TiFlashTestEnv::getGlobalContext();
    uint64_t region_id = 1;
    auto peer_id = 1;
    KVStore & kvs = getKVS();
    auto page_storage = global_context.getWriteNodePageStorage();

    proxy_instance->bootstrapWithRegion(kvs, global_context.getTMTContext(), region_id, std::nullopt);
    auto region = proxy_instance->getRegion(region_id);
    auto store_id = kvs.getStore().store_id.load();
    region->addPeer(store_id, peer_id, metapb::PeerRole::Learner);

    // Write some data, and persist meta.
    auto [index, term]
        = proxy_instance->normalWrite(region_id, {34}, {"v2"}, {WriteCmdType::Put}, {ColumnFamilyType::Default});
    kvs.debugGetConfigMut().debugSetCompactLogConfig(0, 0, 0, 0);
    persistAfterWrite(global_context, kvs, proxy_instance, page_storage, region_id, index);

    auto s3_client = S3::ClientFactory::instance().sharedTiFlashClient();
    ASSERT_TRUE(::DB::tests::TiFlashTestEnv::createBucketIfNotExist(*s3_client));
    dumpCheckpoint();

    auto fap_context = global_context.getSharedContextDisagg()->fap_context;
    {
        auto [data_seq, checkpoint_data_holder] = fap_context->getNewerCheckpointData(global_context, store_id, 0);
        ASSERT_GT(data_seq, 0);
        ASSERT_TRUE(checkpoint_data_holder != nullptr);

        RaftApplyState apply_state;
        {
            // TODO: use `RaftDataReader::readRegionApplyState`?
            auto apply_state_key = UniversalPageIdFormat::toRaftApplyStateKeyInKVEngine(region_id);
            auto page = checkpoint_data_holder->getUniversalPageStorage()->read(apply_state_key);
            apply_state.ParseFromArray(page.data.begin(), page.data.size());
        }

        RegionLocalState region_state;
        {
            auto local_state_key = UniversalPageIdFormat::toRegionLocalStateKeyInKVEngine(region_id);
            auto page = checkpoint_data_holder->getUniversalPageStorage()->read(local_state_key);
            region_state.ParseFromArray(page.data.begin(), page.data.size());
        }

        {
            auto region_key = UniversalPageIdFormat::toKVStoreKey(region_id);
            auto page = checkpoint_data_holder->getUniversalPageStorage()
                            ->read(region_key, /*read_limiter*/ nullptr, {}, /*throw_on_not_exist*/ false);
            RUNTIME_CHECK(page.isValid());
        }

        ASSERT_TRUE(apply_state == region->getApply());
        ASSERT_TRUE(region_state == region->getState());
    }
    {
        auto [data_seq, checkpoint_data_holder]
            = fap_context->getNewerCheckpointData(global_context, store_id, upload_sequence);
        ASSERT_EQ(data_seq, upload_sequence);
        ASSERT_TRUE(checkpoint_data_holder == nullptr);
    }
}
CATCH


void verifyRows(Context & ctx, DM::DeltaMergeStorePtr store, const DM::RowKeyRange & range, size_t rows)
{
    const auto & columns = store->getTableColumns();
    BlockInputStreamPtr in = store->read(
        ctx,
        ctx.getSettingsRef(),
        columns,
        {range},
        /* num_streams= */ 1,
        /* start_ts= */ std::numeric_limits<UInt64>::max(),
        DM::EMPTY_FILTER,
        std::vector<RuntimeFilterPtr>{},
        0,
        "KVStoreFastAddPeer",
        /* keep_order= */ false,
        /* is_fast_scan= */ false,
        /* expected_block_size= */ 1024)[0];
    ASSERT_INPUTSTREAM_NROWS(in, rows);
}

std::vector<CheckpointRegionInfoAndData> RegionKVStoreTestFAP::prepareForRestart(FAPTestOpt opt)
{
    auto & global_context = TiFlashTestEnv::getGlobalContext();
    KVStore & kvs = getKVS();
    global_context.getTMTContext().debugSetKVStore(kvstore);
    auto fap_context = global_context.getSharedContextDisagg()->fap_context;
    auto page_storage = global_context.getWriteNodePageStorage();

    table_id = proxy_instance->bootstrapTable(global_context, kvs, global_context.getTMTContext());

    auto store_id = kvs.getStore().store_id.load();

    {
        auto start = RecordKVFormat::genKey(table_id, 0);
        auto end = RecordKVFormat::genKey(table_id, 1000000);
        proxy_instance->bootstrapWithRegion(
            kvs,
            global_context.getTMTContext(),
            1,
            std::make_pair(start.toString(), end.toString()));
        auto proxy_helper = proxy_instance->generateProxyHelper();
        auto region = proxy_instance->getRegion(1);
        region->addPeer(store_id, 1, metapb::PeerRole::Learner);
    }

    if (opt.second_region)
    {
        auto start = RecordKVFormat::genKey(table_id, 2000000);
        auto end = RecordKVFormat::genKey(table_id, 3000000);
        proxy_instance->debugAddRegions(
            kvs,
            global_context.getTMTContext(),
            {2},
            {std::make_pair(start.toString(), end.toString())});
        auto proxy_helper = proxy_instance->generateProxyHelper();
        auto region = proxy_instance->getRegion(2);
        region->addPeer(store_id, 2, metapb::PeerRole::Learner);
    }
    // Write some data, and persist meta.

    auto prepare_region = [&](UInt64 id, UInt64 peer_id, UInt64 k) {
        UInt64 index = 0;
        if (!opt.persist_empty_segment)
        {
            LOG_DEBUG(log, "Do write to the region");
            auto k1 = RecordKVFormat::genKey(table_id, k, 111);
            auto && [value_write1, value_default1] = proxy_instance->generateTiKVKeyValue(111, 999);
            UInt64 term = 0;
            std::tie(index, term) = proxy_instance->rawWrite(
                id,
                {k1, k1},
                {value_default1, value_write1},
                {WriteCmdType::Put, WriteCmdType::Put},
                {ColumnFamilyType::Default, ColumnFamilyType::Write});
        }
        kvs.debugGetConfigMut().debugSetCompactLogConfig(0, 0, 0, 0);
        if (opt.mock_add_new_peer)
        {
            *kvs.getRegion(id)->mutMeta().debugMutRegionState().getMutRegion().add_peers()
                = RegionBench::createPeer(peer_id, true);
            proxy_instance->getRegion(id)->addPeer(store_id, peer_id, metapb::PeerRole::Learner);
        }
        persistAfterWrite(global_context, kvs, proxy_instance, page_storage, id, index);
    };

    prepare_region(1, 2333, 888);
    if (opt.second_region)
    {
        prepare_region(2, 2334, 888 + 2000000);
    }

    auto s3_client = S3::ClientFactory::instance().sharedTiFlashClient();
    RUNTIME_CHECK(::DB::tests::TiFlashTestEnv::createBucketIfNotExist(*s3_client));
    dumpCheckpoint();

    const auto manifest_key = S3::S3Filename::newCheckpointManifest(kvs.getStoreID(), upload_sequence).toFullKey();
    auto data_holder = buildParsedCheckpointData(global_context, manifest_key, /*dir_seq*/ 100);

    auto build_for_region = [&](auto id) {
        LOG_INFO(log, "build checkpoint manifest from {} for {}", upload_sequence, id);
        auto region_key = UniversalPageIdFormat::toKVStoreKey(id);
        auto checkpoint_info = std::make_shared<CheckpointInfo>();
        checkpoint_info->remote_store_id = kvs.getStoreID();
        // Fake a region id to mock add peer.
        checkpoint_info->region_id = 1000 + id;
        checkpoint_info->checkpoint_data_holder = data_holder;
        auto page = checkpoint_info->checkpoint_data_holder->getUniversalPageStorage()
                        ->read(region_key, /*read_limiter*/ nullptr, {}, /*throw_on_not_exist*/ false);
        RUNTIME_CHECK(page.isValid());
        checkpoint_info->temp_ps = checkpoint_info->checkpoint_data_holder->getUniversalPageStorage();
        RegionPtr kv_region = kvs.getRegion(id);
        {
            auto task_lock = kvs.genTaskLock();
            auto region_lock = kvs.region_manager.genRegionTaskLock(id);
            kvs.removeRegion(id, false, global_context.getTMTContext().getRegionTable(), task_lock, region_lock);
        }

        return std::make_tuple(
            checkpoint_info,
            kv_region,
            kv_region->getMeta().clonedApplyState(),
            kv_region->getMeta().clonedRegionState());
    };

    if (opt.second_region)
    {
        return {build_for_region(1), build_for_region(2)};
    }
    return {build_for_region(1)};
}

// This function get tiflash replica count from local schema.
void setTiFlashReplicaSyncInfo(StorageDeltaMergePtr & dm_storage)
{
    auto table_info = dm_storage->getTableInfo();
    table_info.replica_info.count = 1;
    table_info.replica_info.available = false;
    dm_storage->setTableInfo(table_info);
}

// Test load from restart.
TEST_F(RegionKVStoreTestFAP, RestoreFromRestart1)
try
{
    auto mock_data = prepareForRestart(FAPTestOpt{})[0];
    RegionPtr kv_region = std::get<1>(mock_data);

    auto & global_context = TiFlashTestEnv::getGlobalContext();
    auto fap_context = global_context.getSharedContextDisagg()->fap_context;
    uint64_t region_id = 1;

    {
        auto storage = global_context.getTMTContext().getStorages().get(NullspaceID, table_id);
        auto dm_storage = std::dynamic_pointer_cast<StorageDeltaMerge>(storage);
        ASSERT_TRUE(dm_storage != nullptr);
        setTiFlashReplicaSyncInfo(dm_storage);
    }

    std::mutex exe_mut;
    std::unique_lock exe_lock(exe_mut);
    fap_context->tasks_trace->addTask(region_id, [&]() {
        // Keep the task in `tasks_trace` to prevent from canceling.
        std::scoped_lock wait_exe_lock(exe_mut);
        return genFastAddPeerResFail(FastAddPeerStatus::NoSuitable);
    });
    FastAddPeerImplWrite(global_context.getTMTContext(), proxy_helper.get(), region_id, 2333, std::move(mock_data), 0);
    exe_lock.unlock();
    fap_context->tasks_trace->fetchResult(region_id);

    auto region_to_ingest
        = fap_context
              ->getOrRestoreCheckpointIngestInfo(global_context.getTMTContext(), proxy_helper.get(), region_id, 2333)
              ->getRegion();
    // Remove the checkpoint ingest info and region from memory.
    // Testing whether FAP can be handled properly after restart.
    fap_context->debugRemoveCheckpointIngestInfo(region_id);
    // Remove the region so that the snapshot will be accepted.
    FailPointHelper::enableFailPoint("force_not_clean_fap_on_destroy");
    SCOPE_EXIT({ FailPointHelper::disableFailPoint("force_not_clean_fap_on_destroy"); });
    kvstore->handleDestroy(region_id, global_context.getTMTContext());

    auto prev_ru = TiFlashMetrics::instance().debugQueryReplicaSyncRU(NullspaceID);
    // After restart, continue the FAP from persisted checkpoint ingest info.
    ApplyFapSnapshotImpl(
        global_context.getTMTContext(),
        proxy_helper.get(),
        region_id,
        2333,
        true,
        region_to_ingest->appliedIndex(),
        region_to_ingest->appliedIndexTerm());
    auto current_ru = TiFlashMetrics::instance().debugQueryReplicaSyncRU(NullspaceID);
    ASSERT_GT(current_ru, prev_ru);

    {
        auto keyspace_id = kv_region->getKeyspaceID();
        auto table_id = kv_region->getMappedTableID();
        auto storage = global_context.getTMTContext().getStorages().get(keyspace_id, table_id);
        ASSERT_TRUE(storage && storage->engineType() == TiDB::StorageEngine::DT);
        auto dm_storage = std::dynamic_pointer_cast<StorageDeltaMerge>(storage);
        auto store = dm_storage->getStore();
        ASSERT_EQ(store->getRowKeyColumnSize(), 1);
        verifyRows(
            global_context,
            store,
            DM::RowKeyRange::newAll(store->isCommonHandle(), store->getRowKeyColumnSize()),
            1);
    }
    // CheckpointIngestInfo is not removed.
    eventuallyPredicate([&]() {
        return CheckpointIngestInfo::restore(global_context.getTMTContext(), proxy_helper.get(), region_id, 2333);
    });
    ASSERT_TRUE(fap_context->tryGetCheckpointIngestInfo(region_id).has_value());

    const auto checkpoint_info = fap_context->tryGetCheckpointIngestInfo(region_id).value();
    const auto checkpoint_info_meta = checkpoint_info->serializeMeta();

    EngineStoreServerWrap w{
        .tmt = &global_context.getTMTContext(),
        .proxy_helper = proxy_helper.get(),
    };

    // Re-ingest, will success.
    ApplyFapSnapshotImpl(
        global_context.getTMTContext(),
        proxy_helper.get(),
        region_id,
        2333,
        true,
        region_to_ingest->appliedIndex(),
        region_to_ingest->appliedIndexTerm());
    {
        auto keyspace_id = kv_region->getKeyspaceID();
        auto table_id = kv_region->getMappedTableID();
        auto storage = global_context.getTMTContext().getStorages().get(keyspace_id, table_id);
        ASSERT_TRUE(storage && storage->engineType() == TiDB::StorageEngine::DT);
        auto dm_storage = std::dynamic_pointer_cast<StorageDeltaMerge>(storage);
        auto store = dm_storage->getStore();
        ASSERT_EQ(store->getRowKeyColumnSize(), 1);
        verifyRows(
            global_context,
            store,
            DM::RowKeyRange::newAll(store->isCommonHandle(), store->getRowKeyColumnSize()),
            1);
    }

    ClearFapSnapshot(&w, region_id, 1);

    // Do it again. no throw.
    ClearFapSnapshot(&w, region_id, 1);

    // Re-ingest, throw.
    EXPECT_THROW(
        ApplyFapSnapshotImpl(
            global_context.getTMTContext(),
            proxy_helper.get(),
            region_id,
            2333,
            true,
            region_to_ingest->appliedIndex(),
            region_to_ingest->appliedIndexTerm()),
        DB::Exception);

    // CheckpointIngestInfo is removed.
    eventuallyPredicate([&]() {
        return !CheckpointIngestInfo::restore(global_context.getTMTContext(), proxy_helper.get(), region_id, 2333);
    });
    ASSERT_FALSE(fap_context->tryGetCheckpointIngestInfo(region_id).has_value());

    assertNoSegment(global_context.getTMTContext(), kv_region, checkpoint_info_meta);
}
CATCH

// Test if region is destroyed before applied.
TEST_F(RegionKVStoreTestFAP, RestoreFromRestart2)
try
{
    auto mock_data = prepareForRestart(FAPTestOpt{})[0];
    RegionPtr kv_region = std::get<1>(mock_data);

    auto & global_context = TiFlashTestEnv::getGlobalContext();
    auto fap_context = global_context.getSharedContextDisagg()->fap_context;
    uint64_t region_id = 1;
    std::mutex exe_mut;
    std::unique_lock exe_lock(exe_mut);
    fap_context->tasks_trace->addTask(region_id, [&]() {
        // Keep the task in `tasks_trace` to prevent from canceling.
        std::scoped_lock wait_exe_lock(exe_mut);
        return genFastAddPeerResFail(FastAddPeerStatus::NoSuitable);
    });
    FastAddPeerImplWrite(global_context.getTMTContext(), proxy_helper.get(), region_id, 2333, std::move(mock_data), 0);
    exe_lock.unlock();
    fap_context->tasks_trace->fetchResult(region_id);

    fap_context->debugRemoveCheckpointIngestInfo(region_id);
    kvstore->handleDestroy(region_id, global_context.getTMTContext());
    // CheckpointIngestInfo is removed.
    eventuallyPredicate([&]() {
        return !CheckpointIngestInfo::restore(global_context.getTMTContext(), proxy_helper.get(), region_id, 2333);
    });
    ASSERT_FALSE(fap_context->tryGetCheckpointIngestInfo(region_id).has_value());
}
CATCH

// Test if we can parse from an uploaded manifest
TEST_F(RegionKVStoreTestFAP, RestoreFromRestart3)
try
{
    CheckpointRegionInfoAndData mock_data = prepareForRestart(FAPTestOpt{})[0];
    KVStore & kvs = getKVS();
    RegionPtr kv_region = std::get<1>(mock_data);

    auto & global_context = TiFlashTestEnv::getGlobalContext();
    auto fap_context = global_context.getSharedContextDisagg()->fap_context;
    uint64_t region_id = 1;

    std::mutex exe_mut;
    std::unique_lock exe_lock(exe_mut);
    fap_context->tasks_trace->addTask(region_id, [&]() {
        // Keep the task in `tasks_trace` to prevent from canceling.
        std::scoped_lock wait_exe_lock(exe_mut);
        return genFastAddPeerResFail(FastAddPeerStatus::NoSuitable);
    });
    // Will generate and persist some information in local ps, which will not be uploaded.
    auto mock_data_cp = mock_data;
    FastAddPeerImplWrite(
        global_context.getTMTContext(),
        proxy_helper.get(),
        region_id,
        2333,
        std::move(mock_data_cp),
        0);
    dumpCheckpoint();
    FastAddPeerImplWrite(global_context.getTMTContext(), proxy_helper.get(), region_id, 2333, std::move(mock_data), 0);
    exe_lock.unlock();
    auto in_mem_ingest_info = fap_context->getOrRestoreCheckpointIngestInfo(
        global_context.getTMTContext(),
        proxy_helper.get(),
        region_id,
        2333);
    auto in_disk_ingest_info
        = CheckpointIngestInfo::restore(global_context.getTMTContext(), proxy_helper.get(), region_id, 2333);
    ASSERT_EQ(in_mem_ingest_info->getRegion()->getDebugString(), in_disk_ingest_info->getRegion()->getDebugString());
    ASSERT_EQ(in_mem_ingest_info->getRestoredSegments().size(), in_disk_ingest_info->getRestoredSegments().size());
    ASSERT_EQ(in_mem_ingest_info->getRemoteStoreId(), in_disk_ingest_info->getRemoteStoreId());

    auto s3_client = S3::ClientFactory::instance().sharedTiFlashClient();
    const auto manifests = S3::CheckpointManifestS3Set::getFromS3(*s3_client, kvs.getStoreID());
    const auto & latest_manifest_key = manifests.latestManifestKey();
    auto latest_manifest_key_view = S3::S3FilenameView::fromKey(latest_manifest_key);
    auto latest_upload_seq = latest_manifest_key_view.getUploadSequence();

    buildParsedCheckpointData(global_context, latest_manifest_key, latest_upload_seq);
}
CATCH


TEST_F(RegionKVStoreTestFAP, DumpCheckpointError)
try
{
    auto & global_context = TiFlashTestEnv::getGlobalContext();
    uint64_t region_id = 1;
    auto peer_id = 1;
    KVStore & kvs = getKVS();
    auto page_storage = global_context.getWriteNodePageStorage();

    proxy_instance->bootstrapWithRegion(kvs, global_context.getTMTContext(), region_id, std::nullopt);
    auto region = proxy_instance->getRegion(region_id);
    auto store_id = kvs.getStore().store_id.load();
    region->addPeer(store_id, peer_id, metapb::PeerRole::Learner);

    // Write some data, and persist meta.
    auto [index, term]
        = proxy_instance->normalWrite(region_id, {34}, {"v2"}, {WriteCmdType::Put}, {ColumnFamilyType::Default});
    kvs.debugGetConfigMut().debugSetCompactLogConfig(0, 0, 0, 0);
    persistAfterWrite(global_context, kvs, proxy_instance, page_storage, region_id, index);

    auto s3_client = S3::ClientFactory::instance().sharedTiFlashClient();
    ASSERT_TRUE(::DB::tests::TiFlashTestEnv::createBucketIfNotExist(*s3_client));
    FailPointHelper::enableFailPoint(FailPoints::force_checkpoint_dump_throw_datafile);
    EXPECT_NO_THROW(dumpCheckpoint());
    FailPointHelper::disableFailPoint(FailPoints::force_checkpoint_dump_throw_datafile);
}
CATCH

// Test cancel from peer select
TEST_F(RegionKVStoreTestFAP, Cancel1)
try
{
    CheckpointRegionInfoAndData mock_data = prepareForRestart(FAPTestOpt{})[0];
    RegionPtr kv_region = std::get<1>(mock_data);

    auto & global_context = TiFlashTestEnv::getGlobalContext();
    auto fap_context = global_context.getSharedContextDisagg()->fap_context;
    uint64_t region_id = 1;

    EngineStoreServerWrap server = {
        .tmt = &global_context.getTMTContext(),
        .proxy_helper = proxy_helper.get(),
    };

    kvstore->getStore().store_id.store(1, std::memory_order_release);
    kvstore->debugMutStoreMeta().set_id(1);
    ASSERT_EQ(1, kvstore->getStoreID());
    ASSERT_EQ(1, kvstore->clonedStoreMeta().id());
    FailPointHelper::enableFailPoint(FailPoints::force_set_fap_candidate_store_id);
    auto sp = SyncPointCtl::enableInScope("in_FastAddPeerImplSelect::before_sleep");
    // The FAP will fail because it doesn't contain the new peer in region meta.
    auto t = std::thread([&]() { FastAddPeer(&server, region_id, 2333); });
    // Retry for some times, then cancel.
    sp.waitAndPause();
    sp.next();
    sp.waitAndPause();
    fap_context->tasks_trace->asyncCancelTask(region_id);
    sp.next();
    sp.disable();
    t.join();
    eventuallyPredicate([&]() {
        return !CheckpointIngestInfo::restore(global_context.getTMTContext(), proxy_helper.get(), region_id, 2333);
    });
    ASSERT_TRUE(!fap_context->tryGetCheckpointIngestInfo(region_id).has_value());
    FailPointHelper::disableFailPoint(FailPoints::force_set_fap_candidate_store_id);
}
CATCH

// Test cancel from write
TEST_F(RegionKVStoreTestFAP, Cancel2)
try
{
    using namespace std::chrono_literals;
    auto mock_data = prepareForRestart(FAPTestOpt{
        .mock_add_new_peer = true,
    })[0];
    RegionPtr kv_region = std::get<1>(mock_data);

    auto & global_context = TiFlashTestEnv::getGlobalContext();
    auto fap_context = global_context.getSharedContextDisagg()->fap_context;
    uint64_t region_id = 1;

    EngineStoreServerWrap server = {
        .tmt = &global_context.getTMTContext(),
        .proxy_helper = proxy_helper.get(),
    };

    kvstore->getStore().store_id.store(1, std::memory_order_release);
    kvstore->debugMutStoreMeta().set_id(1);
    ASSERT_EQ(1, kvstore->getStoreID());
    ASSERT_EQ(1, kvstore->clonedStoreMeta().id());
    FailPointHelper::enableFailPoint(FailPoints::force_set_fap_candidate_store_id);
    auto sp = SyncPointCtl::enableInScope("in_FastAddPeerImplWrite::after_write_segments");
    // The FAP will fail because it doesn't contain the new peer in region meta.
    auto t = std::thread([&]() { FastAddPeer(&server, region_id, 2333); });
    sp.waitAndPause();
    // Make sure the data is written.
    auto maybe_info = fap_context->getOrRestoreCheckpointIngestInfo(
        global_context.getTMTContext(),
        proxy_helper.get(),
        region_id,
        2333);
    ASSERT_NE(maybe_info, nullptr);
    auto ingest_info_persisted = maybe_info->serializeMeta();
    auto region = maybe_info->getRegion();
    fap_context->tasks_trace->asyncCancelTask(region_id);
    sp.next();
    sp.disable();
    t.join();
    // Cancel async tasks, and make sure the data is cleaned after limited time.
    eventuallyPredicate([&]() {
        return !CheckpointIngestInfo::restore(global_context.getTMTContext(), proxy_helper.get(), region_id, 2333);
    });
    ASSERT_TRUE(!fap_context->tryGetCheckpointIngestInfo(region_id));
    FailPointHelper::disableFailPoint(FailPoints::force_set_fap_candidate_store_id);
    assertNoSegment(global_context.getTMTContext(), region, ingest_info_persisted);
}
CATCH

// Test cancel and destroy
TEST_F(RegionKVStoreTestFAP, Cancel3)
try
{
    using namespace std::chrono_literals;
    auto mock_data = prepareForRestart(FAPTestOpt{
        .mock_add_new_peer = true,
    })[0];
    RegionPtr kv_region = std::get<1>(mock_data);

    auto & global_context = TiFlashTestEnv::getGlobalContext();
    auto fap_context = global_context.getSharedContextDisagg()->fap_context;
    uint64_t region_id = 1;

    EngineStoreServerWrap server = {
        .tmt = &global_context.getTMTContext(),
        .proxy_helper = proxy_helper.get(),
    };

    kvstore->getStore().store_id.store(1, std::memory_order_release);
    kvstore->debugMutStoreMeta().set_id(1);
    ASSERT_EQ(1, kvstore->getStoreID());
    ASSERT_EQ(1, kvstore->clonedStoreMeta().id());
    FailPointHelper::enableFailPoint(FailPoints::force_set_fap_candidate_store_id);
    auto sp = SyncPointCtl::enableInScope("in_FastAddPeerImplWrite::after_write_segments");
    auto t = std::thread([&]() { FastAddPeer(&server, region_id, 2333); });
    sp.waitAndPause();
    EXPECT_THROW(kvstore->handleDestroy(region_id, global_context.getTMTContext()), Exception);
    sp.next();
    sp.disable();
    t.join();
    auto prev_fap_task_timeout_seconds = server.tmt->getContext().getSettingsRef().fap_task_timeout_seconds;
    SCOPE_EXIT({ server.tmt->getContext().getSettingsRef().fap_task_timeout_seconds = prev_fap_task_timeout_seconds; });
    server.tmt->getContext().getSettingsRef().fap_task_timeout_seconds = 0;
    // Use another call to cancel
    FastAddPeer(&server, region_id, 2333);
    LOG_INFO(log, "Try another destroy");
    kvstore->handleDestroy(region_id, global_context.getTMTContext());
    eventuallyPredicate([&]() {
        return !CheckpointIngestInfo::restore(global_context.getTMTContext(), proxy_helper.get(), region_id, 2333);
    });
    // Wait async cancel in `FastAddPeerImplWrite`.
    ASSERT_FALSE(fap_context->tryGetCheckpointIngestInfo(region_id).has_value());
    FailPointHelper::disableFailPoint(FailPoints::force_set_fap_candidate_store_id);
}
CATCH

// Test cancel and regular snapshot
TEST_F(RegionKVStoreTestFAP, Cancel4)
try
{
    using namespace std::chrono_literals;
    auto mock_data = prepareForRestart(FAPTestOpt{
        .mock_add_new_peer = true,
    })[0];
    KVStore & kvs = getKVS();
    RegionPtr kv_region = std::get<1>(mock_data);

    auto & global_context = TiFlashTestEnv::getGlobalContext();
    auto fap_context = global_context.getSharedContextDisagg()->fap_context;
    uint64_t region_id = 1;

    EngineStoreServerWrap server = {
        .tmt = &global_context.getTMTContext(),
        .proxy_helper = proxy_helper.get(),
    };

    kvstore->getStore().store_id.store(1, std::memory_order_release);
    kvstore->debugMutStoreMeta().set_id(1);
    ASSERT_EQ(1, kvstore->getStoreID());
    ASSERT_EQ(1, kvstore->clonedStoreMeta().id());
    FailPointHelper::enableFailPoint(FailPoints::force_set_fap_candidate_store_id);
    auto sp = SyncPointCtl::enableInScope("in_FastAddPeerImplWrite::after_write_segments");
    auto t = std::thread([&]() { FastAddPeer(&server, region_id, 2333); });
    sp.waitAndPause();

    // Test of ingesting multiple files with MultiSSTReader.
    MockSSTReader::getMockSSTData().clear();
    MockSSTGenerator default_cf{region_id, 1, ColumnFamilyType::Default};
    default_cf.finish_file();
    default_cf.freeze();
    kvs.mutProxyHelperUnsafe()->sst_reader_interfaces = make_mock_sst_reader_interface();
    // Exception: found running scheduled fap task
    EXPECT_THROW(
        proxy_instance->snapshot(kvs, global_context.getTMTContext(), region_id, {default_cf}, 10, 10, std::nullopt),
        Exception);
    sp.next();
    sp.disable();
    t.join();

    auto prev_fap_task_timeout_seconds = server.tmt->getContext().getSettingsRef().fap_task_timeout_seconds;
    SCOPE_EXIT({ server.tmt->getContext().getSettingsRef().fap_task_timeout_seconds = prev_fap_task_timeout_seconds; });
    server.tmt->getContext().getSettingsRef().fap_task_timeout_seconds = 0;
    // Use another call to cancel
    FastAddPeer(&server, region_id, 2333);
    eventuallyPredicate([&]() {
        auto ptr = fap_context->getOrRestoreCheckpointIngestInfo(
            global_context.getTMTContext(),
            proxy_helper.get(),
            region_id,
            2333);
        return ptr == nullptr
            && !CheckpointIngestInfo::restore(global_context.getTMTContext(), proxy_helper.get(), region_id, 2333);
    });

    {
        CheckpointIngestInfo::forciblyClean(
            global_context.getTMTContext(),
            proxy_helper.get(),
            region_id,
            false,
            CheckpointIngestInfo::CleanReason::ProxyFallback);
    }
    eventuallyPredicate([&]() {
        return !CheckpointIngestInfo::restore(global_context.getTMTContext(), proxy_helper.get(), region_id, 2333);
    });
    LOG_INFO(log, "Try another snapshot");
    proxy_instance->snapshot(
        kvs,
        global_context.getTMTContext(),
        region_id,
        {default_cf},
        kv_region->cloneMetaRegion(),
        2,
        11,
        11,
        std::nullopt,
        std::nullopt);
    eventuallyPredicate([&]() {
        return !CheckpointIngestInfo::restore(global_context.getTMTContext(), proxy_helper.get(), region_id, 2333);
    });
    // Wait async cancel in `FastAddPeerImplWrite`.
    ASSERT_FALSE(fap_context->tryGetCheckpointIngestInfo(region_id).has_value());
    FailPointHelper::disableFailPoint(FailPoints::force_set_fap_candidate_store_id);
}
CATCH

// Test cancel when building segments
TEST_F(RegionKVStoreTestFAP, Cancel5)
try
{
    auto mock_data = prepareForRestart(FAPTestOpt{.second_region = true});

    auto & global_context = TiFlashTestEnv::getGlobalContext();
    auto fap_context = global_context.getSharedContextDisagg()->fap_context;
    std::mutex exe_mut;
    std::unique_lock exe_lock(exe_mut);
    fap_context->tasks_trace->addTask(1, [&]() {
        // Keep the task in `tasks_trace` to prevent from canceling.
        std::scoped_lock wait_exe_lock(exe_mut);
        return genFastAddPeerResFail(FastAddPeerStatus::NoSuitable);
    });
    std::mutex exe_mut2;
    std::unique_lock exe_lock2(exe_mut2);
    fap_context->tasks_trace->addTask(2, [&]() {
        // Keep the task in `tasks_trace` to prevent from canceling.
        std::scoped_lock wait_exe_lock(exe_mut2);
        return genFastAddPeerResFail(FastAddPeerStatus::NoSuitable);
    });
    FailPointHelper::enableFailPoint(FailPoints::pause_when_building_fap_segments);
    std::packaged_task<FastAddPeerRes()> task([&]() {
        return FastAddPeerImplWrite(
            global_context.getTMTContext(),
            proxy_helper.get(),
            1,
            2333,
            std::move(mock_data[0]),
            0);
    });
    std::packaged_task<FastAddPeerRes()> task2([&]() {
        return FastAddPeerImplWrite(
            global_context.getTMTContext(),
            proxy_helper.get(),
            2,
            2334,
            std::move(mock_data[1]),
            0);
    });
    auto result = task.get_future();
    auto result2 = task2.get_future();
    std::thread t([&]() { task(); });
    std::thread t2([&]() { task2(); });
    using namespace std::chrono_literals;
    std::this_thread::sleep_for(1s);
    fap_context->tasks_trace->asyncCancelTask(1);
    FailPointHelper::disableFailPoint(FailPoints::pause_when_building_fap_segments);
    // Can see log "FAP is canceled when building segments" and "FAP is canceled after build segments".
    t.join();
    t2.join();
    exe_lock.unlock();
    exe_lock2.unlock();
    ASSERT_EQ(result.get().status, FastAddPeerStatus::Ok);
    ASSERT_EQ(result2.get().status, FastAddPeerStatus::Ok);
}
CATCH

TEST_F(RegionKVStoreTestFAP, EmptySegment)
try
{
    auto mock_data = prepareForRestart(FAPTestOpt{.persist_empty_segment = true})[0];
    RegionPtr kv_region = std::get<1>(mock_data);

    auto & global_context = TiFlashTestEnv::getGlobalContext();
    auto fap_context = global_context.getSharedContextDisagg()->fap_context;
    uint64_t region_id = 1;
    fap_context->tasks_trace->addTask(region_id, []() { return genFastAddPeerResFail(FastAddPeerStatus::NoSuitable); });
    EXPECT_THROW(
        FastAddPeerImplWrite(
            global_context.getTMTContext(),
            proxy_helper.get(),
            region_id,
            2333,
            std::move(mock_data),
            0),
        Exception);
}
CATCH

TEST_F(RegionKVStoreTestFAP, OnExistingPeer)
try
{
    auto mock_data = prepareForRestart(FAPTestOpt{})[0];
    RegionPtr kv_region = std::get<1>(mock_data);

    auto & global_context = TiFlashTestEnv::getGlobalContext();
    auto fap_context = global_context.getSharedContextDisagg()->fap_context;
    uint64_t region_id = 1;

    KVStore & kvs = getKVS();
    MockSSTReader::getMockSSTData().clear();
    MockSSTGenerator default_cf{region_id, 1, ColumnFamilyType::Default};
    default_cf.finish_file();
    default_cf.freeze();
    kvs.mutProxyHelperUnsafe()->sst_reader_interfaces = make_mock_sst_reader_interface();
    proxy_instance->snapshot(
        kvs,
        global_context.getTMTContext(),
        region_id,
        {default_cf},
        kv_region->cloneMetaRegion(),
        2,
        10,
        10,
        std::nullopt,
        std::nullopt);

    std::mutex exe_mut;
    std::unique_lock exe_lock(exe_mut);
    fap_context->tasks_trace->addTask(region_id, [&]() {
        // Keep the task in `tasks_trace` to prevent from canceling.
        std::scoped_lock wait_exe_lock(exe_mut);
        return genFastAddPeerResFail(FastAddPeerStatus::NoSuitable);
    });
    FastAddPeerImplWrite(global_context.getTMTContext(), proxy_helper.get(), region_id, 2333, std::move(mock_data), 0);
    exe_lock.unlock();
    fap_context->tasks_trace->fetchResult(region_id);

    auto region_to_ingest
        = fap_context
              ->getOrRestoreCheckpointIngestInfo(global_context.getTMTContext(), proxy_helper.get(), region_id, 2333)
              ->getRegion();
    // Make sure prehandling will not clean fap snapshot.
    std::vector<SSTView> ssts;
    SSTViewVec snaps{ssts.data(), ssts.size()};
    kvs.preHandleSnapshotToFiles(kv_region, snaps, 100, 100, std::nullopt, global_context.getTMTContext());

    EXPECT_THROW(
        ApplyFapSnapshotImpl(
            global_context.getTMTContext(),
            proxy_helper.get(),
            region_id,
            2333,
            false,
            region_to_ingest->appliedIndex(),
            region_to_ingest->appliedIndexTerm()),
        Exception);
}
CATCH

TEST_F(RegionKVStoreTestFAP, FAPWorkerException)
try
{
    auto mock_data = prepareForRestart(FAPTestOpt{})[0];
    KVStore & kvs = getKVS();
    RegionPtr kv_region = std::get<1>(mock_data);
    auto apply_state = std::get<2>(mock_data);

    auto & global_context = TiFlashTestEnv::getGlobalContext();
    auto fap_context = global_context.getSharedContextDisagg()->fap_context;
    uint64_t region_id = 1;

    EngineStoreServerWrap server = {
        .tmt = &global_context.getTMTContext(),
        .proxy_helper = proxy_helper.get(),
    };

    auto st = QueryFapSnapshotState(
        &server,
        region_id,
        2333,
        kv_region->getMeta().appliedIndex(),
        kv_region->getMeta().appliedIndexTerm());
    ASSERT_EQ(st, FapSnapshotState::NotFound);
    ASSERT_EQ(fap_context->tasks_trace->queryState(region_id), FAPAsyncTasks::TaskState::NotScheduled);

    kvstore->getStore().store_id.store(1, std::memory_order_release);
    kvstore->debugMutStoreMeta().set_id(1);
    ASSERT_EQ(1, kvstore->getStoreID());
    ASSERT_EQ(1, kvstore->clonedStoreMeta().id());
    FailPointHelper::enableFailPoint(FailPoints::force_fap_worker_throw);
    FailPointHelper::enableFailPoint(FailPoints::force_set_fap_candidate_store_id);
    // The FAP will fail because it doesn't contain the new peer in region meta.
    FastAddPeer(&server, region_id, 2333);
    eventuallyPredicateEx(
        [&]() { return fap_context->tasks_trace->queryState(region_id) == FAPAsyncTasks::TaskState::Finished; },
        [&]() {
            LOG_ERROR(log, "Final state is {}", magic_enum::enum_name(fap_context->tasks_trace->queryState(region_id)));
            throw Exception("not meet");
        });
    eventuallyPredicate([&]() {
        return !CheckpointIngestInfo::restore(global_context.getTMTContext(), proxy_helper.get(), region_id, 2333);
    });
    ASSERT_TRUE(!fap_context->tryGetCheckpointIngestInfo(region_id).has_value());
    // Now we try to apply regular snapshot.
    // Note that if an fap snapshot is in stage 1, no regular snapshot could happen,
    // because no MsgAppend is handled, such that no following MsgSnapshot could be sent.
    {
        MockSSTReader::getMockSSTData().clear();
        MockSSTGenerator default_cf{901, 800, ColumnFamilyType::Default};
        default_cf.finish_file();
        default_cf.freeze();
        kvs.mutProxyHelperUnsafe()->sst_reader_interfaces = make_mock_sst_reader_interface();
        proxy_instance->snapshot(
            kvs,
            global_context.getTMTContext(),
            region_id,
            {default_cf},
            kv_region->cloneMetaRegion(),
            2,
            0,
            0,
            std::nullopt,
            std::nullopt);
    }
    ASSERT_EQ(fap_context->tasks_trace->queryState(region_id), FAPAsyncTasks::TaskState::NotScheduled);

    FailPointHelper::disableFailPoint(FailPoints::force_fap_worker_throw);
    FailPointHelper::disableFailPoint(FailPoints::force_set_fap_candidate_store_id);
}
CATCH

TEST_F(RegionKVStoreTestFAP, TableNotFound)
try
{
    auto mock_data = prepareForRestart(FAPTestOpt{})[0];
    RegionPtr kv_region = std::get<1>(mock_data);

    auto & global_context = TiFlashTestEnv::getGlobalContext();
    auto & tmt = global_context.getTMTContext();
    uint64_t region_id = 1;

    auto keyspace_id = kv_region->getKeyspaceID();
    auto table_id = kv_region->getMappedTableID();
    auto fap_context = global_context.getSharedContextDisagg()->fap_context;

    std::mutex exe_mut;
    std::unique_lock exe_lock(exe_mut);
    fap_context->tasks_trace->addTask(region_id, [&]() {
        // Keep the task in `tasks_trace` to prevent from canceling.
        std::scoped_lock wait_exe_lock(exe_mut);
        return genFastAddPeerResFail(FastAddPeerStatus::NoSuitable);
    });

    // Mock that the storage instance have been dropped
    auto & storages = tmt.getStorages();
    storages.remove(keyspace_id, table_id);
    auto res = FastAddPeerImplWrite(
        global_context.getTMTContext(),
        proxy_helper.get(),
        region_id,
        2333,
        std::move(mock_data),
        0);
    ASSERT_EQ(res.status, FastAddPeerStatus::BadData);
}
CATCH

} // namespace tests
} // namespace DB
