/*
 *
 * Copyright 2024 gRPC authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/log/initialize.h"
#include "absl/log/log.h"
#include "absl/strings/str_cat.h"

#include <grpc/grpc.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/grpcpp.h>

#ifdef BAZEL_BUILD
#include "examples/protos/helloworld.grpc.pb.h"
#else
#include "helloworld.grpc.pb.h"
#endif

ABSL_FLAG(std::string, target, "localhost:50051", "Server address");

using grpc::CallbackServerContext;
using grpc::Channel;
using grpc::ClientContext;
using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerUnaryReactor;
using grpc::Status;
using helloworld::Greeter;
using helloworld::HelloReply;
using helloworld::HelloRequest;

namespace {
class GreeterClient {
 public:
  GreeterClient(std::shared_ptr<Channel> channel)
      : stub_(Greeter::NewStub(channel)) {}

  // Assembles the client's payload, sends it and presents the response back
  // from the server.
  void SayHello(const std::string& message, size_t requests) {
    HelloRequest req;
    req.set_name(message);
    LOG(INFO) << "Warming up";
    MakeCallsUntil(req, [end = absl::Now() + absl::Milliseconds(100)]() {
      return absl::Now() < end;
    });
    LOG(INFO) << "Sending " << requests << " requests";
    MakeCallsUntil(req, [i = requests + 1]() mutable { return (--i) > 0; });
  }

 private:
  class GreeterCall {
   public:
    GreeterCall(Greeter::Stub* stub, const HelloRequest& req, absl::Mutex* mu) {
      stub->async()->SayHello(&context_, &req, &response_,
                              [this, mu](const Status& status) {
                                absl::MutexLock lock(mu);
                                done_ = true;
                              });
    }

    bool done() const { return done_; }

   private:
    ClientContext context_;
    HelloReply response_;
    bool done_ = false;
  };

  void MakeCallsUntil(const HelloRequest& req,
                      absl::AnyInvocable<bool()> predicate) {
    std::list<GreeterCall> calls;
    absl::Mutex mu;
    // Worm up loop
    while (predicate()) {
      auto t1 = absl::Now();
      calls.emplace_back(stub_.get(), req, &mu);
      auto duration = absl::Now() - t1;
      LOG(INFO) << duration;
    }
    {
      LOG(INFO) << "Requests: " << calls.size();
      // Wait for all responses to be received
      absl::MutexLock lock(&mu);
      mu.Await(absl::Condition(
          +[](std::list<GreeterCall>* calls) {
            return std::all_of(
                calls->begin(), calls->end(),
                [](const GreeterCall& call) { return call.done(); });
          },
          &calls));
    }
    LOG(INFO) << "Done!";
  }

  std::unique_ptr<Greeter::Stub> stub_;
};

}  // namespace

int main(int argc, char** argv) {
  absl::ParseCommandLine(argc, argv);
  absl::InitializeLog();
  grpc::ChannelArguments channel_arguments;
  channel_arguments.SetInt("grpc.http2.max_frame_size", 16384);
  channel_arguments.SetInt("grpc.http2.bdp_probe", 0);
  GreeterClient greeter(grpc::CreateCustomChannel(
      absl::GetFlag(FLAGS_target), grpc::InsecureChannelCredentials(),
      channel_arguments));
  greeter.SayHello(std::string(500000000, '*'), 20);
  LOG(INFO) << "Done";
  return 0;
}
