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

#include <AggregateFunctions/registerAggregateFunctions.h>
#include <Common/CPUAffinityManager.h>
#include <Common/Config/ConfigReloader.h>
#include <Common/CurrentMetrics.h>
#include <Common/DiskSize.h>
#include <Common/DynamicThreadPool.h>
#include <Common/Exception.h>
#include <Common/FailPoint.h>
#include <Common/MemoryAllocTrace.h>
#include <Common/RedactHelpers.h>
#include <Common/SpillLimiter.h>
#include <Common/StringUtils/StringUtils.h>
#include <Common/ThreadManager.h>
#include <Common/TiFlashBuildInfo.h>
#include <Common/TiFlashException.h>
#include <Common/TiFlashMetrics.h>
#include <Common/UniThreadPool.h>
#include <Common/assert_cast.h>
#include <Common/config.h> // for ENABLE_NEXT_GEN
#include <Common/config.h>
#include <Common/escapeForFileName.h>
#include <Common/formatReadable.h>
#include <Common/getFQDNOrHostName.h>
#include <Common/getMultipleKeysFromConfig.h>
#include <Common/getNumberOfCPUCores.h>
#include <Common/grpcpp.h>
#include <Common/setThreadName.h>
#include <Core/TiFlashDisaggregatedMode.h>
#include <Flash/DiagnosticsService.h>
#include <Flash/FlashService.h>
#include <Flash/Mpp/GRPCCompletionQueuePool.h>
#include <Flash/Pipeline/Schedule/TaskScheduler.h>
#include <Flash/ResourceControl/LocalAdmissionController.h>
#include <Functions/registerFunctions.h>
#include <IO/BaseFile/RateLimiter.h>
#include <IO/Encryption/DataKeyManager.h>
#include <IO/Encryption/KeyspacesKeyManager.h>
#include <IO/Encryption/MockKeyManager.h>
#include <IO/FileProvider/FileProvider.h>
#include <IO/HTTPCommon.h>
#include <IO/IOThreadPools.h>
#include <IO/ReadHelpers.h>
#include <IO/UseSSL.h>
#include <Interpreters/AsynchronousMetrics.h>
#include <Interpreters/Context.h>
#include <Interpreters/ProcessList.h>
#include <Interpreters/SharedContexts/Disagg.h>
#include <Interpreters/loadMetadata.h>
#include <Poco/DirectoryIterator.h>
#include <Poco/StringTokenizer.h>
#include <Poco/Timestamp.h>
#include <Poco/Util/HelpFormatter.h>
#include <Poco/Util/LayeredConfiguration.h>
#include <Server/BgStorageInit.h>
#include <Server/Bootstrap.h>
#include <Server/CertificateReloader.h>
#include <Server/FlashGrpcServerHolder.h>
#include <Server/MetricsPrometheus.h>
#include <Server/RaftConfigParser.h>
#include <Server/Server.h>
#include <Server/ServerInfo.h>
#include <Server/Setup.h>
#include <Server/StatusFile.h>
#include <Server/StorageConfigParser.h>
#include <Server/TCPServersHolder.h>
#include <Server/UserConfigParser.h>
#include <Storages/DeltaMerge/ColumnFile/ColumnFileSchema.h>
#include <Storages/DeltaMerge/ReadThread/DMFileReaderPool.h>
#include <Storages/DeltaMerge/ReadThread/SegmentReadTaskScheduler.h>
#include <Storages/DeltaMerge/ReadThread/SegmentReader.h>
#include <Storages/DeltaMerge/ScanContext.h>
#include <Storages/FormatVersion.h>
#include <Storages/IManageableStorage.h>
#include <Storages/KVStore/FFI/FileEncryption.h>
#include <Storages/KVStore/ProxyStateMachine.h>
#include <Storages/KVStore/TMTContext.h>
#include <Storages/KVStore/TiKVHelpers/PDTiKVClient.h>
#include <Storages/Page/V3/Universal/UniversalPageStorage.h>
#include <Storages/PathCapacityMetrics.h>
#include <Storages/S3/FileCache.h>
#include <Storages/S3/S3Common.h>
#include <Storages/System/attachSystemTables.h>
#include <Storages/registerStorages.h>
#include <TiDB/Schema/SchemaSyncer.h>
#include <TiDB/Schema/TiDBSchemaManager.h>
#include <WindowFunctions/registerWindowFunctions.h>
#include <boost_wrapper/string_split.h>
#include <common/ErrorHandlers.h>
#include <common/config_common.h>
#include <common/logger_useful.h>

#include <ext/scope_guard.h>
#include <magic_enum.hpp>
#include <memory>

#ifdef FIU_ENABLE
#include <fiu.h>
#endif

extern std::atomic<UInt64> tranquil_time_rss;

namespace CurrentMetrics
{
extern const Metric LogicalCPUCores;
extern const Metric MemoryCapacity;
} // namespace CurrentMetrics

namespace DB
{
namespace ErrorCodes
{
extern const int NO_ELEMENTS_IN_CONFIG;
extern const int SUPPORT_IS_DISABLED;
extern const int ARGUMENT_OUT_OF_BOUND;
extern const int INVALID_CONFIG_PARAMETER;
} // namespace ErrorCodes

namespace Debug
{
extern void setServiceAddr(const std::string & addr);
}

static std::string getCanonicalPath(std::string path)
{
    Poco::trimInPlace(path);
    if (path.empty())
        throw Exception("path configuration parameter is empty");
    if (path.back() != '/')
        path += '/';
    return path;
}

void Server::uninitialize()
{
    logger().information("shutting down");
    BaseDaemon::uninitialize();
}

void Server::initialize(Poco::Util::Application & self)
{
    BaseDaemon::initialize(self);
    logger().information("starting up");
}

void Server::defineOptions(Poco::Util::OptionSet & options)
{
    options.addOption(
        Poco::Util::Option("help", "h", "show help and exit").required(false).repeatable(false).binding("help"));
    BaseDaemon::defineOptions(options);
}

int Server::run()
{
    if (config().hasOption("help"))
    {
        Poco::Util::HelpFormatter help_formatter(Server::options());
        auto header_str = fmt::format(
            "{} server [OPTION] [-- [POSITIONAL_ARGS]...]\n"
            "POSITIONAL_ARGS can be used to rewrite config properties, for example, --http_port=8010",
            commandName());
        help_formatter.setHeader(header_str);
        help_formatter.format(std::cout);
        return 0;
    }
    return BaseDaemon::run();
}

std::string Server::getDefaultCorePath() const
{
    return getCanonicalPath(config().getString("path")) + "cores";
}

pingcap::ClusterConfig getClusterConfig(
    TiFlashSecurityConfigPtr security_config,
    const int api_version,
    const LoggerPtr & log)
{
    pingcap::ClusterConfig config;
    config.tiflash_engine_key = "engine";
    config.tiflash_engine_value = DEF_PROXY_LABEL;
    auto [ca_path, cert_path, key_path] = security_config->getPaths();
    config.ca_path = ca_path;
    config.cert_path = cert_path;
    config.key_path = key_path;
    switch (api_version)
    {
    case 1:
        config.api_version = kvrpcpb::APIVersion::V1;
        break;
    case 2:
        config.api_version = kvrpcpb::APIVersion::V2;
        break;
    default:
        throw Exception(ErrorCodes::INVALID_CONFIG_PARAMETER, "Invalid api version {}", api_version);
    }
    LOG_INFO(
        log,
        "update cluster config, ca_path: {}, cert_path: {}, key_path: {}, api_version: {}",
        ca_path,
        cert_path,
        key_path,
        fmt::underlying(config.api_version));
    return config;
}

LoggerPtr grpc_log;

void printGRPCLog(gpr_log_func_args * args)
{
    String log_msg = fmt::format("{}, line number: {}, log msg : {}", args->file, args->line, args->message);
    if (args->severity == GPR_LOG_SEVERITY_DEBUG)
    {
        LOG_DEBUG(grpc_log, log_msg);
    }
    else if (args->severity == GPR_LOG_SEVERITY_INFO)
    {
        LOG_INFO(grpc_log, log_msg);
    }
    else if (args->severity == GPR_LOG_SEVERITY_ERROR)
    {
        LOG_ERROR(grpc_log, log_msg);
    }
}


// By default init global thread pool by hardware_concurrency
// Later we will adjust it by `adjustThreadPoolSize`
void initThreadPool(DisaggregatedMode disaggregated_mode)
{
    size_t default_num_threads = std::max(4UL, 2 * std::thread::hardware_concurrency());

    // Note: Global Thread Pool must be larger than sub thread pools.
    GlobalThreadPool::initialize(
        /*max_threads*/ default_num_threads * 20,
        /*max_free_threads*/ default_num_threads,
        /*queue_size*/ default_num_threads * 8);

    if (disaggregated_mode == DisaggregatedMode::Compute)
    {
        BuildReadTaskForWNPool::initialize(
            /*max_threads*/ default_num_threads,
            /*max_free_threads*/ default_num_threads / 2,
            /*queue_size*/ default_num_threads * 2);

        BuildReadTaskForWNTablePool::initialize(
            /*max_threads*/ default_num_threads,
            /*max_free_threads*/ default_num_threads / 2,
            /*queue_size*/ default_num_threads * 2);

        BuildReadTaskPool::initialize(
            /*max_threads*/ default_num_threads,
            /*max_free_threads*/ default_num_threads / 2,
            /*queue_size*/ default_num_threads * 2);

        RNWritePageCachePool::initialize(
            /*max_threads*/ default_num_threads,
            /*max_free_threads*/ default_num_threads / 2,
            /*queue_size*/ default_num_threads * 2);
    }

    if (disaggregated_mode == DisaggregatedMode::Compute || disaggregated_mode == DisaggregatedMode::Storage)
    {
        DataStoreS3Pool::initialize(
            /*max_threads*/ default_num_threads,
            /*max_free_threads*/ default_num_threads / 2,
            /*queue_size*/ default_num_threads * 2);
        S3FileCachePool::initialize(
            /*max_threads*/ default_num_threads,
            /*max_free_threads*/ default_num_threads / 2,
            /*queue_size*/ default_num_threads * 2);
    }

    if (disaggregated_mode == DisaggregatedMode::Storage)
    {
        WNEstablishDisaggTaskPool::initialize(
            /*max_threads*/ default_num_threads,
            /*max_free_threads*/ default_num_threads / 2,
            /*queue_size*/ default_num_threads * 2);
    }
}

void adjustThreadPoolSize(const Settings & settings, size_t logical_cores)
{
    // TODO: make BackgroundPool/BlockableBackgroundPool/DynamicThreadPool spawned from `GlobalThreadPool`
    size_t max_io_thread_count = std::ceil(settings.io_thread_count_scale * logical_cores);
    // Note: Global Thread Pool must be larger than sub thread pools.
    GlobalThreadPool::instance().setMaxThreads(max_io_thread_count * 200);
    GlobalThreadPool::instance().setMaxFreeThreads(max_io_thread_count);
    GlobalThreadPool::instance().setQueueSize(max_io_thread_count * 400);

    if (BuildReadTaskForWNPool::instance)
    {
        BuildReadTaskForWNPool::instance->setMaxThreads(max_io_thread_count);
        BuildReadTaskForWNPool::instance->setMaxFreeThreads(max_io_thread_count / 2);
        BuildReadTaskForWNPool::instance->setQueueSize(max_io_thread_count * 2);
    }
    if (BuildReadTaskForWNTablePool::instance)
    {
        BuildReadTaskForWNTablePool::instance->setMaxThreads(max_io_thread_count);
        BuildReadTaskForWNTablePool::instance->setMaxFreeThreads(max_io_thread_count / 2);
        BuildReadTaskForWNTablePool::instance->setQueueSize(max_io_thread_count * 2);
    }
    if (BuildReadTaskPool::instance)
    {
        BuildReadTaskPool::instance->setMaxThreads(max_io_thread_count);
        BuildReadTaskPool::instance->setMaxFreeThreads(max_io_thread_count / 2);
        BuildReadTaskPool::instance->setQueueSize(max_io_thread_count * 2);
    }
    if (DataStoreS3Pool::instance)
    {
        DataStoreS3Pool::instance->setMaxThreads(max_io_thread_count);
        DataStoreS3Pool::instance->setMaxFreeThreads(max_io_thread_count / 2);
        DataStoreS3Pool::instance->setQueueSize(max_io_thread_count * 2);
    }
    if (S3FileCachePool::instance)
    {
        S3FileCachePool::instance->setMaxThreads(max_io_thread_count);
        S3FileCachePool::instance->setMaxFreeThreads(max_io_thread_count / 2);
        S3FileCachePool::instance->setQueueSize(max_io_thread_count * 2);
    }
    if (RNWritePageCachePool::instance)
    {
        RNWritePageCachePool::instance->setMaxThreads(max_io_thread_count);
        RNWritePageCachePool::instance->setMaxFreeThreads(max_io_thread_count / 2);
        RNWritePageCachePool::instance->setQueueSize(max_io_thread_count * 2);
    }

    size_t max_cpu_thread_count = std::ceil(settings.cpu_thread_count_scale * logical_cores);
    if (WNEstablishDisaggTaskPool::instance)
    {
        // Tasks of EstablishDisaggTask is computation-intensive.
        WNEstablishDisaggTaskPool::instance->setMaxThreads(max_cpu_thread_count);
        WNEstablishDisaggTaskPool::instance->setMaxFreeThreads(max_cpu_thread_count / 2);
        WNEstablishDisaggTaskPool::instance->setQueueSize(max_cpu_thread_count * 2);
    }
}

void syncSchemaWithTiDB(
    const TiFlashStorageConfig & storage_config,
    BgStorageInitHolder & bg_init_stores,
    const std::atomic_size_t & terminate_signals_counter,
    const std::unique_ptr<Context> & global_context,
    const LoggerPtr & log)
{
    /// Then, sync schemas with TiDB, and initialize schema sync service.
    /// If in API V2 mode, each keyspace's schema is fetch lazily.
    if (storage_config.api_version == 1)
    {
        Stopwatch watch;
        const UInt64 total_wait_seconds = global_context->getSettingsRef().ddl_restart_wait_seconds;
        static constexpr int retry_wait_seconds = 3;
        while (true)
        {
            if (watch.elapsedSeconds() > total_wait_seconds)
            {
                LOG_WARNING(log, "Sync schemas during init timeout, cost={:.3f}s", watch.elapsedSeconds());
                break;
            }

            try
            {
                global_context->getTMTContext().getSchemaSyncerManager()->syncSchemas(*global_context, NullspaceID);
                LOG_INFO(log, "Sync schemas during init done, cost={:.3f}s", watch.elapsedSeconds());
                break;
            }
            catch (DB::Exception & e)
            {
                LOG_ERROR(
                    log,
                    "Bootstrap failed because sync schema error: {}\nWe will sleep for {}"
                    " seconds and try again.",
                    e.displayText(),
                    retry_wait_seconds);
                ::sleep(retry_wait_seconds);
            }
            catch (Poco::Exception & e)
            {
                LOG_ERROR(
                    log,
                    "Bootstrap failed because sync schema error: {}\nWe will sleep for {}"
                    " seconds and try again.",
                    e.displayText(),
                    retry_wait_seconds);
                ::sleep(retry_wait_seconds);
            }
        }
    }

    // Init the DeltaMergeStore instances if data exist.
    // Make the disk usage correct and prepare for serving
    // queries.
    bg_init_stores.start(
        *global_context,
        terminate_signals_counter,
        log,
        storage_config.lazily_init_store,
        storage_config.s3_config.isS3Enabled());

    // init schema sync service with tidb
    global_context->initializeSchemaSyncService();
}

void loadBlockList(
    [[maybe_unused]] const Poco::Util::LayeredConfiguration & config,
    Context & global_context,
    [[maybe_unused]] const LoggerPtr & log)
{
#if ENABLE_NEXT_GEN == 0
    // We do not support blocking store by id in OP mode currently.
    global_context.initializeStoreIdBlockList("");
#else
    global_context.initializeStoreIdBlockList(global_context.getSettingsRef().disagg_blocklist_wn_store_id);

    /// Load keyspace blocklist json file
    LOG_INFO(log, "Loading blocklist file.");
    auto blocklist_file_path = config.getString("blacklist_file", "");
    if (blocklist_file_path.length() == 0)
    {
        LOG_INFO(log, "blocklist file not enabled, ignore it.");
        return;
    }
    auto blacklist_file = Poco::File(blocklist_file_path);
    if (!(blacklist_file.exists() && blacklist_file.isFile() && blacklist_file.canRead()))
    {
        LOG_INFO(log, "blocklist file not exists or non-readble, ignore it, path={}", blocklist_file_path);
        return;
    }

    // Read the json file
    std::ifstream ifs(blocklist_file_path);
    std::string json_content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    Poco::JSON::Parser parser;
    Poco::Dynamic::Var json_var = parser.parse(json_content);
    const auto & json_obj = json_var.extract<Poco::JSON::Object::Ptr>();

    // load keyspace list
    auto keyspace_arr = json_obj->getArray("keyspace_ids");
    if (!keyspace_arr.isNull())
    {
        std::unordered_set<KeyspaceID> keyspace_blocklist;
        for (size_t i = 0; i < keyspace_arr->size(); i++)
        {
            keyspace_blocklist.emplace(keyspace_arr->getElement<KeyspaceID>(i));
        }
        global_context.initKeyspaceBlocklist(keyspace_blocklist);
    }

    // load region list
    auto region_arr = json_obj->getArray("region_ids");
    if (!region_arr.isNull())
    {
        std::unordered_set<RegionID> region_blocklist;
        for (size_t i = 0; i < region_arr->size(); i++)
        {
            region_blocklist.emplace(region_arr->getElement<RegionID>(i));
        }
        global_context.initRegionBlocklist(region_blocklist);
    }

    LOG_INFO(
        log,
        "Load blocklist file done, total {} keyspaces and {} regions in blocklist.",
        keyspace_arr.isNull() ? 0 : keyspace_arr->size(),
        region_arr.isNull() ? 0 : region_arr->size());
#endif
}

int Server::main(const std::vector<std::string> & /*args*/)
try
{
    setThreadName("TiFlashMain");

    UseSSL ssl_holder;

    const auto log = Logger::get();
#ifdef FIU_ENABLE
    fiu_init(0); // init failpoint
    FailPointHelper::initRandomFailPoints(config(), log);
#endif

    // Setup the config for jemalloc or mimalloc when enabled
    setupAllocator(log);

    // Setup the SIMD flags
    setupSIMD(log);

    registerFunctions();
    registerAggregateFunctions();
    registerWindowFunctions();
    registerStorages();

    const auto disagg_opt = DisaggOptions::parseFromConfig(config());

    // Later we may create thread pool from GlobalThreadPool
    // init it before other components
    initThreadPool(disagg_opt.mode);

    TiFlashErrorRegistry::instance(); // This invocation is for initializing

    DM::ScanContext::initCurrentInstanceId(config(), log);

    // Some Storage's config is necessary for Proxy
    TiFlashStorageConfig storage_config;
    // Deprecated settings.
    // `global_capacity_quota` will be ignored if `storage_config.main_capacity_quota` is not empty.
    // "0" by default, means no quota, the actual disk capacity is used.
    size_t global_capacity_quota = 0;
    std::tie(global_capacity_quota, storage_config) = TiFlashStorageConfig::parseSettings(config(), log);
    if (!storage_config.s3_config.bucket.empty())
    {
        storage_config.s3_config.enable(/*check_requirements*/ true, log);
    }
    else if (disagg_opt.mode == DisaggregatedMode::Compute && disagg_opt.use_autoscaler)
    {
        // compute node with auto scaler, the requirements will be initted later.
        storage_config.s3_config.enable(/*check_requirements*/ false, log);
    }

    if (storage_config.format_version != 0)
    {
        if (storage_config.s3_config.isS3Enabled() && !isStorageFormatForDisagg(storage_config.format_version))
        {
            auto message = fmt::format(
                "'storage.format_version' must be set to {} when S3 is enabled!",
                getStorageFormatsForDisagg());
            LOG_ERROR(log, message);
            throw Exception(ErrorCodes::INVALID_CONFIG_PARAMETER, message);
        }
        setStorageFormat(storage_config.format_version);
        LOG_INFO(log, "Using format_version={} (explicit storage format detected).", STORAGE_FORMAT_CURRENT.identifier);
    }
    else
    {
        if (storage_config.s3_config.isS3Enabled())
        {
            // If the user does not explicitly set format_version in the config file but
            // enables S3, then we set up a proper format version to support S3.
            setStorageFormat(DEFAULT_STORAGE_FORMAT_FOR_DISAGG.identifier);
            LOG_INFO(log, "Using format_version={} (infer by S3 is enabled).", STORAGE_FORMAT_CURRENT.identifier);
        }
        else
        {
            // Use the default settings
            LOG_INFO(log, "Using format_version={} (default settings).", STORAGE_FORMAT_CURRENT.identifier);
        }
    }

    // sanitize check for disagg mode
    if (storage_config.s3_config.isS3Enabled())
    {
        if (disagg_opt.mode == DisaggregatedMode::None)
        {
            const String message = "'flash.disaggregated_mode' must be set when S3 is enabled!";
            LOG_ERROR(log, message);
            throw Exception(ErrorCodes::INVALID_CONFIG_PARAMETER, message);
        }
    }

    // Set whether to use safe point v2.
    PDClientHelper::enable_safepoint_v2 = config().getBool("enable_safe_point_v2", false);

    /** Context contains all that query execution is dependent:
      *  settings, available functions, data types, aggregate functions, databases...
      */
    global_context = Context::createGlobal(Context::ApplicationType::SERVER, disagg_opt);

    /// Initialize users config reloader.
    auto users_config_reloader = UserConfig::parseSettings(config(), config_path, global_context, log);

    /// Load global settings from default_profile
    /// It internally depends on UserConfig::parseSettings.
    // TODO: Parse the settings from config file at the program beginning
    global_context->setDefaultProfiles();
    LOG_INFO(
        log,
        "Loaded global settings from default_profile and system_profile, changed configs: {{{}}}",
        global_context->getSettingsRef().toString());
    Settings & settings = global_context->getSettingsRef();

    // Init Proxy's config
    TiFlashProxyConfig proxy_conf( //
        config(),
        disagg_opt.mode,
        disagg_opt.use_autoscaler,
        STORAGE_FORMAT_CURRENT,
        settings,
        log);

    ProxyStateMachine proxy_machine{log, std::move(proxy_conf)};

    proxy_machine.runProxy();

    SCOPE_EXIT({ proxy_machine.waitProxyStopped(); });

    /// get CPU/memory/disk info of this server
    proxy_machine.getServerInfo(server_info, settings);

    grpc_log = Logger::get("grpc");
    gpr_set_log_verbosity(GPR_LOG_SEVERITY_DEBUG);
    gpr_set_log_function(&printGRPCLog);

    // Must init this before KVStore.
    global_context->initializeJointThreadInfoJeallocMap();

    /// Init File Provider
    if (proxy_machine.isProxyRunnable())
    {
        const bool enable_encryption = proxy_machine.getProxyHelper()->checkEncryptionEnabled();
        if (enable_encryption && storage_config.s3_config.isS3Enabled())
        {
            LOG_INFO(log, "encryption can be enabled, method is Aes256Ctr");
            // The UniversalPageStorage has not been init yet, the UniversalPageStoragePtr in KeyspacesKeyManager is nullptr.
            KeyManagerPtr key_manager
                = std::make_shared<KeyspacesKeyManager<TiFlashRaftProxyHelper>>(proxy_machine.getProxyHelper());
            global_context->initializeFileProvider(key_manager, true);
        }
        else if (enable_encryption)
        {
            const auto method = proxy_machine.getProxyHelper()->getEncryptionMethod();
            LOG_INFO(log, "encryption is enabled, method is {}", magic_enum::enum_name(method));
            KeyManagerPtr key_manager = std::make_shared<DataKeyManager>(proxy_machine.getEngineStoreServerWrap());
            global_context->initializeFileProvider(key_manager, method != EncryptionMethod::Plaintext);
        }
        else
        {
            LOG_INFO(log, "encryption is disabled");
            KeyManagerPtr key_manager = std::make_shared<DataKeyManager>(proxy_machine.getEngineStoreServerWrap());
            global_context->initializeFileProvider(key_manager, false);
        }
    }
    else
    {
        KeyManagerPtr key_manager = std::make_shared<MockKeyManager>(false);
        global_context->initializeFileProvider(key_manager, false);
    }

    /// ===== Paths related configuration initialized start ===== ///
    /// Note that theses global variables should be initialized by the following order:
    // 1. capacity
    // 2. path pool
    // 3. TMTContext

    LOG_INFO(
        log,
        "disaggregated_mode={} use_autoscaler={} enable_s3={}",
        magic_enum::enum_name(global_context->getSharedContextDisagg()->disaggregated_mode),
        disagg_opt.use_autoscaler,
        storage_config.s3_config.isS3Enabled());

    if (storage_config.s3_config.isS3Enabled())
        S3::ClientFactory::instance().init(storage_config.s3_config);

    global_context->getSharedContextDisagg()->initRemoteDataStore(
        global_context->getFileProvider(),
        storage_config.s3_config.isS3Enabled());

    const auto is_disagg_compute_mode = global_context->getSharedContextDisagg()->isDisaggregatedComputeMode();
    const auto is_disagg_storage_mode = global_context->getSharedContextDisagg()->isDisaggregatedStorageMode();
    const auto not_disagg_mode = global_context->getSharedContextDisagg()->notDisaggregatedMode();
    const auto [remote_cache_paths, remote_cache_capacity_quota]
        = storage_config.remote_cache_config.getCacheDirInfos(is_disagg_compute_mode);
    global_context->initializePathCapacityMetric( //
        global_capacity_quota, //
        storage_config.main_data_paths,
        storage_config.main_capacity_quota, //
        storage_config.latest_data_paths,
        storage_config.latest_capacity_quota,
        remote_cache_paths,
        remote_cache_capacity_quota);
    TiFlashRaftConfig raft_config = TiFlashRaftConfig::parseSettings(config(), log);
    global_context->setPathPool( //
        storage_config.main_data_paths, //
        storage_config.latest_data_paths, //
        storage_config.kvstore_data_path, //
        global_context->getPathCapacity(),
        global_context->getFileProvider());
    if (const auto & config = storage_config.remote_cache_config; config.isCacheEnabled() && is_disagg_compute_mode)
    {
        config.initCacheDir();
        FileCache::initialize(global_context->getPathCapacity(), config);
    }

    /// Determining PageStorage run mode based on current files on disk and storage config.
    /// Do it as early as possible after loading storage config.
    global_context->initializePageStorageMode(global_context->getPathPool(), STORAGE_FORMAT_CURRENT.page);

    // Use "system" as the default_database for all TCP connections, which is always exist in TiFlash.
    const std::string default_database = "system";
    Strings all_normal_path = storage_config.getAllNormalPaths();
    const std::string path = all_normal_path[0];
    global_context->setPath(path);

    /// ===== Paths related configuration initialized end ===== ///
    global_context->setSecurityConfig(config(), log);
    Redact::setRedactLog(global_context->getSecurityConfig()->redactInfoLog());

    // Create directories for 'path' and for default database, if not exist.
    for (const String & candidate_path : all_normal_path)
    {
        Poco::File(candidate_path + "data/" + default_database).createDirectories();
    }
    Poco::File(path + "metadata/" + default_database).createDirectories();

    StatusFile status{path + "status"};

    SCOPE_EXIT({
        // Set the TMTContext reference in `proxy_machine` to nullptr.
        proxy_machine.destroyProxyContext();
        /** Explicitly destroy Context. It is more convenient than in destructor of Server, because logger is still available.
          * At this moment, no one could own shared part of Context.
          */
        global_context.reset();

        LOG_INFO(log, "Destroyed global context.");
    });

    /// Try to increase limit on number of open files.
    setOpenFileLimit(config().getUInt("max_open_files", 0), log);

    static ServerErrorHandler error_handler;
    Poco::ErrorHandler::set(&error_handler);

    /// Initialize DateLUT early, to not interfere with running time of first query.
    LOG_DEBUG(log, "Initializing DateLUT.");
    DateLUT::instance();
    LOG_TRACE(log, "Initialized DateLUT with time zone `{}`.", DateLUT::instance().getTimeZone());

    /// Directory with temporary data for processing of heavy queries.
    {
        const std::string & temp_path = storage_config.temp_path;
        RUNTIME_CHECK(!temp_path.empty());
        Poco::File(temp_path).createDirectories();

        Poco::DirectoryIterator dir_end;
        for (Poco::DirectoryIterator it(temp_path); it != dir_end; ++it)
        {
            if (it->isFile() && startsWith(it.name(), "tmp"))
                global_context->getFileProvider()->deleteRegularFile(it->path(), EncryptionPath(it->path(), ""));
        }
        LOG_INFO(log, "temp files in temp directory({}) removed", temp_path);

        storage_config.checkTempCapacity(global_capacity_quota, log);
        global_context->setTemporaryPath(temp_path);
        SpillLimiter::instance->setMaxSpilledBytes(storage_config.temp_capacity);
    }

    /** Directory with 'flags': files indicating temporary settings for the server set by system administrator.
      * Flags may be cleared automatically after being applied by the server.
      * Examples: do repair of local data; clone all replicated tables from replica.
      */
    {
        Poco::File(path + "flags/").createDirectories();
        global_context->setFlagsPath(path + "flags/");
    }

    /// Init TiFlash metrics.
    global_context->initializeTiFlashMetrics();

    ///
    /// The config value in global settings can only be used from here because we just loaded it from config file.
    ///

    /// Initialize the background & blockable background thread pool.
    LOG_INFO(log, "Background & Blockable Background pool size: {}", settings.background_pool_size);
    auto & bg_pool = global_context->initializeBackgroundPool(settings.background_pool_size);
    auto & blockable_bg_pool = global_context->initializeBlockableBackgroundPool(settings.background_pool_size);
    // adjust the thread pool size according to settings and logical cores num
    adjustThreadPoolSize(settings, server_info.cpu_info.logical_cores);
    initStorageMemoryTracker(
        settings.max_memory_usage_for_all_queries.getActualBytes(server_info.memory_info.capacity),
        settings.bytes_that_rss_larger_than_limit);

    if (is_disagg_compute_mode)
    {
        // No need to have local index scheduler.
    }
    else if (is_disagg_storage_mode)
    {
        // There is no compute task in write node.
        // Set the pool size to 80% of logical cores and 60% of memory
        // to take full advantage of the resources and avoid blocking other tasks like writes and compactions.
        global_context->initializeGlobalLocalIndexerScheduler(
            std::max(1, server_info.cpu_info.logical_cores * 8 / 10), // at least 1 thread
            std::max(256 * 1024 * 1024ULL, server_info.memory_info.capacity * 6 / 10)); // at least 256MB
    }
    else
    {
        // There could be compute tasks, reserve more memory for computes.
        global_context->initializeGlobalLocalIndexerScheduler(
            std::max(1, server_info.cpu_info.logical_cores * 4 / 10), // at least 1 thread
            std::max(256 * 1024 * 1024ULL, server_info.memory_info.capacity * 4 / 10)); // at least 256MB
    }

    /// PageStorage run mode has been determined above
    global_context->initializeGlobalPageIdAllocator();
    if (!is_disagg_compute_mode)
    {
        global_context->initializeGlobalStoragePoolIfNeed(global_context->getPathPool());
        LOG_INFO(
            log,
            "Global PageStorage run mode is {}",
            magic_enum::enum_name(global_context->getPageStorageRunMode()));
    }

    /// Try to restore the StoreIdent from UniPS. There are many services that require
    /// `store_id` to generate the path to RemoteStore under disagg mode.
    std::optional<raft_serverpb::StoreIdent> store_ident;
    // Only when this node is disagg compute node and autoscaler is enabled, we don't need the WriteNodePageStorage instance
    // Disagg compute node without autoscaler still need this instance for proxy's data
    if (!(is_disagg_compute_mode && disagg_opt.use_autoscaler))
    {
        global_context->initializeWriteNodePageStorageIfNeed(global_context->getPathPool());
        if (auto wn_ps = global_context->tryGetWriteNodePageStorage(); wn_ps != nullptr)
        {
            if (proxy_machine.getProxyHelper()->checkEncryptionEnabled() && storage_config.s3_config.isS3Enabled())
            {
                global_context->getFileProvider()->setPageStoragePtrForKeyManager(wn_ps);
            }
            store_ident = tryGetStoreIdent(wn_ps);
            if (!store_ident)
            {
                LOG_INFO(log, "StoreIdent not exist, new tiflash node");
            }
            else
            {
                LOG_INFO(log, "StoreIdent restored, {{{}}}", store_ident->ShortDebugString());
            }
        }
    }

    if (is_disagg_storage_mode)
    {
        global_context->getSharedContextDisagg()->initWriteNodeSnapManager();
        global_context->getSharedContextDisagg()->initFastAddPeerContext(settings.fap_handle_concurrency);
    }

    if (is_disagg_compute_mode)
    {
        global_context->getSharedContextDisagg()->initReadNodePageCache(
            global_context->getPathPool(),
            storage_config.remote_cache_config.getPageCacheDir(),
            storage_config.remote_cache_config.getPageCapacity());
    }

    /// Initialize RateLimiter.
    global_context->initializeRateLimiter(config(), bg_pool, blockable_bg_pool);

    global_context->setServerInfo(server_info);
    if (server_info.memory_info.capacity == 0)
    {
        LOG_ERROR(
            log,
            "Failed to get memory capacity, float-pointing memory limit config (for example, set "
            "`max_memory_usage_for_all_queries` to `0.1`) won't take effect. If you set them as float-pointing value, "
            "you can change them to integer instead.");
    }
    else
    {
        LOG_INFO(
            log,
            "Detected memory capacity {} bytes, you have config `max_memory_usage_for_all_queries` to {}, finally "
            "limit to {} bytes.",
            server_info.memory_info.capacity,
            settings.max_memory_usage_for_all_queries.toString(),
            settings.max_memory_usage_for_all_queries.getActualBytes(server_info.memory_info.capacity));
    }

    /// Initialize main config reloader.
    auto main_config_reloader = std::make_unique<ConfigReloader>(
        config_path,
        [&](ConfigurationPtr config) {
            LOG_DEBUG(log, "run main config reloader");
            buildLoggers(*config);
            global_context->getTMTContext().reloadConfig(*config);
            global_context->getIORateLimiter().updateConfig(*config);
            global_context->reloadDeltaTreeConfig(*config);
            DM::SegmentReadTaskScheduler::instance().updateConfig(global_context->getSettingsRef());
            if (FileCache::instance() != nullptr)
            {
                FileCache::instance()->updateConfig(global_context->getSettingsRef());
            }
            {
                // update TiFlashSecurity and related config in client for ssl certificate reload.
                if (bool updated = global_context->getSecurityConfig()->update(*config); updated)
                {
                    auto raft_config = TiFlashRaftConfig::parseSettings(*config, log);
                    auto cluster_config
                        = getClusterConfig(global_context->getSecurityConfig(), storage_config.api_version, log);
                    global_context->getTMTContext().updateSecurityConfig(
                        std::move(raft_config),
                        std::move(cluster_config));
                    LOG_DEBUG(log, "TMTContext updated security config");
                }
            }
        },
        /* already_loaded = */ true);

    /// Reload config in SYSTEM RELOAD CONFIG query.
    global_context->setConfigReloadCallback([&]() {
        main_config_reloader->reload();

        if (users_config_reloader)
            users_config_reloader->reload();
    });

    /// Size of cache for marks (index of MergeTree family of tables). It is necessary.
    size_t mark_cache_size = config().getUInt64("mark_cache_size", DEFAULT_MARK_CACHE_SIZE);
    if (mark_cache_size)
        global_context->setMarkCache(mark_cache_size);

    /// Size of cache for minmax index, used by DeltaMerge engine.
    size_t minmax_index_cache_size = config().getUInt64("minmax_index_cache_size", mark_cache_size);
    if (minmax_index_cache_size)
        global_context->setMinMaxIndexCache(minmax_index_cache_size);

    /// The vector index cache by number instead of bytes. Because it use `mmap` and let the operator system decide the memory usage.
    size_t light_local_index_cache_entities = config().getUInt64("light_local_index_cache_entities", 10000);
    size_t heavy_local_index_cache_entities = config().getUInt64("heavy_local_index_cache_entities", 500);
    if (light_local_index_cache_entities && heavy_local_index_cache_entities)
        global_context->setLocalIndexCache(light_local_index_cache_entities, heavy_local_index_cache_entities);

    size_t column_cache_long_term_size
        = config().getUInt64("column_cache_long_term_size", 512 * 1024 * 1024 /* 512MB */);
    if (column_cache_long_term_size)
        global_context->setColumnCacheLongTerm(column_cache_long_term_size);

    /// Size of max memory usage of DeltaIndex, used by DeltaMerge engine.
    /// - In non-disaggregated mode, its default value is 0, means unlimited, and it
    ///   controls the number of total bytes keep in the memory.
    /// - In disaggregated mode, its default value is memory_capacity_of_host * 0.02.
    ///   0 means cache is disabled.
    ///   We cannot support unlimited delta index cache in disaggregated mode for now,
    ///   because cache items will be never explicitly removed.
    if (is_disagg_compute_mode)
    {
        constexpr auto delta_index_cache_ratio = 0.02;
        constexpr auto backup_delta_index_cache_size = 1024 * 1024 * 1024; // 1GiB
        const auto default_delta_index_cache_size = server_info.memory_info.capacity > 0
            ? server_info.memory_info.capacity * delta_index_cache_ratio
            : backup_delta_index_cache_size;
        size_t n = config().getUInt64("delta_index_cache_size", default_delta_index_cache_size);
        LOG_INFO(log, "delta_index_cache_size={}", n);
        // In disaggregated compute node, we will not use DeltaIndexManager to cache the delta index.
        // Instead, we use RNMVCCIndexCache.
        global_context->getSharedContextDisagg()->initReadNodeMVCCIndexCache(n);
    }
    else
    {
        size_t n = config().getUInt64("delta_index_cache_size", 0);
        global_context->setDeltaIndexManager(n);
    }

    loadBlockList(config(), *global_context, log);

    LOG_INFO(log, "Loading metadata.");
    loadMetadataSystem(*global_context); // Load "system" database. Its engine keeps as Ordinary.
    /// After attaching system databases we can initialize system log.
    global_context->initializeSystemLogs();
    /// After the system database is created, attach virtual system tables (in addition to query_log and part_log)
    attachSystemTablesServer(*global_context->getDatabase("system"));

    {
        /// Create TMTContext
        auto cluster_config = getClusterConfig(global_context->getSecurityConfig(), storage_config.api_version, log);
        global_context->createTMTContext(raft_config, std::move(cluster_config));

        // Must be executed before restore data.
        // Get the memory usage of tranquil time.
        auto mem_res = get_process_mem_usage();
        tranquil_time_rss = static_cast<Int64>(mem_res.resident_bytes);

        auto kvs_watermark = settings.max_memory_usage_for_all_queries.getActualBytes(server_info.memory_info.capacity);
        if (kvs_watermark == 0)
            kvs_watermark = server_info.memory_info.capacity * 0.8;
        LOG_INFO(
            log,
            "Global memory status: kvstore_high_watermark={} tranquil_time_rss={} cur_virt_size={} capacity={}",
            kvs_watermark,
            tranquil_time_rss,
            mem_res.cur_virt_bytes,
            server_info.memory_info.capacity);

        proxy_machine.initKVStore(global_context->getTMTContext(), store_ident, kvs_watermark);

        global_context->getTMTContext().reloadConfig(config());
        // setup the kv cluster for disagg compute node fetching config
        if (S3::ClientFactory::instance().isEnabled())
        {
            auto & tmt = global_context->getTMTContext();
            S3::ClientFactory::instance().setKVCluster(tmt.getKVCluster());
        }
    }
    LOG_INFO(log, "Init S3 GC Manager");
    global_context->getTMTContext().initS3GCManager(proxy_machine.getProxyHelper());
    // Initialize the thread pool of storage before the storage engine is initialized.
    LOG_INFO(log, "dt_enable_read_thread {}", global_context->getSettingsRef().dt_enable_read_thread);
    // `DMFileReaderPool` should be constructed before and destructed after `SegmentReaderPoolManager`.
    DM::DMFileReaderPool::instance();
    DM::SegmentReaderPoolManager::instance().init(
        server_info.cpu_info.logical_cores,
        settings.dt_read_thread_count_scale);
    DM::SegmentReadTaskScheduler::instance().updateConfig(global_context->getSettingsRef());

    auto schema_cache_size = config().getInt("schema_cache_size", 10000);
    global_context->initializeSharedBlockSchemas(schema_cache_size);

    // Load remaining databases
    loadMetadata(*global_context);
    LOG_DEBUG(log, "Load metadata done.");
    BgStorageInitHolder bg_init_stores;
    if (!is_disagg_compute_mode)
    {
        if (not_disagg_mode || store_ident.has_value())
        {
            // This node has been bootstrapped, the `store_id` is set. Or non-disagg mode,
            // do not depend on `store_id`. Start sync schema before serving any requests.
            // For the node has not been bootstrapped, this stage will be postpone.
            // FIXME: (bootstrap) we should bootstrap the tiflash node more early!
            syncSchemaWithTiDB(storage_config, bg_init_stores, terminate_signals_counter, global_context, log);
        }
    }
    // set default database for ch-client
    global_context->setCurrentDatabase(default_database);

    CPUAffinityManager::initCPUAffinityManager(config());
    LOG_INFO(log, "CPUAffinity: {}", CPUAffinityManager::getInstance().toString());
    SCOPE_EXIT({
        /** Ask to cancel background jobs all table engines,
          *  and also query_log.
          * It is important to do early, not in destructor of Context, because
          *  table engines could use Context on destroy.
          */
        LOG_INFO(log, "Shutting down storages.");
        // `SegmentReader` threads may hold a segment and its delta-index for read.
        // `Context::shutdown()` will destroy `DeltaIndexManager`.
        // So, stop threads explicitly before `TiFlashTestEnv::shutdown()`.
        DB::DM::SegmentReaderPoolManager::instance().stop();
        FileCache::shutdown();
        global_context->shutdown();
        if (storage_config.s3_config.isS3Enabled())
        {
            S3::ClientFactory::instance().shutdown();
        }
        LOG_DEBUG(log, "Shutted down storages.");
    });

    proxy_machine.restoreKVStore(global_context->getTMTContext(), global_context->getPathPool());

    /// setting up elastic thread pool
    bool enable_elastic_threadpool = settings.enable_elastic_threadpool;
    if (enable_elastic_threadpool)
        DynamicThreadPool::global_instance = std::make_unique<DynamicThreadPool>(
            settings.elastic_threadpool_init_cap,
            std::chrono::milliseconds(settings.elastic_threadpool_shrink_period_ms));
    SCOPE_EXIT({
        if (enable_elastic_threadpool)
        {
            assert(DynamicThreadPool::global_instance);
            DynamicThreadPool::global_instance.reset();
        }
    });

    // FIXME: (bootstrap) we should bootstrap the tiflash node more early!
    if (not_disagg_mode || /*has_been_bootstrap*/ store_ident.has_value())
    {
        // If S3 enabled, wait for all DeltaMergeStores' initialization
        // before this instance can accept requests.
        // Else it just do nothing.
        bg_init_stores.waitUntilFinish();
    }

    if (is_disagg_storage_mode && /*has_been_bootstrap*/ store_ident.has_value())
    {
        // Only disagg write node that has been bootstrap need wait. For the write node does not bootstrap, its
        // store id is allocated later.
        // Wait until all CheckpointInfo are restored from S3
        auto wn_ps = global_context->getWriteNodePageStorage();
        wn_ps->waitUntilInitedFromRemoteStore();
    }

    {
        TCPServersHolder tcp_http_servers_holder(
            *this,
            settings,
            global_context->getSecurityConfig(),
            /*max_connections=*/1024,
            log);

        main_config_reloader->addConfigObject(global_context->getSecurityConfig());
        main_config_reloader->start();
        if (users_config_reloader)
            users_config_reloader->start();

        {
            // on ARM processors it can show only enabled at current moment cores
            CurrentMetrics::set(CurrentMetrics::LogicalCPUCores, server_info.cpu_info.logical_cores);
            CurrentMetrics::set(CurrentMetrics::MemoryCapacity, server_info.memory_info.capacity);
            LOG_INFO(
                log,
                "Available RAM = {}; physical cores = {}; logical cores = {}.",
                server_info.memory_info.capacity,
                server_info.cpu_info.physical_cores,
                server_info.cpu_info.logical_cores);
        }

        LOG_INFO(log, "Ready for connections.");

        SCOPE_EXIT({
            is_cancelled = true;

            tcp_http_servers_holder.onExit();

            main_config_reloader.reset();
            users_config_reloader.reset();
        });

        /// This object will periodically calculate some metrics.
        /// should init after `createTMTContext` cause we collect some data from the TiFlash context object.
        AsynchronousMetrics async_metrics(*global_context);
        attachSystemTablesAsync(*global_context->getDatabase("system"), async_metrics);

        auto metrics_prometheus = std::make_unique<MetricsPrometheus>(*global_context, async_metrics);

        SessionCleaner session_cleaner(*global_context);
        auto & tmt_context = global_context->getTMTContext();

        proxy_machine.startProxyService(tmt_context, store_ident);
        if (proxy_machine.isProxyRunnable())
        {
            const auto store_id = tmt_context.getKVStore()->getStoreID(std::memory_order_seq_cst);
            if (is_disagg_compute_mode)
            {
                // compute node do not need to handle read index
                LOG_INFO(log, "store_id={}, tiflash proxy is ready to serve", store_id);
            }
            else
            {
                LOG_INFO(
                    log,
                    "store_id={}, tiflash proxy is ready to serve, try to wake up all regions' leader",
                    store_id);

                if (global_context->getSharedContextDisagg()->isDisaggregatedStorageMode() && !store_ident.has_value())
                {
                    // Not disagg node done it before
                    // For the disagg node has not been bootstrap, begin the very first schema sync with TiDB.
                    // FIXME: (bootstrap) we should bootstrap the tiflash node more early!
                    syncSchemaWithTiDB(storage_config, bg_init_stores, terminate_signals_counter, global_context, log);
                    bg_init_stores.waitUntilFinish();
                }
                proxy_machine.waitProxyServiceReady(tmt_context, terminate_signals_counter);
            }
        }

        SCOPE_EXIT({ proxy_machine.stopProxy(tmt_context); });

        {
            // Report the unix timestamp, git hash, release version
            Poco::Timestamp ts;
            GET_METRIC(tiflash_server_info, start_time).Set(ts.epochTime());
        }

        // For test mode, TaskScheduler and LAC is controlled by test case.
        // TODO: resource control is not supported for WN. So disable pipeline model and LAC.
        const bool init_pipeline_and_lac = !global_context->isTest() && !is_disagg_storage_mode;
        if (init_pipeline_and_lac)
        {
#ifdef DBMS_PUBLIC_GTEST
            LocalAdmissionController::global_instance = std::make_unique<MockLocalAdmissionController>();
#else
            const bool with_keyspace = (storage_config.api_version == 2);
            LocalAdmissionController::global_instance = std::make_unique<LocalAdmissionController>(
                tmt_context.getKVCluster(),
                tmt_context.getEtcdClient(),
                with_keyspace);
#endif

            auto get_pool_size = [](const auto & setting) {
                return setting == 0 ? getNumberOfLogicalCPUCores() : static_cast<size_t>(setting);
            };
            TaskSchedulerConfig config{
                {get_pool_size(settings.pipeline_cpu_task_thread_pool_size),
                 settings.pipeline_cpu_task_thread_pool_queue_type},
                {get_pool_size(settings.pipeline_io_task_thread_pool_size),
                 settings.pipeline_io_task_thread_pool_queue_type},
            };
            RUNTIME_CHECK(!TaskScheduler::instance);
            TaskScheduler::instance = std::make_unique<TaskScheduler>(config);
            LOG_INFO(log, "init pipeline task scheduler with {}", config.toString());
        }

        SCOPE_EXIT({
            if (init_pipeline_and_lac)
            {
                assert(TaskScheduler::instance);
                TaskScheduler::instance.reset();
                // Stop LAC instead of reset, because storage layer still needs it.
                // Workload will not be throttled when LAC is stopped.
                // It's ok because flash service has already been destructed, so throllting is meaningless.
                assert(LocalAdmissionController::global_instance);
                LocalAdmissionController::global_instance->safeStop();
            }
        });

        if (settings.enable_async_grpc_client)
        {
            auto size = settings.grpc_completion_queue_pool_size;
            if (size == 0)
                size = getNumberOfLogicalCPUCores();
            GRPCCompletionQueuePool::global_instance = std::make_unique<GRPCCompletionQueuePool>(size);
        }

        /// startup grpc server to serve raft and/or flash services.
        FlashGrpcServerHolder flash_grpc_server_holder(this->context(), this->config(), raft_config, log);

        SCOPE_EXIT({
            // Stop LAC for AutoScaler managed CN before FlashGrpcServerHolder is destructed.
            // Because AutoScaler it will kill tiflash process when port of flash_server_addr is down.
            // And we want to make sure LAC is cleanedup.
            // The effects are there will be no resource control during [lac.safeStop(), FlashGrpcServer destruct done],
            // but it's basically ok, that duration is small(normally 100-200ms).
            if (is_disagg_compute_mode && disagg_opt.use_autoscaler && LocalAdmissionController::global_instance)
                LocalAdmissionController::global_instance->safeStop();
        });

        proxy_machine.runKVStore(tmt_context);

        try
        {
            // Bind CPU affinity after all threads started.
            CPUAffinityManager::getInstance().bindThreadCPUAffinity();
        }
        catch (...)
        {
            LOG_ERROR(log, "CPUAffinityManager::bindThreadCPUAffinity throws exception.");
        }

        LOG_INFO(log, "Start to wait for terminal signal");
        waitForTerminationRequest();

        // Note: `waitAllMPPTasksFinish` must be called before stopping the proxy.
        // Otherwise, read index requests may fail, which can prevent TiFlash from shutting down gracefully.
        LOG_INFO(log, "Set unavailable for MPPTask");
        tmt_context.getMPPTaskManager()->setUnavailable();
        tmt_context.getMPPTaskManager()->getMPPTaskMonitor()->waitAllMPPTasksFinish(global_context);

        {
            // Set limiters stopping and wakeup threads in waitting queue.
            global_context->getIORateLimiter().setStop();
        }
    }

    return Application::EXIT_OK;
}
catch (...)
{
    // The default exception handler of Poco::Util::Application will catch the
    // `DB::Exception` as `Poco::Exception` and do not print the stacktrace.
    // So we catch all exceptions here and print the stacktrace.
    tryLogCurrentException("Server::main");
    auto code = getCurrentExceptionCode();
    return code > 0 ? code : 1;
}
} // namespace DB

int mainEntryClickHouseServer(int argc, char ** argv)
{
    DB::Server app;
    try
    {
        return app.run(argc, argv);
    }
    catch (...)
    {
        std::cerr << DB::getCurrentExceptionMessage(true) << "\n";
        auto code = DB::getCurrentExceptionCode();
        return code ? code : 1;
    }
}
