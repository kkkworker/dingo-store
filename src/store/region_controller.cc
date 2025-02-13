// Copyright (c) 2023 dingodb.com, Inc. All Rights Reserved
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

#include "store/region_controller.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "butil/status.h"
#include "common/constant.h"
#include "common/helper.h"
#include "common/logging.h"
#include "event/store_state_machine_event.h"
#include "fmt/core.h"
#include "glog/logging.h"
#include "proto/common.pb.h"
#include "proto/coordinator.pb.h"
#include "proto/error.pb.h"
#include "server/server.h"
#include "store/heartbeat.h"

namespace dingodb {

butil::Status CreateRegionTask::PreValidateCreateRegion(const pb::coordinator::RegionCmd& command) {
  auto store_meta_manager = Server::GetInstance()->GetStoreMetaManager();

  return ValidateCreateRegion(store_meta_manager, command.region_id());
}

butil::Status CreateRegionTask::ValidateCreateRegion(std::shared_ptr<StoreMetaManager> store_meta_manager,
                                                     uint64_t region_id) {
  auto region = store_meta_manager->GetStoreRegionMeta()->GetRegion(region_id);
  if (region != nullptr && region->State() != pb::common::StoreRegionState::NEW) {
    return butil::Status(pb::error::EREGION_EXIST, fmt::format("Region {} already exist", region_id));
  }

  return butil::Status();
}

butil::Status CreateRegionTask::CreateRegion(std::shared_ptr<Context> ctx, store::RegionPtr region,
                                             uint64_t split_from_region_id) {
  auto store_meta_manager = Server::GetInstance()->GetStoreMetaManager();
  DINGO_LOG(DEBUG) << fmt::format("Create region {}, {}", region->Id(), region->InnerRegion().ShortDebugString());

  // Valiate region
  auto status = ValidateCreateRegion(store_meta_manager, region->Id());
  if (!status.ok()) {
    return status;
  }

  // Add region to store region meta manager
  DINGO_LOG(DEBUG) << fmt::format("Create region {} save region meta", region->Id());
  auto store_region_meta = store_meta_manager->GetStoreRegionMeta();
  region->SetState(pb::common::StoreRegionState::NEW);
  store_region_meta->AddRegion(region);

  // Add region metrics
  DINGO_LOG(DEBUG) << fmt::format("Create region {} add region metrics", region->Id());
  auto region_metrics = StoreRegionMetrics::NewMetrics(region->Id());
  Server::GetInstance()->GetStoreMetricsManager()->GetStoreRegionMetrics()->AddMetrics(region_metrics);

  // Add raft node
  DINGO_LOG(DEBUG) << fmt::format("Create region {} add raft node", region->Id());
  auto engine = Server::GetInstance()->GetEngine();
  if (engine->GetID() == pb::common::ENG_RAFT_STORE) {
    auto raft_meta = StoreRaftMeta::NewRaftMeta(region->Id());
    Server::GetInstance()->GetStoreMetaManager()->GetStoreRaftMeta()->AddRaftMeta(raft_meta);

    auto listener_factory = std::make_shared<StoreSmEventListenerFactory>();

    auto raft_kv_engine = std::dynamic_pointer_cast<RaftKvEngine>(engine);
    status = raft_kv_engine->AddNode(ctx, region, raft_meta, region_metrics, listener_factory->Build(), false);
    if (!status.ok()) {
      return status;
    }
  }

  DINGO_LOG(DEBUG) << fmt::format("Create region {} update region state NORMAL", region->Id());
  if (split_from_region_id == 0) {
    store_region_meta->UpdateState(region, pb::common::StoreRegionState::NORMAL);
  } else {
    store_region_meta->UpdateState(region, pb::common::StoreRegionState::STANDBY);
  }

  return butil::Status();
}

void CreateRegionTask::Run() {
  auto region = store::Region::New(region_cmd_->create_request().region_definition());

  auto status = CreateRegion(ctx_, region, region_cmd_->create_request().split_from_region_id());
  if (!status.ok()) {
    DINGO_LOG(DEBUG) << fmt::format("Create region {} failed, {}", region->Id(), status.error_str());
  }

  Server::GetInstance()->GetRegionCommandManager()->UpdateCommandStatus(
      region_cmd_,
      status.ok() ? pb::coordinator::RegionCmdStatus::STATUS_DONE : pb::coordinator::RegionCmdStatus::STATUS_FAIL);

  // Notify coordinator
  if (region_cmd_->is_notify()) {
    Heartbeat::TriggerStoreHeartbeat(region_cmd_->region_id());
  }
}

butil::Status DeleteRegionTask::PreValidateDeleteRegion(const pb::coordinator::RegionCmd& command) {
  auto store_meta_manager = Server::GetInstance()->GetStoreMetaManager();
  return ValidateDeleteRegion(store_meta_manager,
                              store_meta_manager->GetStoreRegionMeta()->GetRegion(command.region_id()));
}

butil::Status DeleteRegionTask::ValidateDeleteRegion(std::shared_ptr<StoreMetaManager> /*store_meta_manager*/,
                                                     store::RegionPtr region) {
  if (region == nullptr) {
    return butil::Status(pb::error::EREGION_NOT_FOUND, "Region is not exist, can't delete.");
  }
  if (region->State() == pb::common::StoreRegionState::DELETING ||
      region->State() == pb::common::StoreRegionState::DELETED) {
    return butil::Status(pb::error::EREGION_DELETING, "Region is deleting or deleted.");
  }

  if (region->State() == pb::common::StoreRegionState::SPLITTING ||
      region->State() == pb::common::StoreRegionState::MERGING) {
    return butil::Status(pb::error::EREGION_STATE, "Region state not allow delete.");
  }

  return butil::Status();
}

butil::Status DeleteRegionTask::DeleteRegion(std::shared_ptr<Context> ctx, uint64_t region_id) {
  auto store_meta_manager = Server::GetInstance()->GetStoreMetaManager();
  auto store_region_meta = store_meta_manager->GetStoreRegionMeta();
  auto region = store_region_meta->GetRegion(region_id);

  DINGO_LOG(DEBUG) << fmt::format("Delete region {}", region_id);
  // Valiate region
  auto status = ValidateDeleteRegion(store_meta_manager, region);
  if (!status.ok()) {
    return status;
  }

  // Update state
  DINGO_LOG(DEBUG) << fmt::format("Delete region {} update region state DELETING", region_id);
  store_region_meta->UpdateState(region, pb::common::StoreRegionState::DELETING);

  // Shutdown raft node
  auto engine = Server::GetInstance()->GetEngine();

  // Delete data
  DINGO_LOG(DEBUG) << fmt::format("Delete region {} delete data", region_id);
  auto writer = engine->GetRawEngine()->NewWriter(Constant::kStoreDataCF);
  writer->KvDeleteRange(region->Range());

  // Raft kv engine
  if (engine->GetID() == pb::common::ENG_RAFT_STORE) {
    auto raft_kv_engine = std::dynamic_pointer_cast<RaftKvEngine>(engine);
    // Delete raft
    DINGO_LOG(DEBUG) << fmt::format("Delete region {} delete raft node", region_id);
    raft_kv_engine->DestroyNode(ctx, region_id);
  }

  // Update state
  DINGO_LOG(DEBUG) << fmt::format("Delete region {} update region state DELETED", region_id);
  store_region_meta->UpdateState(region, pb::common::StoreRegionState::DELETED);

  // Delete metrics
  DINGO_LOG(DEBUG) << fmt::format("Delete region {} delete region metrics", region_id);
  Server::GetInstance()->GetStoreMetricsManager()->GetStoreRegionMetrics()->DeleteMetrics(region_id);

  // Delete raft meta
  store_meta_manager->GetStoreRaftMeta()->DeleteRaftMeta(region_id);

  // Delete region executor
  auto region_controller = Server::GetInstance()->GetRegionController();

  auto command = std::make_shared<pb::coordinator::RegionCmd>();
  command->set_id(Helper::TimestampNs());
  command->set_region_id(region_id);
  command->set_create_timestamp(Helper::TimestampMs());
  command->set_region_cmd_type(pb::coordinator::CMD_DESTROY_EXECUTOR);
  command->mutable_destroy_executor_request()->set_region_id(region_id);

  status = region_controller->DispatchRegionControlCommand(std::make_shared<Context>(), command);
  if (!status.ok()) {
    DINGO_LOG(ERROR) << "Dispatch destroy region executor command failed, region: " << region_id
                     << " error: " << status.error_code() << " " << status.error_str();
  }

  // purge region for coordinator recycle_orphan_region mechanism
  // TODO: need to implement a better mechanism of tombstone for region's meta info
  DINGO_LOG(DEBUG) << fmt::format("Purge region {}", region_id);
  store_region_meta->DeleteRegion(region_id);

  return butil::Status();
}

void DeleteRegionTask::Run() {
  auto status = DeleteRegion(ctx_, region_cmd_->delete_request().region_id());
  if (!status.ok()) {
    DINGO_LOG(DEBUG) << fmt::format("Delete region {} failed, {}", region_cmd_->delete_request().region_id(),
                                    status.error_str());
  }

  Server::GetInstance()->GetRegionCommandManager()->UpdateCommandStatus(
      region_cmd_,
      status.ok() ? pb::coordinator::RegionCmdStatus::STATUS_DONE : pb::coordinator::RegionCmdStatus::STATUS_FAIL);

  // Notify coordinator
  if (region_cmd_->is_notify()) {
    Heartbeat::TriggerStoreHeartbeat(region_cmd_->region_id());
  }
}

butil::Status SplitRegionTask::PreValidateSplitRegion(const pb::coordinator::RegionCmd& command) {
  auto store_meta_manager = Server::GetInstance()->GetStoreMetaManager();

  return ValidateSplitRegion(store_meta_manager->GetStoreRegionMeta(), command.split_request());
}

butil::Status SplitRegionTask::ValidateSplitRegion(std::shared_ptr<StoreRegionMeta> store_region_meta,
                                                   const pb::coordinator::SplitRequest& split_request) {
  auto parent_region_id = split_request.split_from_region_id();
  auto child_region_id = split_request.split_to_region_id();

  auto parent_region = store_region_meta->GetRegion(parent_region_id);
  if (parent_region == nullptr) {
    return butil::Status(pb::error::EREGION_NOT_FOUND, "Parent region not exist.");
  }
  auto child_region = store_region_meta->GetRegion(child_region_id);
  if (child_region == nullptr) {
    return butil::Status(pb::error::EREGION_NOT_FOUND, "Child region not exist.");
  }

  const auto& split_key = split_request.split_watershed_key();
  auto range = parent_region->Range();
  if (range.start_key().compare(split_key) >= 0 || range.end_key().compare(split_key) <= 0) {
    return butil::Status(pb::error::EKEY_INVALID, "Split key is invalid.");
  }

  if (parent_region->State() == pb::common::StoreRegionState::SPLITTING) {
    return butil::Status(pb::error::EREGION_SPLITING, "Parent region state is splitting.");
  }

  if (parent_region->State() == pb::common::StoreRegionState::NEW ||
      parent_region->State() == pb::common::StoreRegionState::MERGING ||
      parent_region->State() == pb::common::StoreRegionState::DELETING ||
      parent_region->State() == pb::common::StoreRegionState::DELETED) {
    return butil::Status(pb::error::EREGION_STATE, "Parent region state not allow split.");
  }

  auto engine = Server::GetInstance()->GetEngine();
  if (engine->GetID() == pb::common::ENG_RAFT_STORE) {
    auto raft_kv_engine = std::dynamic_pointer_cast<RaftKvEngine>(engine);
    auto node = raft_kv_engine->GetNode(parent_region_id);
    if (node == nullptr) {
      return butil::Status(pb::error::ERAFT_NOT_FOUND, "No found raft node.");
    }

    if (!node->IsLeader()) {
      return butil::Status(pb::error::ERAFT_NOTLEADER, node->GetLeaderId().to_string());
    }
  }

  return butil::Status();
}

butil::Status SplitRegionTask::SplitRegion() {
  auto store_region_meta = Server::GetInstance()->GetStoreMetaManager()->GetStoreRegionMeta();

  auto status = ValidateSplitRegion(store_region_meta, region_cmd_->split_request());
  if (!status.ok()) {
    return status;
  }

  // Commit raft log
  WriteData write_data;
  auto datum = std::make_shared<SplitDatum>();
  datum->from_region_id = region_cmd_->split_request().split_from_region_id();
  datum->to_region_id = region_cmd_->split_request().split_to_region_id();
  datum->split_key = region_cmd_->split_request().split_watershed_key();

  write_data.AddDatums(std::static_pointer_cast<DatumAble>(datum));

  ctx_->SetRegionId(datum->from_region_id);
  return Server::GetInstance()->GetEngine()->AsyncWrite(ctx_, write_data,
                                                        [](std::shared_ptr<Context>, butil::Status status) {
                                                          if (!status.ok()) {
                                                            LOG(ERROR) << "Write split failed, " << status.error_str();
                                                          }
                                                        });
}

void SplitRegionTask::Run() {
  auto status = SplitRegion();
  if (!status.ok()) {
    DINGO_LOG(DEBUG) << fmt::format("Split region {} failed, {}", region_cmd_->split_request().split_from_region_id(),
                                    status.error_str());
  }

  Server::GetInstance()->GetRegionCommandManager()->UpdateCommandStatus(
      region_cmd_,
      status.ok() ? pb::coordinator::RegionCmdStatus::STATUS_DONE : pb::coordinator::RegionCmdStatus::STATUS_FAIL);

  // Notify coordinator
  if (region_cmd_->is_notify()) {
    Heartbeat::TriggerStoreHeartbeat(region_cmd_->region_id());
  }
}

butil::Status ChangeRegionTask::PreValidateChangeRegion(const pb::coordinator::RegionCmd& command) {
  auto store_meta_manager = Server::GetInstance()->GetStoreMetaManager();

  return ValidateChangeRegion(store_meta_manager, command.change_peer_request().region_definition());
}

butil::Status ChangeRegionTask::ValidateChangeRegion(std::shared_ptr<StoreMetaManager> store_meta_manager,
                                                     const pb::common::RegionDefinition& region_definition) {
  auto region = store_meta_manager->GetStoreRegionMeta()->GetRegion(region_definition.id());
  if (region == nullptr) {
    return butil::Status(pb::error::EREGION_NOT_FOUND, "Region not exist, can't change.");
  }

  if (region->State() != pb::common::StoreRegionState::NORMAL) {
    return butil::Status(pb::error::EREGION_STATE, "Region state not allow change.");
  }

  auto engine = Server::GetInstance()->GetEngine();
  if (engine->GetID() == pb::common::ENG_RAFT_STORE) {
    auto raft_kv_engine = std::dynamic_pointer_cast<RaftKvEngine>(engine);
    auto node = raft_kv_engine->GetNode(region_definition.id());
    if (node == nullptr) {
      return butil::Status(pb::error::ERAFT_NOT_FOUND, "No found raft node.");
    }

    if (!node->IsLeader()) {
      return butil::Status(pb::error::ERAFT_NOTLEADER, node->GetLeaderId().to_string());
    }
  }

  return butil::Status();
}

butil::Status ChangeRegionTask::ChangeRegion(std::shared_ptr<Context> ctx,
                                             const pb::common::RegionDefinition& region_definition) {
  auto store_meta_manager = Server::GetInstance()->GetStoreMetaManager();
  DINGO_LOG(DEBUG) << fmt::format("Change region {}, {}", region_definition.id(), region_definition.ShortDebugString());

  // Valiate region
  auto status = ValidateChangeRegion(store_meta_manager, region_definition);
  if (!status.ok()) {
    return status;
  }

  auto filter_peers_by_role = [region_definition](pb::common::PeerRole role) -> std::vector<pb::common::Peer> {
    std::vector<pb::common::Peer> peers;
    for (const auto& peer : region_definition.peers()) {
      if (peer.role() == role) {
        peers.push_back(peer);
      }
    }
    return peers;
  };

  auto engine = Server::GetInstance()->GetEngine();
  if (engine->GetID() == pb::common::ENG_RAFT_STORE) {
    auto raft_kv_engine = std::dynamic_pointer_cast<RaftKvEngine>(engine);
    return raft_kv_engine->ChangeNode(ctx, region_definition.id(), filter_peers_by_role(pb::common::VOTER));
  }

  return butil::Status();
}

void ChangeRegionTask::Run() {
  auto status = ChangeRegion(ctx_, region_cmd_->change_peer_request().region_definition());
  if (!status.ok()) {
    DINGO_LOG(DEBUG) << fmt::format("Change region {} failed, {}",
                                    region_cmd_->change_peer_request().region_definition().id(), status.error_str());
  }

  Server::GetInstance()->GetRegionCommandManager()->UpdateCommandStatus(
      region_cmd_,
      status.ok() ? pb::coordinator::RegionCmdStatus::STATUS_DONE : pb::coordinator::RegionCmdStatus::STATUS_FAIL);

  // Notify coordinator
  if (region_cmd_->is_notify()) {
    Heartbeat::TriggerStoreHeartbeat(region_cmd_->region_id());
  }
}

butil::Status TransferLeaderTask::PreValidateTransferLeader(const pb::coordinator::RegionCmd& command) {
  auto store_meta_manager = Server::GetInstance()->GetStoreMetaManager();

  return ValidateTransferLeader(store_meta_manager, command.region_id(), command.transfer_leader_request().peer());
}

butil::Status TransferLeaderTask::ValidateTransferLeader(std::shared_ptr<StoreMetaManager> store_meta_manager,
                                                         uint64_t region_id, const pb::common::Peer& peer) {
  auto region = store_meta_manager->GetStoreRegionMeta()->GetRegion(region_id);
  if (region == nullptr) {
    return butil::Status(pb::error::EREGION_NOT_FOUND, "Region not exist, can't transfer leader.");
  }

  if (region->State() != pb::common::StoreRegionState::NORMAL) {
    return butil::Status(pb::error::EREGION_STATE, "Region state not allow transfer leader.");
  }

  if (peer.store_id() == Server::GetInstance()->Id()) {
    return butil::Status(pb::error::ERAFT_TRANSFER_LEADER, "The peer is already leader, not need transfer.");
  }

  if (peer.raft_location().host().empty() || peer.raft_location().host() == "0.0.0.0") {
    return butil::Status(pb::error::EILLEGAL_PARAMTETERS, "Raft location is invalid.");
  }

  return butil::Status();
}

butil::Status TransferLeaderTask::TransferLeader(std::shared_ptr<Context>, uint64_t region_id,
                                                 const pb::common::Peer& peer) {
  auto store_meta_manager = Server::GetInstance()->GetStoreMetaManager();
  DINGO_LOG(DEBUG) << fmt::format("Transfer leader {}, {}", region_id, peer.ShortDebugString());

  auto status = ValidateTransferLeader(store_meta_manager, region_id, peer);
  if (!status.ok()) {
    return status;
  }

  auto engine = Server::GetInstance()->GetEngine();
  if (engine->GetID() == pb::common::ENG_RAFT_STORE) {
    auto raft_kv_engine = std::dynamic_pointer_cast<RaftKvEngine>(engine);
    return raft_kv_engine->TransferLeader(region_id, peer);
  }

  return butil::Status();
}

void TransferLeaderTask::Run() {
  auto status = TransferLeader(ctx_, region_cmd_->region_id(), region_cmd_->transfer_leader_request().peer());
  if (!status.ok()) {
    DINGO_LOG(DEBUG) << fmt::format("Transfer leader {} failed, {}",
                                    region_cmd_->change_peer_request().region_definition().id(), status.error_cstr());
  }

  Server::GetInstance()->GetRegionCommandManager()->UpdateCommandStatus(
      region_cmd_,
      status.ok() ? pb::coordinator::RegionCmdStatus::STATUS_DONE : pb::coordinator::RegionCmdStatus::STATUS_FAIL);

  // Notify coordinator
  if (region_cmd_->is_notify()) {
    Heartbeat::TriggerStoreHeartbeat(region_cmd_->region_id());
  }
}

butil::Status SnapshotRegionTask::Snapshot(std::shared_ptr<Context> ctx, uint64_t region_id) {
  auto engine = Server::GetInstance()->GetEngine();
  return engine->DoSnapshot(ctx, region_id);
}

void SnapshotRegionTask::Run() {
  auto status = Snapshot(ctx_, region_cmd_->region_id());
  if (!status.ok()) {
    DINGO_LOG(DEBUG) << fmt::format("Snapshot region {} failed, {}", region_cmd_->region_id(), status.error_str());
  }

  Server::GetInstance()->GetRegionCommandManager()->UpdateCommandStatus(
      region_cmd_,
      status.ok() ? pb::coordinator::RegionCmdStatus::STATUS_DONE : pb::coordinator::RegionCmdStatus::STATUS_FAIL);
}

butil::Status PurgeRegionTask::PreValidatePurgeRegion(const pb::coordinator::RegionCmd& command) {
  auto store_meta_manager = Server::GetInstance()->GetStoreMetaManager();

  return ValidatePurgeRegion(store_meta_manager->GetStoreRegionMeta()->GetRegion(command.region_id()));
}

butil::Status PurgeRegionTask::ValidatePurgeRegion(store::RegionPtr region) {
  if (region == nullptr) {
    return butil::Status(pb::error::EREGION_NOT_FOUND, "Region is not exist, can't purge.");
  }
  if (region->State() != pb::common::StoreRegionState::DELETED) {
    return butil::Status(pb::error::EREGION_DELETED, "Region is not deleted, can't purge.");
  }

  return butil::Status();
}

butil::Status PurgeRegionTask::PurgeRegion(std::shared_ptr<Context>, uint64_t region_id) {
  DINGO_LOG(DEBUG) << fmt::format("Purge region {}", region_id);
  auto store_meta_manager = Server::GetInstance()->GetStoreMetaManager();
  auto store_region_meta = store_meta_manager->GetStoreRegionMeta();
  store_region_meta->DeleteRegion(region_id);

  return butil::Status();
}

void PurgeRegionTask::Run() {
  auto status = PurgeRegion(ctx_, region_cmd_->purge_request().region_id());
  if (!status.ok()) {
    DINGO_LOG(DEBUG) << fmt::format("Purge region {} failed, %s", region_cmd_->purge_request().region_id(),
                                    status.error_str());
  }

  Server::GetInstance()->GetRegionCommandManager()->UpdateCommandStatus(
      region_cmd_,
      status.ok() ? pb::coordinator::RegionCmdStatus::STATUS_DONE : pb::coordinator::RegionCmdStatus::STATUS_FAIL);

  // Notify coordinator
  if (region_cmd_->is_notify()) {
    Heartbeat::TriggerStoreHeartbeat(region_cmd_->region_id());
  }
}

butil::Status StopRegionTask::PreValidateStopRegion(const pb::coordinator::RegionCmd& command) {
  auto store_meta_manager = Server::GetInstance()->GetStoreMetaManager();

  return ValidateStopRegion(store_meta_manager->GetStoreRegionMeta()->GetRegion(command.region_id()));
}

butil::Status StopRegionTask::ValidateStopRegion(store::RegionPtr region) {
  if (region == nullptr) {
    return butil::Status(pb::error::EREGION_NOT_FOUND, "Region is not exist, can't delete peer.");
  }
  if (region->State() != pb::common::StoreRegionState::ORPHAN) {
    return butil::Status(pb::error::EREGION_STATE, "Region is not orphan.");
  }

  return butil::Status();
}

butil::Status StopRegionTask::StopRegion(std::shared_ptr<Context> ctx, uint64_t region_id) {
  auto store_meta_manager = Server::GetInstance()->GetStoreMetaManager();
  auto store_region_meta = store_meta_manager->GetStoreRegionMeta();
  auto region = store_region_meta->GetRegion(region_id);

  DINGO_LOG(DEBUG) << fmt::format("Stop region {}", region_id);
  // Valiate region
  auto status = ValidateStopRegion(region);
  if (!status.ok()) {
    return status;
  }

  // Shutdown raft node
  auto engine = Server::GetInstance()->GetEngine();
  if (engine->GetID() == pb::common::ENG_RAFT_STORE) {
    auto raft_kv_engine = std::dynamic_pointer_cast<RaftKvEngine>(engine);
    // Delete raft
    DINGO_LOG(DEBUG) << fmt::format("Delete peer {} delete raft node", region_id);
    raft_kv_engine->StopNode(ctx, region_id);
  }

  return butil::Status();
}

void StopRegionTask::Run() {
  auto status = StopRegion(ctx_, region_cmd_->stop_request().region_id());
  if (!status.ok()) {
    DINGO_LOG(DEBUG) << fmt::format("Delete peer region {} failed, {}", region_cmd_->stop_request().region_id(),
                                    status.error_str());
  }

  Server::GetInstance()->GetRegionCommandManager()->UpdateCommandStatus(
      region_cmd_,
      status.ok() ? pb::coordinator::RegionCmdStatus::STATUS_DONE : pb::coordinator::RegionCmdStatus::STATUS_FAIL);
}

butil::Status DestroyRegionExecutorTask::DestroyRegionExecutor(std::shared_ptr<Context>, uint64_t region_id) {
  auto regoin_controller = Server::GetInstance()->GetRegionController();
  if (regoin_controller == nullptr) {
    DINGO_LOG(ERROR) << "Region controller is nullptr";
    return butil::Status(pb::error::EINTERNAL, "Region controller is nullptr");
  }

  regoin_controller->UnRegisterExecutor(region_id);

  return butil::Status();
}

void DestroyRegionExecutorTask::Run() {
  auto status = DestroyRegionExecutor(ctx_, region_cmd_->destroy_executor_request().region_id());
  if (!status.ok()) {
    DINGO_LOG(DEBUG) << fmt::format("Destroy executor region {} failed, {}",
                                    region_cmd_->destroy_executor_request().region_id(), status.error_str());
  }

  Server::GetInstance()->GetRegionCommandManager()->UpdateCommandStatus(
      region_cmd_,
      status.ok() ? pb::coordinator::RegionCmdStatus::STATUS_DONE : pb::coordinator::RegionCmdStatus::STATUS_FAIL);
}

static int ExecuteRoutine(void*, bthread::TaskIterator<TaskRunnable*>& iter) {
  if (iter.is_queue_stopped()) {
    return 0;
  }

  {
    std::unique_ptr<TaskRunnable> self_guard(*iter);
    for (; iter; ++iter) {
      (*iter)->Run();
    }
  }

  return 0;
}

bool ControlExecutor::Init() {
  bthread::ExecutionQueueOptions options;
  options.bthread_attr = BTHREAD_ATTR_NORMAL;

  if (bthread::execution_queue_start(&queue_id_, &options, ExecuteRoutine, nullptr) != 0) {
    DINGO_LOG(ERROR) << "Start execution queue failed";
    return false;
  }

  is_available_.store(true, std::memory_order_relaxed);

  return true;
}

bool ControlExecutor::Execute(TaskRunnable* task) {
  if (!is_available_.load(std::memory_order_relaxed)) {
    DINGO_LOG(ERROR) << "Control execute queue is not available.";
    return false;
  }

  if (bthread::execution_queue_execute(queue_id_, task) != 0) {
    DINGO_LOG(ERROR) << "region execution queue execute failed";
    return false;
  }
  return true;
}

void ControlExecutor::Stop() {
  is_available_.store(false, std::memory_order_relaxed);

  if (bthread::execution_queue_stop(queue_id_) != 0) {
    DINGO_LOG(ERROR) << "region execution queue stop failed";
    return;
  }

  if (bthread::execution_queue_join(queue_id_) != 0) {
    DINGO_LOG(ERROR) << "region execution queue join failed";
  }
}

bool RegionCommandManager::Init() {
  std::vector<pb::common::KeyValue> kvs;
  if (!meta_reader_->Scan(Prefix(), kvs)) {
    DINGO_LOG(ERROR) << "Scan store raft meta failed!";
    return false;
  }

  if (!kvs.empty()) {
    TransformFromKv(kvs);
  }
  return true;
}

bool RegionCommandManager::IsExist(uint64_t command_id) {
  BAIDU_SCOPED_LOCK(mutex_);
  return region_commands_.find(command_id) != region_commands_.end();
}

void RegionCommandManager::AddCommand(std::shared_ptr<pb::coordinator::RegionCmd> region_cmd) {
  {
    BAIDU_SCOPED_LOCK(mutex_);
    if (region_commands_.find(region_cmd->id()) != region_commands_.end()) {
      DINGO_LOG(WARNING) << fmt::format("Region control command {} already exist!", region_cmd->id());
      return;
    }

    region_commands_.insert(std::make_pair(region_cmd->id(), region_cmd));
  }

  meta_writer_->Put(TransformToKv(&region_cmd));
}

void RegionCommandManager::UpdateCommandStatus(std::shared_ptr<pb::coordinator::RegionCmd> region_cmd,
                                               pb::coordinator::RegionCmdStatus status) {
  region_cmd->set_status(status);
  meta_writer_->Put(TransformToKv(&region_cmd));
}

void RegionCommandManager::UpdateCommandStatus(uint64_t command_id, pb::coordinator::RegionCmdStatus status) {
  auto region_cmd = GetCommand(command_id);
  if (region_cmd != nullptr) {
    UpdateCommandStatus(region_cmd, status);
  }
}

std::shared_ptr<pb::coordinator::RegionCmd> RegionCommandManager::GetCommand(uint64_t command_id) {
  BAIDU_SCOPED_LOCK(mutex_);
  auto it = region_commands_.find(command_id);
  if (it == region_commands_.end()) {
    return nullptr;
  }

  return it->second;
}

std::vector<std::shared_ptr<pb::coordinator::RegionCmd>> RegionCommandManager::GetCommands(
    pb::coordinator::RegionCmdStatus status) {
  BAIDU_SCOPED_LOCK(mutex_);
  std::vector<std::shared_ptr<pb::coordinator::RegionCmd>> commands;
  for (auto& [_, command] : region_commands_) {
    if (command->status() == status) {
      commands.push_back(command);
    }
  }

  std::sort(commands.begin(), commands.end(),
            [](const std::shared_ptr<pb::coordinator::RegionCmd>& a,
               const std::shared_ptr<pb::coordinator::RegionCmd>& b) { return a->id() < b->id(); });

  return commands;
}

std::vector<std::shared_ptr<pb::coordinator::RegionCmd>> RegionCommandManager::GetCommands(uint64_t region_id) {
  BAIDU_SCOPED_LOCK(mutex_);
  std::vector<std::shared_ptr<pb::coordinator::RegionCmd>> commands;
  for (auto& [_, command] : region_commands_) {
    if (command->region_id() == region_id) {
      commands.push_back(command);
    }
  }

  std::sort(commands.begin(), commands.end(),
            [](const std::shared_ptr<pb::coordinator::RegionCmd>& a,
               const std::shared_ptr<pb::coordinator::RegionCmd>& b) { return a->id() < b->id(); });

  return commands;
}

std::vector<std::shared_ptr<pb::coordinator::RegionCmd>> RegionCommandManager::GetAllCommand() {
  BAIDU_SCOPED_LOCK(mutex_);
  std::vector<std::shared_ptr<pb::coordinator::RegionCmd>> commands;
  commands.reserve(region_commands_.size());
  for (auto& [_, command] : region_commands_) {
    commands.push_back(command);
  }

  std::sort(commands.begin(), commands.end(),
            [](const std::shared_ptr<pb::coordinator::RegionCmd>& a,
               const std::shared_ptr<pb::coordinator::RegionCmd>& b) { return a->id() < b->id(); });

  return commands;
}

std::shared_ptr<pb::common::KeyValue> RegionCommandManager::TransformToKv(void* obj) {
  auto region_cmd = *static_cast<std::shared_ptr<pb::coordinator::RegionCmd>*>(obj);
  std::shared_ptr<pb::common::KeyValue> kv = std::make_shared<pb::common::KeyValue>();
  kv->set_key(GenKey(region_cmd->id()));
  kv->set_value(region_cmd->SerializeAsString());

  return kv;
}

void RegionCommandManager::TransformFromKv(const std::vector<pb::common::KeyValue>& kvs) {
  BAIDU_SCOPED_LOCK(mutex_);
  for (const auto& kv : kvs) {
    uint64_t command_id = ParseRegionId(kv.key());
    auto region_cmd = std::make_shared<pb::coordinator::RegionCmd>();
    region_cmd->ParsePartialFromArray(kv.value().data(), kv.value().size());
    region_commands_.insert_or_assign(command_id, region_cmd);
  }
}

bool RegionController::Init() {
  share_executor_ = std::make_shared<ControlExecutor>();
  if (!share_executor_->Init()) {
    DINGO_LOG(ERROR) << "Share executor init failed";
    return false;
  }

  auto store_meta_manager = Server::GetInstance()->GetStoreMetaManager();
  auto regions = store_meta_manager->GetStoreRegionMeta()->GetAllAliveRegion();
  for (auto& region : regions) {
    if (!RegisterExecutor(region->Id())) {
      DINGO_LOG(ERROR) << "Register region control executor failed, region: " << region->Id();
      return false;
    }
  }

  return true;
}

bool RegionController::Recover() {
  auto commands =
      Server::GetInstance()->GetRegionCommandManager()->GetCommands(pb::coordinator::RegionCmdStatus::STATUS_NONE);

  for (auto& command : commands) {
    auto ctx = std::make_shared<Context>();

    auto status = InnerDispatchRegionControlCommand(ctx, command);
    if (!status.ok()) {
      DINGO_LOG(ERROR) << "Recover region control command failed, error: " << status.error_str();
    }
  }

  return true;
}

void RegionController::Destroy() {
  BAIDU_SCOPED_LOCK(mutex_);
  for (auto [_, executor] : executors_) {
    executor->Stop();
  }

  share_executor_->Stop();
}

std::vector<uint64_t> RegionController::GetAllRegion() {
  BAIDU_SCOPED_LOCK(mutex_);
  std::vector<uint64_t> region_ids;
  region_ids.reserve(executors_.size());
  for (auto [region_id, _] : executors_) {
    region_ids.push_back(region_id);
  }
  return region_ids;
}

bool RegionController::RegisterExecutor(uint64_t region_id) {
  BAIDU_SCOPED_LOCK(mutex_);

  if (executors_.find(region_id) == executors_.end()) {
    auto executor = std::make_shared<RegionControlExecutor>(region_id);
    if (!executor->Init()) {
      DINGO_LOG(ERROR) << "Region controller executor init failed";
      return false;
    }
    executors_.insert({region_id, executor});
  }

  return true;
}

void RegionController::UnRegisterExecutor(uint64_t region_id) {
  std::shared_ptr<RegionControlExecutor> executor;
  {
    BAIDU_SCOPED_LOCK(mutex_);
    auto it = executors_.find(region_id);
    if (it != executors_.end()) {
      executor = it->second;
      executors_.erase(it);
    }
  }
  if (executor != nullptr) {
    executor->Stop();
  }
}

std::shared_ptr<RegionControlExecutor> RegionController::GetRegionControlExecutor(uint64_t region_id) {
  BAIDU_SCOPED_LOCK(mutex_);

  auto it = executors_.find(region_id);
  if (it == executors_.end()) {
    return nullptr;
  }

  return it->second;
}

butil::Status RegionController::InnerDispatchRegionControlCommand(std::shared_ptr<Context> ctx,
                                                                  std::shared_ptr<pb::coordinator::RegionCmd> command) {
  DINGO_LOG(DEBUG) << fmt::format("Dispatch region control command, region {} {} {}", command->region_id(),
                                  command->id(), pb::coordinator::RegionCmdType_Name(command->region_cmd_type()));

  // Create region, need to add region control executor
  if (command->region_cmd_type() == pb::coordinator::RegionCmdType::CMD_CREATE) {
    RegisterExecutor(command->region_id());
  }

  auto executor = (command->region_cmd_type() == pb::coordinator::RegionCmdType::CMD_PURGE ||
                   command->region_cmd_type() == pb::coordinator::RegionCmdType::CMD_DESTROY_EXECUTOR)
                      ? share_executor_
                      : GetRegionControlExecutor(command->region_id());
  if (executor == nullptr) {
    DINGO_LOG(ERROR) << "Not find region control executor, regoin: " << command->region_id();
    return butil::Status(pb::error::EREGION_NOT_FOUND, "Not find regon control executor");
  }

  auto it = task_builders.find(command->region_cmd_type());
  if (it == task_builders.end()) {
    DINGO_LOG(ERROR) << "Not exist region control command";
    return butil::Status(pb::error::EINTERNAL, "Not exist region control command");
  }

  // Free at ExecuteRoutine()
  TaskRunnable* task = it->second(ctx, command);
  if (task == nullptr) {
    DINGO_LOG(ERROR) << "Not support region control command";
    return butil::Status(pb::error::EINTERNAL, "Not support region control command");
  }
  if (!executor->Execute(task)) {
    return butil::Status(pb::error::EINTERNAL, "Execute region control command failed");
  }

  return butil::Status();
}

butil::Status RegionController::DispatchRegionControlCommand(std::shared_ptr<Context> ctx,
                                                             std::shared_ptr<pb::coordinator::RegionCmd> command) {
  // Check repeat region command
  auto region_command_manager = Server::GetInstance()->GetRegionCommandManager();
  if (region_command_manager->IsExist(command->id())) {
    return butil::Status(pb::error::EREGION_REPEAT_COMMAND, "Repeat region control command");
  }

  // Save region command
  region_command_manager->AddCommand(command);

  return InnerDispatchRegionControlCommand(ctx, command);
}

RegionController::ValidateFunc RegionController::GetValidater(pb::coordinator::RegionCmdType cmd_type) {
  auto it = validaters.find(cmd_type);
  if (it == validaters.end()) {
    DINGO_LOG(ERROR) << "Unknown command type: " << pb::coordinator::RegionCmdType_Name(cmd_type);
    return nullptr;
  }

  return it->second;
}

RegionController::TaskBuilderMap RegionController::task_builders = {
    {pb::coordinator::CMD_CREATE,
     [](std::shared_ptr<Context> ctx, std::shared_ptr<pb::coordinator::RegionCmd> command) -> TaskRunnable* {
       return new CreateRegionTask(ctx, command);
     }},
    {pb::coordinator::CMD_DELETE,
     [](std::shared_ptr<Context> ctx, std::shared_ptr<pb::coordinator::RegionCmd> command) -> TaskRunnable* {
       return new DeleteRegionTask(ctx, command);
     }},
    {pb::coordinator::CMD_SPLIT,
     [](std::shared_ptr<Context> ctx, std::shared_ptr<pb::coordinator::RegionCmd> command) -> TaskRunnable* {
       return new SplitRegionTask(ctx, command);
     }},
    {pb::coordinator::CMD_MERGE,
     [](std::shared_ptr<Context>, std::shared_ptr<pb::coordinator::RegionCmd>) -> TaskRunnable* { return nullptr; }},
    {pb::coordinator::CMD_CHANGE_PEER,
     [](std::shared_ptr<Context> ctx, std::shared_ptr<pb::coordinator::RegionCmd> command) -> TaskRunnable* {
       return new ChangeRegionTask(ctx, command);
     }},
    {pb::coordinator::CMD_TRANSFER_LEADER,
     [](std::shared_ptr<Context> ctx, std::shared_ptr<pb::coordinator::RegionCmd> command) -> TaskRunnable* {
       return new TransferLeaderTask(ctx, command);
     }},
    {pb::coordinator::CMD_SNAPSHOT,
     [](std::shared_ptr<Context> ctx, std::shared_ptr<pb::coordinator::RegionCmd> command) -> TaskRunnable* {
       return new SnapshotRegionTask(ctx, command);
     }},
    {pb::coordinator::CMD_PURGE,
     [](std::shared_ptr<Context> ctx, std::shared_ptr<pb::coordinator::RegionCmd> command) -> TaskRunnable* {
       return new PurgeRegionTask(ctx, command);
     }},
    {pb::coordinator::CMD_STOP,
     [](std::shared_ptr<Context> ctx, std::shared_ptr<pb::coordinator::RegionCmd> command) -> TaskRunnable* {
       return new StopRegionTask(ctx, command);
     }},
    {pb::coordinator::CMD_DESTROY_EXECUTOR,
     [](std::shared_ptr<Context> ctx, std::shared_ptr<pb::coordinator::RegionCmd> command) -> TaskRunnable* {
       return new DestroyRegionExecutorTask(ctx, command);
     }},
};

RegionController::ValidaterMap RegionController::validaters = {
    {pb::coordinator::CMD_CREATE, CreateRegionTask::PreValidateCreateRegion},
    {pb::coordinator::CMD_DELETE, DeleteRegionTask::PreValidateDeleteRegion},
    {pb::coordinator::CMD_SPLIT, SplitRegionTask::PreValidateSplitRegion},
    {pb::coordinator::CMD_CHANGE_PEER, ChangeRegionTask::PreValidateChangeRegion},
    {pb::coordinator::CMD_TRANSFER_LEADER, TransferLeaderTask::PreValidateTransferLeader},
    {pb::coordinator::CMD_PURGE, PurgeRegionTask::PreValidatePurgeRegion},
    {pb::coordinator::CMD_STOP, StopRegionTask::PreValidateStopRegion},
};

}  // namespace dingodb