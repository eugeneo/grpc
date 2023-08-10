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

#include "test/cpp/interop/pre_stop_hook_server.h"

#include <map>
#include <memory>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "absl/strings/str_format.h"

#include <grpc/grpc.h>
#include <grpcpp/grpcpp.h>

#include "src/proto/grpc/testing/test.grpc.pb.h"
#include "test/core/util/port.h"
#include "test/core/util/test_config.h"

namespace grpc {
namespace testing {
namespace {

TEST(PreStopHookServer, StartDoRequestStop) {
  int port = grpc_pick_unused_port_or_die();
  PreStopHookServerManager server;
  Status start_status = server.Start(port);
  ASSERT_TRUE(start_status.ok()) << start_status.error_message();
  auto channel = CreateChannel(absl::StrFormat("127.0.0.1:%d", port),
                               InsecureChannelCredentials());
  ASSERT_TRUE(channel);
  ClientContext context;
  CompletionQueue cq;
  Empty request, response;
  HookService::Stub stub(std::move(channel));
  auto call = stub.PrepareAsyncHook(&context, request, &cq);
  call->StartCall();
  Status status;
  int tag = 42;
  call->Finish(&response, &status, &tag);
  server.Return(StatusCode::INTERNAL, "Just a test");
  int* returned_tag;
  bool ok = false;
  CompletionQueue::NextStatus st;
  cq.Next(reinterpret_cast<void**>(&returned_tag), &ok);
  EXPECT_TRUE(ok);
  EXPECT_EQ(*returned_tag, tag);
  cq.Shutdown();
  EXPECT_EQ(status.error_code(), StatusCode::INTERNAL);
  EXPECT_EQ(status.error_message(), "Just a test");
}

TEST(PreStopHookServer, DISABLED_StartedWhileRunning) {}
TEST(PreStopHookServer, DISABLED_ClosingWhilePending) {}
TEST(PreStopHookServer, DISABLED_MultiplePending) {}
TEST(PreStopHookServer, DISABLED_StoppingNotStarted) {}

}  // namespace
}  // namespace testing
}  // namespace grpc

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  grpc::testing::TestEnvironment env(&argc, argv);
  grpc_init();
  auto result = RUN_ALL_TESTS();
  grpc_shutdown();
  return result;
}
