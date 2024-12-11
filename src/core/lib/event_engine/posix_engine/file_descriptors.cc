// Copyright 2024 The gRPC Authors
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

namespace grpc_event_engine {
namespace experimental {

namespace {
// class LocksState {
//  public:
//   void Lock(const SystemApi* system_api, int fd) {
//     if (++counters_[system_api] == 1) {
//       system_api->ReaderLock();
//     }
//   }

//   void Unlock(const SystemApi* system_api, int fd) {
//     CHECK_GT(counters_[system_api], 0);
//     if (--counters_[system_api] == 0) {
//       system_api->ReaderUnlock();
//     }
//   }

//  private:
//   std::unordered_map<const SystemApi*, int> counters_;
// };

// thread_local LocksState locks_state;
}  // namespace

// LockedFd::LockedFd(int fd, const SystemApi& system_api)
//     : fd_(fd), system_api_(&system_api) {
//   locks_state.Lock(system_api_, fd_);
// }

// LockedFd::~LockedFd() { locks_state.Unlock(system_api_, fd_); }

}  // namespace experimental
}  // namespace grpc_event_engine