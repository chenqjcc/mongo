/**
 *    Copyright (C) 2015 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/sharding_state.h"

#include "mongo/base/init.h"
#include "mongo/bson/util/bson_extract.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/replica_set_monitor.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/client.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/ops/update.h"
#include "mongo/db/ops/update_lifecycle_impl.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/chunk_splitter.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharded_connection_info.h"
#include "mongo/db/s/sharding_initialization_mongod.h"
#include "mongo/db/s/type_shard_identity.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/rpc/metadata/config_server_metadata.h"
#include "mongo/rpc/metadata/metadata_hook.h"
#include "mongo/s/catalog/sharding_catalog_client.h"
#include "mongo/s/catalog/type_chunk.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/client/sharding_network_connection_hook.h"
#include "mongo/s/grid.h"
#include "mongo/s/sharding_initialization.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace {

const auto getShardingState = ServiceContext::declareDecoration<ShardingState>();

/**
 * Updates the config server field of the shardIdentity document with the given connection string
 * if setName is equal to the config server replica set name.
 *
 * Note: This is intended to be used on a new thread that hasn't called Client::initThread.
 * One example use case is for the ReplicaSetMonitor asynchronous callback when it detects changes
 * to replica set membership.
 */
void updateShardIdentityConfigStringCB(const std::string& setName,
                                       const std::string& newConnectionString) {
    auto configsvrConnStr = grid.shardRegistry()->getConfigServerConnectionString();
    if (configsvrConnStr.getSetName() != setName) {
        // Ignore all change notification for other sets that are not the config server.
        return;
    }

    Client::initThread("updateShardIdentityConfigConnString");
    auto uniqOpCtx = Client::getCurrent()->makeOperationContext();

    auto status = ShardingState::get(uniqOpCtx.get())
                      ->updateShardIdentityConfigString(uniqOpCtx.get(), newConnectionString);
    if (!status.isOK() && !ErrorCodes::isNotMasterError(status.code())) {
        warning() << "error encountered while trying to update config connection string to "
                  << newConnectionString << causedBy(redact(status));
    }
}

}  // namespace

ShardingState::ShardingState()
    : _initializationState(static_cast<uint32_t>(InitializationState::kNew)),
      _initializationStatus(Status(ErrorCodes::InternalError, "Uninitialized value")),
      _globalInit(&initializeGlobalShardingStateForMongod) {}

ShardingState::~ShardingState() = default;

ShardingState* ShardingState::get(ServiceContext* serviceContext) {
    return &getShardingState(serviceContext);
}

ShardingState* ShardingState::get(OperationContext* operationContext) {
    return ShardingState::get(operationContext->getServiceContext());
}

bool ShardingState::enabled() const {
    return _getInitializationState() == InitializationState::kInitialized;
}

void ShardingState::setEnabledForTest(const std::string& shardName) {
    _setInitializationState(InitializationState::kInitialized);
    _shardName = shardName;
}

Status ShardingState::canAcceptShardedCommands() const {
    if (serverGlobalParams.clusterRole != ClusterRole::ShardServer) {
        return {ErrorCodes::NoShardingEnabled,
                "Cannot accept sharding commands if not started with --shardsvr"};
    } else if (!enabled()) {
        return {ErrorCodes::ShardingStateNotInitialized,
                "Cannot accept sharding commands if sharding state has not "
                "been initialized with a shardIdentity document"};
    } else {
        return Status::OK();
    }
}

std::string ShardingState::getShardName() {
    invariant(enabled());
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    return _shardName;
}

void ShardingState::shutDown(OperationContext* opCtx) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    if (enabled()) {
        Grid::get(opCtx)->getExecutorPool()->shutdownAndJoin();
        Grid::get(opCtx)->catalogClient()->shutDown(opCtx);
    }
}

Status ShardingState::updateConfigServerOpTimeFromMetadata(OperationContext* opCtx) {
    if (!enabled()) {
        // Nothing to do if sharding state has not been initialized.
        return Status::OK();
    }

    boost::optional<repl::OpTime> opTime = rpc::ConfigServerMetadata::get(opCtx).getOpTime();
    if (opTime) {
        if (!AuthorizationSession::get(opCtx->getClient())
                 ->isAuthorizedForActionsOnResource(ResourcePattern::forClusterResource(),
                                                    ActionType::internal)) {
            return Status(ErrorCodes::Unauthorized, "Unauthorized to update config opTime");
        }

        Grid::get(opCtx)->advanceConfigOpTime(*opTime);
    }

    return Status::OK();
}

void ShardingState::setGlobalInitMethodForTest(GlobalInitFunc func) {
    _globalInit = func;
}

// NOTE: This method will be called inside a database lock so it should never take any database
// locks, perform I/O, or any long running operations.
Status ShardingState::initializeFromShardIdentity(OperationContext* opCtx,
                                                  const ShardIdentityType& shardIdentity) {
    invariant(serverGlobalParams.clusterRole == ClusterRole::ShardServer);
    invariant(opCtx->lockState()->isLocked());

    Status validationStatus = shardIdentity.validate();
    if (!validationStatus.isOK()) {
        return validationStatus.withContext(
            "Invalid shard identity document found when initializing sharding state");
    }

    log() << "initializing sharding state with: " << shardIdentity;

    stdx::unique_lock<stdx::mutex> lk(_mutex);

    auto configSvrConnStr = shardIdentity.getConfigsvrConnString();

    if (enabled()) {
        invariant(!_shardName.empty());
        fassert(40372, _shardName == shardIdentity.getShardName());

        auto prevConfigsvrConnStr =
            Grid::get(opCtx)->shardRegistry()->getConfigServerConnectionString();
        invariant(prevConfigsvrConnStr.type() == ConnectionString::SET);
        fassert(40373, prevConfigsvrConnStr.getSetName() == configSvrConnStr.getSetName());

        invariant(_clusterId.isSet());
        fassert(40374, _clusterId == shardIdentity.getClusterId());

        return Status::OK();
    }

    if (_getInitializationState() == InitializationState::kError) {
        return {ErrorCodes::ManualInterventionRequired,
                str::stream() << "Server's sharding metadata manager failed to initialize and will "
                                 "remain in this state until the instance is manually reset"
                              << causedBy(_initializationStatus)};
    }

    ShardedConnectionInfo::addHook(opCtx->getServiceContext());

    try {
        Status status = _globalInit(opCtx, configSvrConnStr, generateDistLockProcessId(opCtx));
        if (status.isOK()) {
            ReplicaSetMonitor::setSynchronousConfigChangeHook(
                &ShardRegistry::replicaSetChangeShardRegistryUpdateHook);
            ReplicaSetMonitor::setAsynchronousConfigChangeHook(&updateShardIdentityConfigStringCB);

            // Determine primary/secondary/standalone state in order to properly initialize sharding
            // components.
            auto replCoord = repl::ReplicationCoordinator::get(opCtx);
            bool isReplSet =
                replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet;
            bool isStandaloneOrPrimary =
                !isReplSet || (repl::ReplicationCoordinator::get(opCtx)->getMemberState() ==
                               repl::MemberState::RS_PRIMARY);

            CatalogCacheLoader::get(opCtx).initializeReplicaSetRole(isStandaloneOrPrimary);
            ChunkSplitter::get(opCtx).setReplicaSetMode(isStandaloneOrPrimary);

            log() << "initialized sharding components for "
                  << (isStandaloneOrPrimary ? "primary" : "secondary") << " node.";
            _setInitializationState(InitializationState::kInitialized);
        } else {
            log() << "failed to initialize sharding components" << causedBy(status);
            _initializationStatus = status;
            _setInitializationState(InitializationState::kError);
        }
        _shardName = shardIdentity.getShardName();
        _clusterId = shardIdentity.getClusterId();

        return status;
    } catch (const DBException& ex) {
        auto errorStatus = ex.toStatus();
        _initializationStatus = errorStatus;
        _setInitializationState(InitializationState::kError);
        return errorStatus;
    }

    MONGO_UNREACHABLE;
}

ShardingState::InitializationState ShardingState::_getInitializationState() const {
    return static_cast<InitializationState>(_initializationState.load());
}

void ShardingState::_setInitializationState(InitializationState newState) {
    _initializationState.store(static_cast<uint32_t>(newState));
}

StatusWith<bool> ShardingState::initializeShardingAwarenessIfNeeded(OperationContext* opCtx) {
    invariant(!opCtx->lockState()->isLocked());

    // In sharded readOnly mode, we ignore the shardIdentity document on disk and instead *require*
    // a shardIdentity document to be passed through --overrideShardIdentity.
    if (storageGlobalParams.readOnly) {
        if (serverGlobalParams.clusterRole == ClusterRole::ShardServer) {
            if (serverGlobalParams.overrideShardIdentity.isEmpty()) {
                return {ErrorCodes::InvalidOptions,
                        "If started with --shardsvr in queryableBackupMode, a shardIdentity "
                        "document must be provided through --overrideShardIdentity"};
            }
            auto swOverrideShardIdentity =
                ShardIdentityType::fromBSON(serverGlobalParams.overrideShardIdentity);
            if (!swOverrideShardIdentity.isOK()) {
                return swOverrideShardIdentity.getStatus();
            }
            {
                // Global lock is required to call initializeFromShardIdenetity().
                Lock::GlobalWrite lk(opCtx);
                auto status =
                    initializeFromShardIdentity(opCtx, swOverrideShardIdentity.getValue());
                if (!status.isOK()) {
                    return status;
                }
            }
            return true;
        } else {
            // Error if --overrideShardIdentity is used but *not* started with --shardsvr.
            if (!serverGlobalParams.overrideShardIdentity.isEmpty()) {
                return {
                    ErrorCodes::InvalidOptions,
                    str::stream()
                        << "Not started with --shardsvr, but a shardIdentity document was provided "
                           "through --overrideShardIdentity: "
                        << serverGlobalParams.overrideShardIdentity};
            }
            return false;
        }
    }
    // In sharded *non*-readOnly mode, error if --overrideShardIdentity is provided. Use the
    // shardIdentity document on disk if one exists, but it is okay if no shardIdentity document is
    // provided at all (sharding awareness will be initialized when a shardIdentity document is
    // inserted).
    else {
        if (!serverGlobalParams.overrideShardIdentity.isEmpty()) {
            return {
                ErrorCodes::InvalidOptions,
                str::stream() << "--overrideShardIdentity is only allowed in sharded "
                                 "queryableBackupMode. If not in queryableBackupMode, you can edit "
                                 "the shardIdentity document by starting the server *without* "
                                 "--shardsvr, manually updating the shardIdentity document in the "
                              << NamespaceString::kServerConfigurationNamespace.toString()
                              << " collection, and restarting the server with --shardsvr."};
        }

        // Load the shardIdentity document from disk.
        BSONObj shardIdentityBSON;
        bool foundShardIdentity = false;
        try {
            AutoGetCollection autoColl(
                opCtx, NamespaceString::kServerConfigurationNamespace, MODE_IS);
            foundShardIdentity = Helpers::findOne(opCtx,
                                                  autoColl.getCollection(),
                                                  BSON("_id" << ShardIdentityType::IdName),
                                                  shardIdentityBSON);
        } catch (const DBException& ex) {
            return ex.toStatus();
        }

        if (serverGlobalParams.clusterRole == ClusterRole::ShardServer) {
            if (!foundShardIdentity) {
                warning() << "Started with --shardsvr, but no shardIdentity document was found on "
                             "disk in "
                          << NamespaceString::kServerConfigurationNamespace
                          << ". This most likely means this server has not yet been added to a "
                             "sharded cluster.";
                return false;
            }

            invariant(!shardIdentityBSON.isEmpty());

            auto swShardIdentity = ShardIdentityType::fromBSON(shardIdentityBSON);
            if (!swShardIdentity.isOK()) {
                return swShardIdentity.getStatus();
            }
            {
                // Global lock is required to call initializeFromShardIdenetity().
                Lock::GlobalWrite lk(opCtx);
                auto status = initializeFromShardIdentity(opCtx, swShardIdentity.getValue());
                if (!status.isOK()) {
                    return status;
                }
            }
            return true;
        } else {
            // Warn if a shardIdentity document is found on disk but *not* started with --shardsvr.
            if (!shardIdentityBSON.isEmpty()) {
                warning() << "Not started with --shardsvr, but a shardIdentity document was found "
                             "on disk in "
                          << NamespaceString::kServerConfigurationNamespace << ": "
                          << shardIdentityBSON;
            }
            return false;
        }
    }
}

StatusWith<ScopedDonateChunk> ShardingState::registerDonateChunk(const MoveChunkRequest& args) {
    return _activeMigrationsRegistry.registerDonateChunk(args);
}

StatusWith<ScopedReceiveChunk> ShardingState::registerReceiveChunk(const NamespaceString& nss,
                                                                   const ChunkRange& chunkRange,
                                                                   const ShardId& fromShardId) {
    return _activeMigrationsRegistry.registerReceiveChunk(nss, chunkRange, fromShardId);
}

boost::optional<NamespaceString> ShardingState::getActiveDonateChunkNss() {
    return _activeMigrationsRegistry.getActiveDonateChunkNss();
}

BSONObj ShardingState::getActiveMigrationStatusReport(OperationContext* opCtx) {
    return _activeMigrationsRegistry.getActiveMigrationStatusReport(opCtx);
}

StatusWith<ScopedMovePrimary> ShardingState::registerMovePrimary(
    const ShardMovePrimary& requestArgs) {
    return _activeMovePrimariesRegistry.registerMovePrimary(requestArgs);
}

boost::optional<NamespaceString> ShardingState::getActiveMovePrimaryNss() {
    return _activeMovePrimariesRegistry.getActiveMovePrimaryNss();
}

void ShardingState::appendInfo(OperationContext* opCtx, BSONObjBuilder& builder) {
    const bool isEnabled = enabled();
    builder.appendBool("enabled", isEnabled);
    if (!isEnabled)
        return;

    stdx::lock_guard<stdx::mutex> lk(_mutex);

    builder.append("configServer",
                   Grid::get(opCtx)->shardRegistry()->getConfigServerConnectionString().toString());
    builder.append("shardName", _shardName);
    builder.append("clusterId", _clusterId);
}

bool ShardingState::needCollectionMetadata(OperationContext* opCtx, const std::string& ns) {
    if (!enabled())
        return false;

    Client* client = opCtx->getClient();

    // Shard version information received from mongos may either by attached to the Client or
    // directly to the OperationContext.
    return ShardedConnectionInfo::get(client, false) ||
        OperationShardingState::get(opCtx).hasShardVersion();
}

Status ShardingState::updateShardIdentityConfigString(OperationContext* opCtx,
                                                      const std::string& newConnectionString) {
    BSONObj updateObj(ShardIdentityType::createConfigServerUpdateObject(newConnectionString));

    UpdateRequest updateReq(NamespaceString::kServerConfigurationNamespace);
    updateReq.setQuery(BSON("_id" << ShardIdentityType::IdName));
    updateReq.setUpdates(updateObj);
    UpdateLifecycleImpl updateLifecycle(NamespaceString::kServerConfigurationNamespace);
    updateReq.setLifecycle(&updateLifecycle);

    try {
        AutoGetOrCreateDb autoDb(
            opCtx, NamespaceString::kServerConfigurationNamespace.db(), MODE_X);

        auto result = update(opCtx, autoDb.getDb(), updateReq);
        if (result.numMatched == 0) {
            warning() << "failed to update config string of shard identity document because "
                      << "it does not exist. This shard could have been removed from the cluster";
        } else {
            LOG(2) << "Updated config server connection string in shardIdentity document to"
                   << newConnectionString;
        }
    } catch (const DBException& exception) {
        return exception.toStatus();
    }

    return Status::OK();
}

}  // namespace mongo
