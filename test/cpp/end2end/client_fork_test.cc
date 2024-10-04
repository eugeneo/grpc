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

#include <csignal>

#include <grpc/support/port_platform.h>

#ifndef GRPC_ENABLE_FORK_SUPPORT
// No-op for builds without fork support.
int main(int /* argc */, char** /* argv */) { return 0; }
#else  // GRPC_ENABLE_FORK_SUPPORT

#include <signal.h>

#include <thread>

#include <gtest/gtest.h>

#include "absl/log/log.h"
#include "absl/strings/str_cat.h"

#include <grpc/fork.h>
#include <grpc/grpc.h>
#include <grpc/support/time.h>
#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>

#include "src/core/util/debug_location.h"
#include "src/core/util/fork.h"
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
      LOG(INFO) << "recv msg " << request.message();
      response.set_message(request.message());
      stream->Write(response);
      LOG(INFO) << "wrote msg " << response.message();
    }
    return Status::OK;
  }
};

pid_t InChild(absl::AnyInvocable<void(const std::string&)> child,
              const std::string& addr) {
  pid_t pid = fork();
  if (pid == 0) {
    child(addr);
    exit(0);
  } else {
    return pid;
  }
}

void RunServer(absl::string_view addr) {
  ServiceImpl impl;
  grpc::ServerBuilder builder;
  builder.AddListeningPort(std::string(addr),
                           grpc::InsecureServerCredentials());
  builder.RegisterService(&impl);
  std::unique_ptr<Server> server(builder.BuildAndStart());
  server->Wait();
}

void SendRequest(
    const std::string& addr, absl::string_view message,
    grpc_core::SourceLocation location = grpc_core::SourceLocation()) {
  LOG(INFO) << absl::StrFormat("[PID: %ld] Sending message \"%s\" @ %s:%lu",
                               getpid(), message, location.file(),
                               location.line());
  EchoRequest request;
  EchoResponse response;
  ClientContext context;
  context.set_wait_for_ready(true);
  std::unique_ptr<EchoTestService::Stub> stub = EchoTestService::NewStub(
      grpc::CreateChannel(addr, InsecureChannelCredentials()));
  auto stream = stub->BidiStream(&context);
  request.set_message(message);
  ASSERT_TRUE(stream->Write(request))
      << location.file() << ":" << location.line();
  ASSERT_TRUE(stream->Read(&response))
      << location.file() << ":" << location.line();
  ASSERT_EQ(response.message(), request.message())
      << location.file() << ":" << location.line();
}

void ChildWatcher(pid_t process, int wait_s) {
  sleep(wait_s);
  kill(process, SIGUSR1);
  LOG(INFO) << "Sent a kill signal";
}

TEST(ClientForkTest, ClientCallsBeforeAndAfterForkSucceed) {
  grpc_core::Fork::Enable(true);
  int port = grpc_pick_unused_port_or_die();
  std::string addr = absl::StrCat("localhost:", port);
  pid_t server_pid = InChild(RunServer, addr);
  // Do a round trip before we fork.
  // NOTE: without this scope, test running with the epoll1 poller will
  // fail.
  SendRequest(addr, "Main process message #1");
  pid_t child_client_pid = InChild(
      [](const std::string& addr) { SendRequest(addr, "Child process"); },
      addr);
  std::thread child_watcher(ChildWatcher, child_client_pid, 20);
  ASSERT_GT(child_client_pid, 0);
  SendRequest(addr, "Main process message #2");
  // Wait for the post-fork child to exit; ensure it exited cleanly.
  int child_status;
  ASSERT_EQ(waitpid(child_client_pid, &child_status, 0), child_client_pid)
      << "failed to get status of child client";
  ASSERT_EQ(WEXITSTATUS(child_status), 0) << "child did not exit cleanly";
  child_watcher.join();
  kill(server_pid, SIGINT);
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
