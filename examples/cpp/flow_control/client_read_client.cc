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

#include <iostream>
#include <string>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "examples/protos/hellostreamingworld.grpc.pb.h"

#include <grpc/grpc.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/grpcpp.h>

ABSL_FLAG(std::string, target, "localhost:50051", "Server address");
ABSL_FLAG(bool, bdp_probe, false,
          "Setting to true will enable gRPC to increase buffer sizes and allow "
          "for more in-flight bytes");

namespace {

class Reader final
    : public grpc::ClientReadReactor<hellostreamingworld::HelloReply> {
 public:
  void Start() {
    StartRead(&res_);
    StartCall();
  }

  grpc::Status WaitForDone() {
    absl::MutexLock lock(&mu_);
    mu_.Await(absl::Condition(
        +[](Reader* reader) { return reader->result_.has_value(); }, this));
    return *result_;
  }

  void OnReadDone(bool ok) override {
    if (!ok) {
      std::cout << "Done reading\n";
      return;
    }
    std::cout << "Read " << res_.message().length() << " bytes.\n";
    res_.set_message("");
    if (ok) {
      sleep(1);
      StartRead(&res_);
    }
  }

  void OnDone(const grpc::Status& status) override {
    absl::MutexLock lock(&mu_);
    result_ = status;
  }

 private:
  absl::Mutex mu_;
  absl::optional<grpc::Status> result_;
  hellostreamingworld::HelloReply res_;
};

}  // namespace

int main(int argc, char* argv[]) {
  absl::ParseCommandLine(argc, argv);
  grpc::ChannelArguments channel_arguments;
  channel_arguments.SetInt("grpc.http2.bdp_probe", 0);

  auto greeter =
      hellostreamingworld::MultiGreeter::NewStub(grpc::CreateCustomChannel(
          absl::GetFlag(FLAGS_target), grpc::InsecureChannelCredentials(),
          channel_arguments));
  grpc::ClientContext ctx;
  hellostreamingworld::HelloRequest req;
  req.set_name(std::string(4000000, '*'));
  Reader reader;
  greeter->async()->sayHello(&ctx, &req, &reader);
  reader.Start();
  auto status = reader.WaitForDone();
  if (status.ok()) {
    std::cout << "Success\n";
  } else {
    std::cerr << "Failed with error: " << status.error_message() << "\n";
  }
  return 0;
}