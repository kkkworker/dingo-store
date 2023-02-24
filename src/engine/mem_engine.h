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

#ifndef DINGODB_ENGINE_MEM_ENGINE_H_
#define DINGODB_ENGINE_MEM_ENGINE_H_

#include <map>
#include <shared_mutex>

#include "common/context.h"
#include "engine/engine.h"
#include "proto/error.pb.h"
#include "proto/store.pb.h"

namespace dingodb {

class MemEngine : public Engine {
 public:
  MemEngine();

  bool Init();
  std::string GetName();
  uint32_t GetID();

  int AddRegion(uint64_t region_id, const pb::common::Region& region);
  int DestroyRegion(uint64_t region_id);

  std::shared_ptr<std::string> KvGet(std::shared_ptr<Context> ctx,
                                     const std::string& key);
  pb::error::Errno KvPut(std::shared_ptr<Context> ctx,
                         const pb::common::KeyValue& kv);

 private:
  std::shared_mutex mutex_;
  std::map<std::string, std::string> store_;
};

}  // namespace dingodb

#endif