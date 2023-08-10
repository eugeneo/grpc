//
//
// Copyright 2023 gRPC authors.
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
//
//

#include "test/cpp/interop/pre_stop_hook_server.h"

namespace grpc {
namespace testing {

class PreStopHookServer {};

Status PreStopHookServerManager::Start(int port) {
  if (server_) {
    return Status(StatusCode::ALREADY_EXISTS,
                  "Pre hook server is already running");
  }
  server_ = std::unique_ptr<PreStopHookServer,
                            PreStopHookServerManager::DeleteServer>(
      new PreStopHookServer(), PreStopHookServerManager::DeleteServer());
  return Status::OK;
}

Status PreStopHookServerManager::Stop() {
  if (!server_) {
    return Status(StatusCode::UNAVAILABLE, "Pre hook server is not running");
  }
  server_.reset();
  return Status::OK;
}
Status PreStopHookServerManager::Return(StatusCode code,
                                        absl::string_view description) {
  return Status::OK;
}

void PreStopHookServerManager::DeleteServer::operator()(
    PreStopHookServer* server) {
  delete server;
}

}  // namespace testing
}  // namespace grpc
