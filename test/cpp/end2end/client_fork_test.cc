// Copyright 2022 The gRPC Authors
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

#include <grpc/support/port_platform.h>

#ifndef GRPC_ENABLE_FORK_SUPPORT
// No-op for builds without fork support.
int main(int /* argc */, char** /* argv */) { return 0; }
#else  // GRPC_ENABLE_FORK_SUPPORT

#include <signal.h>

#include <gtest/gtest.h>

#include "absl/strings/str_cat.h"

#include <grpc/fork.h>
#include <grpc/grpc.h>
#include <grpc/support/log.h>
#include <grpc/support/time.h>
#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>

#include "src/core/lib/gprpp/fork.h"
#include "src/proto/grpc/testing/echo.grpc.pb.h"
#include "test/core/test_util/port.h"
#include "test/core/test_util/test_config.h"
#include "test/cpp/util/test_config.h"

namespace grpc {
namespace testing {
namespace {

class ServiceImpl final : public EchoTestService::Service {
  Status BidiStream(
      ServerContext* /*context*/,
      ServerReaderWriter<EchoResponse, EchoRequest>* stream) override {
    EchoRequest request;
    EchoResponse response;
    while (stream->Read(&request)) {
      gpr_log(GPR_INFO, "recv msg %s", request.message().c_str());
      response.set_message(request.message());
      stream->Write(response);
      gpr_log(GPR_INFO, "wrote msg %s", response.message().c_str());
    }
    return Status::OK;
  }
};

std::unique_ptr<EchoTestService::Stub> MakeStub(const std::string& addr) {
  return EchoTestService::NewStub(
      grpc::CreateChannel(addr, InsecureChannelCredentials()));
}

void RunServer(const std::string& addr) {
  gpr_log(GPR_INFO, "Server pid: %d", getpid());
  ServiceImpl impl;
  grpc::ServerBuilder builder;
  builder.AddListeningPort(addr, grpc::InsecureServerCredentials());
  builder.RegisterService(&impl);
  std::unique_ptr<Server> server(builder.BuildAndStart());
  server->Wait();
}

void SendMessage(const std::string& addr, const std::string& message) {
  std::unique_ptr<EchoTestService::Stub> stub = MakeStub(addr);
  EchoRequest request;
  EchoResponse response;
  ClientContext context;
  context.set_wait_for_ready(true);
  auto stream = stub->BidiStream(&context);
  request.set_message(message);
  ASSERT_TRUE(stream->Write(request));
  ASSERT_TRUE(stream->Read(&response));
  ASSERT_EQ(response.message(), request.message());
}

TEST(ClientForkTest, ClientCallsBeforeAndAfterForkSucceed) {
  grpc_core::Fork::Enable(true);
  int port = grpc_pick_unused_port_or_die();
  std::string addr = absl::StrCat("localhost:", port);
  gpr_log(GPR_INFO, "Test pid: %d", getpid());

  pid_t server_pid = fork();
  ASSERT_NE(server_pid, -1) << "fork failed";
  // Run server in child
  if (server_pid == 0) {
    gpr_log(GPR_INFO, "Server pid: %d", getpid());
    sleep(2);
    RunServer(addr);
    exit(0);
  }
  SendMessage(addr, "Hello");

  sleep(1);
  // Fork and do round trips in the post-fork parent and child.
  gpr_log(GPR_INFO, "About to fork (%d)", getpid());
  pid_t child_client_pid = fork();
  ASSERT_NE(child_client_pid, -1) << "fork failed";
  gpr_log(GPR_INFO, "After fork 2 (%d)", getpid());
  SendMessage(addr, child_client_pid == 0 ? "Hello again from child"
                                          : "Hello again from parent");
  // Cleanup childe processes from parent
  if (child_client_pid != 0) {
    // Wait for the post-fork child to exit; ensure it exited cleanly.
    int child_status;
    ASSERT_EQ(waitpid(child_client_pid, &child_status, 0), child_client_pid)
        << "failed to get status of child client";
    ASSERT_EQ(WEXITSTATUS(child_status), 0) << "child did not exit cleanly";
    kill(server_pid, SIGINT);
  }
}

}  // namespace
}  // namespace testing
}  // namespace grpc

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  grpc::testing::InitTest(&argc, &argv, true);
  grpc::testing::TestEnvironment env(&argc, argv);
  grpc_init();
  int res = RUN_ALL_TESTS();
  grpc_shutdown();
  return res;
}
#endif  // GRPC_ENABLE_FORK_SUPPORT
