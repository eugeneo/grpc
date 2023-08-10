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

#ifndef GRPC_TEST_CPP_INTEROP_PRE_STOP_HOOK_SERVER_H
#define GRPC_TEST_CPP_INTEROP_PRE_STOP_HOOK_SERVER_H

#include <grpc/support/port_platform.h>

#include <grpcpp/server.h>

#include "src/core/lib/config/core_configuration.h"

namespace grpc {
namespace testing {

class PreStopHookServer;

class PreStopHookServerManager {
 public:
  Status Start(int port);
  Status Stop();
  void Return(StatusCode code, absl::string_view description);

 private:
  struct DeleteServer {
    void operator()(PreStopHookServer* server);
  };
  // Custom deleter so we don't have to include PreStopHookServer in this header
  std::unique_ptr<PreStopHookServer, DeleteServer> server_;
};

}  // namespace testing
}  // namespace grpc
#endif  // GRPC_TEST_CPP_INTEROP_PRE_STOP_HOOK_SERVER_H
