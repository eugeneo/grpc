//
//
// Copyright 2024 gRPC authors.
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

#include <memory>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "absl/base/thread_annotations.h"

#include <grpc/grpc.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/support/status.h>

#include "src/core/lib/gprpp/sync.h"
#include "test/core/util/port.h"
#include "test/core/util/test_config.h"
#include "test/cpp/end2end/helloworld.grpc.pb.h"
#include "test/cpp/end2end/helloworld.pb.h"

namespace grpc {
namespace testing {

namespace {

class WriteReactor : public ServerWriteReactor<helloworld::HelloReply> {
 public:
  void OnDone() override { delete this; }
  void Write(absl::string_view str) {
    res_.set_message(str);
    StartWrite(&res_, WriteOptions().set_buffer_hint());
  }

 private:
  helloworld::HelloReply res_;
};

class GreeterImpl : public helloworld::Greeter::CallbackService {
  ServerWriteReactor<helloworld::HelloReply>* SayHelloStreamReply(
      CallbackServerContext* context,
      const ::helloworld::HelloRequest* request) override {
    auto* reactor = new WriteReactor();
    reactor->Write(std::string(1050, '#'));
    return reactor;
  }
};

class GreeterServer {
 public:
  GreeterServer() : thread_(Loop, this) {
    grpc_core::MutexLock lock(&mu_);
    while (server_ == nullptr) {
      cond_.Wait(&mu_);
    }
  }

  ~GreeterServer() {
    server_->Shutdown();
    thread_.join();
  }

  std::string address() {
    grpc_core::MutexLock lock(&mu_);
    return address_;
  }

 private:
  static void Loop(GreeterServer* server) {
    GreeterImpl service;
    {
      grpc_core::MutexLock lock(&server->mu_);
      server->address_ =
          absl::StrCat("127.0.0.1:", grpc_pick_unused_port_or_die());
      ServerBuilder builder;
      builder.AddListeningPort(server->address_, InsecureServerCredentials());
      builder.AddChannelArgument(GRPC_ARG_HTTP2_WRITE_BUFFER_SIZE, 1024);
      builder.RegisterService(&service);
      server->server_ = builder.BuildAndStart();
      server->cond_.SignalAll();
    }
    server->server_->Wait();
  }

  std::thread thread_;
  grpc_core::Mutex mu_;
  grpc_core::CondVar cond_;
  std::string address_ ABSL_GUARDED_BY(&mu_);
  std::unique_ptr<Server> server_;
};

TEST(BufferHintHangTest, SettingBufferHintOnFirstRpcDoesNotHang) {
  GreeterServer server;
  auto channel = CreateChannel(server.address(), InsecureChannelCredentials());
  helloworld::Greeter::Stub stub(channel);
  ClientContext context;
  helloworld::HelloRequest req;
  helloworld::HelloReply res;
  req.set_name("It's me!");
  auto stream = stub.SayHelloStreamReply(&context, req);
  ASSERT_TRUE(stream->Read(&res));
  EXPECT_EQ(res.message(), std::string(10, '#'));
}

}  // namespace

}  // namespace testing
}  // namespace grpc

int main(int argc, char** argv) {
  grpc::testing::TestEnvironment env(&argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
