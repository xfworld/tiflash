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

#include <Interpreters/Context_fwd.h>
#include <Parsers/IAST_fwd.h>
#include <Storages/DeltaMerge/DeltaMergeInterfaces.h>
#include <Storages/DeltaMerge/RowKeyRange.h>
#include <Storages/KVStore/Decode/RegionDataRead.h>
#include <Storages/KVStore/FFI/JointThreadAllocInfo.h>
#include <Storages/KVStore/KVStoreConfig.h>
#include <Storages/KVStore/MultiRaft/Disagg/RaftLogManager.h>
#include <Storages/KVStore/MultiRaft/PreHandlingTrace.h>
#include <Storages/KVStore/MultiRaft/RegionManager.h>
#include <Storages/KVStore/MultiRaft/RegionRangeKeys.h>
#include <Storages/KVStore/Region_fwd.h>
#include <Storages/KVStore/StorageEngineType.h>

#include <magic_enum.hpp>

namespace TiDB
{
struct TableInfo;
}
namespace DB
{
namespace RegionBench
{
struct DebugKVStore;
} // namespace RegionBench
namespace DM
{
enum class FileConvertJobType;
struct ExternalDTFileInfo;
} // namespace DM

namespace tests
{
class KVStoreTestBase;
} // namespace tests

class KVStore;
using KVStorePtr = std::shared_ptr<KVStore>;

class RegionTable;
struct RaftCommandResult;
class KVStoreTaskLock;

struct MockRaftCommand;
struct MockTiDBTable;
struct TiKVRangeKey;

class TMTContext;

struct SSTViewVec;
struct WriteCmdsView;

enum class EngineStoreApplyRes : uint32_t;

struct TiFlashRaftProxyHelper;
class ReadIndexWorkerManager;
using BatchReadIndexRes = std::vector<std::pair<kvrpcpb::ReadIndexResponse, uint64_t>>;
class ReadIndexStressTest;
struct FileUsageStatistics;
class PathPool;
class RegionPersister;
struct CheckpointIngestInfo;
using CheckpointIngestInfoPtr = std::shared_ptr<CheckpointIngestInfo>;
class UniversalPageStorage;
using UniversalPageStoragePtr = std::shared_ptr<UniversalPageStorage>;

enum class PersistRegionReason
{
    Debug,
    UselessAdminCommand, // Does not include passive CompactLog
    AdminCommand,
    Flush, // passive CompactLog
    ProactiveFlush,
    ApplySnapshotPrevRegion,
    ApplySnapshotCurRegion,
    IngestSst,
    EagerRaftGc,
};

constexpr const char * PersistRegionReasonMap[magic_enum::enum_count<PersistRegionReason>()] = {
    "debug",
    "admin cmd useless",
    "admin raft cmd",
    "tryFlushRegionData",
    "ProactiveFlush",
    "save previous region before apply",
    "save current region after apply",
    "ingestsst",
    "eager raft log gc",
};

static_assert(magic_enum::enum_count<PersistRegionReason>() == sizeof(PersistRegionReasonMap) / sizeof(const char *));

struct ProxyConfigSummary
{
    bool valid = false;
    size_t snap_handle_pool_size = 0;
    std::string engine_addr;
};

/// KVStore manages raft replication and transactions.
/// - Holds all regions in this TiFlash store.
/// - Manages region -> table mapping.
/// - Manages persistence of all regions.
/// - Implements learner read.
/// - Wraps FFI interfaces.
/// - Use `Decoder` to transform row format into col format.
class KVStore final : private boost::noncopyable
{
public:
    using RegionRange = RegionRangeKeys::RegionRange;

    explicit KVStore(Context & context);
    ~KVStore();

    size_t regionSize() const;
    const TiFlashRaftProxyHelper * getProxyHelper() const { return proxy_helper; }
    // Exported only for tests.
    TiFlashRaftProxyHelper * mutProxyHelperUnsafe() { return const_cast<TiFlashRaftProxyHelper *>(proxy_helper); }
    void setStore(metapb::Store);
    // May return 0 if uninitialized
    StoreID getStoreID(std::memory_order = std::memory_order_relaxed) const;
    metapb::Store clonedStoreMeta() const;
    const metapb::Store & getStoreMeta() const;
    metapb::Store & debugMutStoreMeta();
    FileUsageStatistics getFileUsageStatistics() const;
    // Proxy will validate and refit the config items from the toml file.
    const ProxyConfigSummary & getProxyConfigSummay() const { return proxy_config_summary; }
    void reportThreadAllocInfo(std::string_view, ReportThreadAllocateInfoType type, uint64_t value) const;
    static void reportThreadAllocBatch(std::string_view, ReportThreadAllocateInfoBatch data);
    JointThreadInfoJeallocMapPtr getJointThreadInfoJeallocMap() const { return joint_memory_allocation_map; }
    void fetchProxyConfig(const TiFlashRaftProxyHelper * proxy_helper);

public: // Region Management
    void restore(PathPool & path_pool, const TiFlashRaftProxyHelper *);
    void gcPersistedRegion(Seconds gc_persist_period = Seconds(60 * 5));
    RegionMap getRegionsByRangeOverlap(const RegionRange & range) const;
    void traverseRegions(std::function<void(RegionID, const RegionPtr &)> && callback) const;
    RegionPtr genRegionPtr(
        metapb::Region && region,
        UInt64 peer_id,
        UInt64 index,
        UInt64 term,
        std::optional<std::reference_wrapper<RegionTable>> region_table);
    void handleDestroy(UInt64 region_id, TMTContext & tmt);
    void setKVStoreMemoryLimit(size_t s) { maximum_kvstore_memory = s; }
    size_t getKVStoreMemoryLimit() const { return maximum_kvstore_memory; }

    // `genRegionTaskLock` make public for `GetLockByKey`.
    // TODO: find a better way to wrap the function?
    RegionTaskLock genRegionTaskLock(UInt64 region_id) const;
    RegionPtr getRegion(RegionID region_id) const;

public: // Raft Read and Write
    EngineStoreApplyRes handleAdminRaftCmd(
        raft_cmdpb::AdminRequest && request,
        raft_cmdpb::AdminResponse && response,
        UInt64 region_id,
        UInt64 index,
        UInt64 term,
        TMTContext & tmt);
    EngineStoreApplyRes handleWriteRaftCmd(
        const WriteCmdsView & cmds,
        UInt64 region_id,
        UInt64 index,
        UInt64 term,
        TMTContext & tmt);
    EngineStoreApplyRes handleWriteRaftCmdInner(
        const WriteCmdsView & cmds,
        UInt64 region_id,
        UInt64 index,
        UInt64 term,
        TMTContext & tmt,
        DM::WriteResult & write_result);

public: // Configs
    void reloadConfig(const Poco::Util::AbstractConfiguration & config_file) { config.reloadConfig(config_file, log); }
    const KVStoreConfig & getConfigRef() const { return config; }
    UInt64 getRaftLogEagerGCRows() const { return config.regionEagerGCLogGap(); }

    // debug only
    KVStoreConfig & debugGetConfigMut() { return config; }

public: // Flush
    static bool tryFlushRegionCacheInStorage(
        TMTContext & tmt,
        const Region & region,
        const LoggerPtr & log,
        bool try_until_succeed = true);
    bool needFlushRegionData(UInt64 region_id, TMTContext & tmt);
    bool tryFlushRegionData(
        UInt64 region_id,
        bool force_persist,
        bool try_until_succeed,
        TMTContext & tmt,
        UInt64 index,
        UInt64 term,
        uint64_t truncated_index,
        uint64_t truncated_term);
    // TODO(proactive flush)
    // void proactiveFlushCacheAndRegion(TMTContext & tmt, const DM::RowKeyRange & rowkey_range, KeyspaceID keyspace_id, TableID table_id, bool is_background);
    void notifyCompactLog(
        RegionID region_id,
        UInt64 compact_index,
        UInt64 compact_term,
        bool is_background,
        bool lock_held = true);
    RaftLogEagerGcTasks::Hints getRaftLogGcHints();
    void applyRaftLogGcTaskRes(const RaftLogGcTasksRes & res) const;

public: // Raft Snapshot
    void handleIngestCheckpoint(RegionPtr region, CheckpointIngestInfoPtr checkpoint_info, TMTContext & tmt);
    // For Raftstore V2, there could be some orphan keys in the write column family being left to `new_region` after pre-handled.
    // All orphan write keys are asserted to be replayed before reaching `deadline_index`.
    PrehandleResult preHandleSnapshotToFiles(
        RegionPtr new_region,
        SSTViewVec,
        uint64_t index,
        uint64_t term,
        std::optional<uint64_t> deadline_index,
        TMTContext & tmt);
    template <typename RegionPtrWrap>
    void applyPreHandledSnapshot(const RegionPtrWrap &, TMTContext & tmt);
    template <typename RegionPtrWrap>
    void releasePreHandledSnapshot(const RegionPtrWrap &, TMTContext & tmt);
    void abortPreHandleSnapshot(uint64_t region_id, TMTContext & tmt);
    size_t getOngoingPrehandleTaskCount() const;
    size_t getOngoingPrehandleSubtaskCount() const;
    EngineStoreApplyRes handleIngestSST(UInt64 region_id, SSTViewVec, UInt64 index, UInt64 term, TMTContext & tmt);
    size_t getMaxParallelPrehandleSize() const;
    size_t getMaxPrehandleSubtaskSize() const;

public: // Raft Read
    void addReadIndexEvent(Int64 f) { read_index_event_flag += f; }
    Int64 getReadIndexEvent() const { return read_index_event_flag; }
    BatchReadIndexRes batchReadIndex(const std::vector<kvrpcpb::ReadIndexRequest> & req, uint64_t timeout_ms) const;
    /// Initialize read-index worker context. It only can be invoked once.
    /// `worker_coefficient` means `worker_coefficient * runner_cnt` workers will be created.
    /// `runner_cnt` means number of runner which controls behavior of worker.
    void initReadIndexWorkers(
        std::function<std::chrono::milliseconds()> && fn_min_dur_handle_region,
        size_t runner_cnt,
        size_t worker_coefficient = 64);
    /// Create `runner_cnt` threads to run ReadIndexWorker asynchronously and automatically.
    /// If there is other runtime framework, DO NOT invoke it.
    void asyncRunReadIndexWorkers() const;
    /// Stop workers after there is no more read-index task.
    void stopReadIndexWorkers() const;
    /// TODO: if supported by runtime framework, run one round for specific runner by `id`.
    void runOneRoundOfReadIndexRunner(size_t runner_id);

#ifndef DBMS_PUBLIC_GTEST
private:
#endif
    friend struct MockRaftStoreProxy;
    friend class MockTiDB;
    friend struct MockTiDBTable;
    friend struct MockRaftCommand;
    friend class RegionMockTest;
    friend class NaturalDag;
    using DBGInvokerPrinter = std::function<void(const std::string &)>;
    friend void dbgFuncRemoveRegion(Context &, const ASTs &, DBGInvokerPrinter);
    friend void dbgFuncPutRegion(Context &, const ASTs &, DBGInvokerPrinter);
    friend class tests::KVStoreTestBase;
    friend class ReadIndexStressTest;
    friend struct RegionBench::DebugKVStore;
    struct StoreMeta
    {
        mutable std::mutex mu;

        using Base = metapb::Store;
        Base base;
        std::atomic_uint64_t store_id{0};
        void update(Base &&);
        Base getMeta() const;
        friend class KVStore;
    };
    StoreMeta & getStore();
    const StoreMeta & getStore() const;

    //  ---- Raft Snapshot ----  //

    PrehandleResult preHandleSSTsToDTFiles(
        RegionPtr new_region,
        const SSTViewVec,
        uint64_t index,
        uint64_t term,
        DM::FileConvertJobType,
        TMTContext & tmt);

    // Note that if there are committed rows in `new_region`, those rows will be left to
    // the in-memory `RegionData` but not flushed into the IStorage layer after calling
    // `checkAndApplyPreHandledSnapshot`.
    // `PreHandle` should take care of the committed rows and only leave uncommitted
    // key-values in the `new_region`.
    template <typename RegionPtrWrap>
    void checkAndApplyPreHandledSnapshot(const RegionPtrWrap & new_region, TMTContext & tmt);
    template <typename RegionPtrWrap>
    void onSnapshot(const RegionPtrWrap & new_region, RegionPtr old_region, UInt64 old_region_index, TMTContext & tmt);

    RegionPtr handleIngestSSTByDTFile(
        const RegionPtr & region,
        const SSTViewVec,
        UInt64 index,
        UInt64 term,
        TMTContext & tmt);

    //  ---- Region Management ----  //

    // Remove region from this TiFlash node.
    // If region is destroy or moved to another node(change peer),
    // set `remove_data` true to remove obsolete data from storage.
    void removeRegion(
        RegionID region_id,
        bool remove_data,
        RegionTable & region_table,
        const KVStoreTaskLock & task_lock,
        const RegionTaskLock & region_lock);
    KVStoreTaskLock genTaskLock() const;
    RegionManager::RegionReadLock genRegionMgrReadLock() const;
    RegionManager::RegionWriteLock genRegionMgrWriteLock(const KVStoreTaskLock &);
    void handleDestroy(UInt64 region_id, TMTContext & tmt, const KVStoreTaskLock &);

    //  ---- Region Write ----  //

    EngineStoreApplyRes handleUselessAdminRaftCmd(
        raft_cmdpb::AdminCmdType cmd_type,
        UInt64 curr_region_id,
        UInt64 index,
        UInt64 term,
        TMTContext & tmt) const;

    //  ---- Region Persistence ----  //

    /// Notice that if flush_if_possible is set to false, we only check if a flush is allowed by rowsize/size/interval.
    /// It will not check if a flush will eventually succeed.
    /// In other words, `canFlushRegionDataImpl(flush_if_possible=true)` can return false.
    bool canFlushRegionDataImpl(
        const RegionPtr & curr_region_ptr,
        UInt8 flush_if_possible,
        bool try_until_succeed,
        TMTContext & tmt,
        const RegionTaskLock & region_task_lock,
        UInt64 index,
        UInt64 term,
        UInt64 truncated_index,
        UInt64 truncated_term) const;

    bool forceFlushRegionDataImpl(
        Region & curr_region,
        bool try_until_succeed,
        TMTContext & tmt,
        const RegionTaskLock & region_task_lock,
        UInt64 index,
        UInt64 term) const;

    void persistRegion(
        const Region & region,
        const RegionTaskLock & region_task_lock,
        PersistRegionReason reason,
        const char * extra_msg) const;

    bool tryRegisterEagerRaftLogGCTask(const RegionPtr & region, RegionTaskLock &);

    //  ---- Raft Read ----  //

    void releaseReadIndexWorkers();

#ifndef DBMS_PUBLIC_GTEST
private:
#endif
    RegionManager region_manager;

    std::unique_ptr<RegionPersister> region_persister;

    std::atomic<Timepoint> last_gc_time = Timepoint::min();

    mutable std::mutex task_mutex;

    LoggerPtr log;

    KVStoreConfig config;

    mutable std::mutex bg_gc_region_data_mutex;
    std::list<RegionDataReadInfoList> bg_gc_region_data;

    const TiFlashRaftProxyHelper * proxy_helper{nullptr};

    // It should be initialized after `proxy_helper` is set.
    // It should be visited from outside after status of proxy is `Running`
    ReadIndexWorkerManager * read_index_worker_manager{nullptr};

    std::atomic_int64_t read_index_event_flag{0};

    PreHandlingTrace prehandling_trace;

    StoreMeta store;

    // Eager RaftLog GC
    const bool eager_raft_log_gc_enabled;
    // The index hints for eager RaftLog GC tasks
    RaftLogEagerGcTasks raft_log_gc_hints;
    // Relates to `queue_size` in `can_apply_snapshot`,
    // we can't have access to these codes though.
    std::atomic<int64_t> ongoing_prehandle_task_count{0};
    std::atomic<int64_t> ongoing_prehandle_subtask_count{0};
    ProxyConfigSummary proxy_config_summary;

    JointThreadInfoJeallocMapPtr joint_memory_allocation_map;
    size_t maximum_kvstore_memory = 0;

#ifdef DBMS_PUBLIC_GTEST
    std::atomic<size_t> debug_memory_limit_warning_count = 0;
#endif
};

/// Encapsulation of lock guard of task mutex in KVStore
class KVStoreTaskLock : private boost::noncopyable
{
    friend class KVStore;
    explicit KVStoreTaskLock(std::mutex & mutex_)
        : lock(mutex_)
    {}
    std::lock_guard<std::mutex> lock;
};

void WaitCheckRegionReady(KVStore & kvstore, const std::atomic_size_t & terminate_signals_counter);
void WaitCheckRegionReadyImpl(
    KVStore & kvstore,
    const std::atomic_size_t &,
    UInt64 read_index_timeout,
    double,
    double,
    double);

} // namespace DB
