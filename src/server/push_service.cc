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

#include "server/push_service.h"

#include <cstdint>

#include "common/constant.h"
#include "common/context.h"
#include "common/helper.h"
#include "common/logging.h"
#include "proto/common.pb.h"
#include "proto/coordinator.pb.h"
#include "proto/push.pb.h"
#include "server/server.h"
#include "store/heartbeat.h"
#include "store/region_controller.h"

namespace dingodb {

PushServiceImpl::PushServiceImpl() = default;

void PushServiceImpl::PushHeartbeat(google::protobuf::RpcController* controller,
                                    const dingodb::pb::push::PushHeartbeatRequest* request,
                                    dingodb::pb::push::PushHeartbeatResponse* /*response*/,
                                    google::protobuf::Closure* done) {
  brpc::Controller* cntl = (brpc::Controller*)controller;
  brpc::ClosureGuard const done_guard(done);
  DINGO_LOG(DEBUG) << "PushHeartbeat request: " << request->ShortDebugString();

  // call HandleStoreHeartbeatResponse
  const auto& heartbeat_response = request->heartbeat_response();
  auto store_meta = Server::GetInstance()->GetStoreMetaManager();
  HeartbeatTask::HandleStoreHeartbeatResponse(store_meta, heartbeat_response);
}

void PushServiceImpl::PushStoreOperation(google::protobuf::RpcController* controller,
                                         const dingodb::pb::push::PushStoreOperationRequest* request,
                                         dingodb::pb::push::PushStoreOperationResponse* response,
                                         google::protobuf::Closure* done) {
  brpc::Controller* cntl = (brpc::Controller*)controller;
  brpc::ClosureGuard const done_guard(done);
  DINGO_LOG(DEBUG) << "PushStoreOperation request: " << request->ShortDebugString();

  if (request->store_operation().id() != Server::GetInstance()->Id()) {
    return;
  }

  auto region_controller = Server::GetInstance()->GetRegionController();
  for (const auto& command : request->store_operation().region_cmds()) {
    butil::Status status;
    auto store_meta_manager = Server::GetInstance()->GetStoreMetaManager();
    switch (command.region_cmd_type()) {
      case pb::coordinator::CMD_CREATE:
        status = CreateRegionTask::ValidateCreateRegion(store_meta_manager, command.region_id());
        break;
      case pb::coordinator::CMD_DELETE:
        status = DeleteRegionTask::ValidateDeleteRegion(
            store_meta_manager, store_meta_manager->GetStoreRegionMeta()->GetRegion(command.region_id()));
        break;
      case pb::coordinator::CMD_SPLIT:
        status =
            SplitRegionTask::ValidateSplitRegion(store_meta_manager->GetStoreRegionMeta(), command.split_request());
        break;
      case pb::coordinator::CMD_CHANGE_PEER:
        status = ChangeRegionTask::ValidateChangeRegion(
            store_meta_manager, store_meta_manager->GetStoreRegionMeta()->GetRegion(command.region_id()));
        break;
      default:
        DINGO_LOG(ERROR) << "Unknown command type: " << command.region_cmd_type();
    }

    auto error_func = [response](uint64_t command_id, ::dingodb::pb::coordinator::RegionCmdType region_cmd_type,
                                 butil::Status status) {
      auto* result = response->add_region_cmd_results();
      result->set_region_cmd_id(command_id);
      result->set_region_cmd_type(region_cmd_type);
      auto* mut_err = result->mutable_error();
      mut_err->set_errcode(static_cast<pb::error::Errno>(status.error_code()));
      mut_err->set_errmsg(status.error_str());
    };

    if (!status.ok()) {
      error_func(command.id(), command.region_cmd_type(), status);
      continue;
    }

    std::shared_ptr<Context> ctx = std::make_shared<Context>();
    status =
        region_controller->DispatchRegionControlCommand(ctx, std::make_shared<pb::coordinator::RegionCmd>(command));
    // coordinator need to get all region_cmd results, so add all results to response here
    error_func(command.id(), command.region_cmd_type(), status);
  }

  if (!response->region_cmd_results().empty()) {
    response->mutable_error()->CopyFrom(response->region_cmd_results(0).error());
  }
}

}  // namespace dingodb
