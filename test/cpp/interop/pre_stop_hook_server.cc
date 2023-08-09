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

#include <thread>

namespace grpc {
namespace testing {

class PreStopHookServer {
 public:
  PreStopHookServer(int port, int timeout_s = 15)
      : thread_(PreStopHookServer::ServerThread, this, port) {
    grpc_core::MutexLock lock(&mu_);
    absl::Time deadline = absl::Now() + absl::Seconds(timeout_s);
    while (!startup_status_.has_value() &&
           !startup_cv_.WaitWithDeadline(&mu_, deadline)) {
    }
  }

  ~PreStopHookServer() { thread_.join(); }

  Status startup_status() {
    grpc_core::MutexLock lock(&mu_);
    return startup_status_.value_or(
        Status(StatusCode::DEADLINE_EXCEEDED, "Server did not start on time"));
  }

 private:
  static bool ServerStarting(PreStopHookServer* server) {
    grpc_core::MutexLock lock(&server->mu_);
    return !server->startup_status_.has_value();
  }
  static void ServerThread(PreStopHookServer* server, int port);
  std::thread thread_;
  absl::optional<Status> startup_status_ ABSL_GUARDED_BY(mu_);
  grpc_core::Mutex mu_;
  grpc_core::CondVar startup_cv_ ABSL_GUARDED_BY(mu_);
};

void PreStopHookServer::ServerThread(PreStopHookServer* server, int port) {
  grpc_core::MutexLock lock(&server->mu_);
}

Status PreStopHookServerManager::Start(int port) {
  if (server_) {
    return Status(StatusCode::ALREADY_EXISTS,
                  "Pre hook server is already running");
  }
  server_ = std::unique_ptr<PreStopHookServer,
                            PreStopHookServerManager::DeleteServer>(
      new PreStopHookServer(port), PreStopHookServerManager::DeleteServer());
  return server_->startup_status();
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
