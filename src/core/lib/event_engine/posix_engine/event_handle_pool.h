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

#ifndef GRPC_SRC_CORE_LIB_EVENT_ENGINE_POSIX_ENGINE_EVENT_HANDLE_POOL_H
#define GRPC_SRC_CORE_LIB_EVENT_ENGINE_POSIX_ENGINE_EVENT_HANDLE_POOL_H

#include <bitset>
#include <memory>

#include "absl/functional/any_invocable.h"
#include "absl/log/log.h"

#include "src/core/lib/gprpp/sync.h"

namespace grpc_event_engine {
namespace experimental {

static constexpr size_t kBlockSize = 16;

template <typename Poller, typename EventHandle>
class EventHandlePool {
 public:
  explicit EventHandlePool(Poller* poller) : poller_(poller) {
    for (EventHandle& handle : events_) {
      handle.SetPoller(poller);
    }
  }

  EventHandle* GetEventFromPool(int fd) {
    EventHandle* handle = GetFreeEventFromBlock();
    if (handle != nullptr) {
      handle->InitWithFd(fd);
      return handle;
    }
    if (next_block_ == nullptr) {  // Check next block
      next_block_ = std::make_unique<EventHandlePool>(poller_);
    }
    return next_block_->GetEventFromPool(fd);
  }

  void ReturnEventHandle(EventHandle* handle) {
    LOG(INFO) << "Returning " << handle;
    if (handle >= &events_.front() && handle <= &events_.back()) {
      int ind = handle - events_.data();
      CHECK(events_in_use_[ind]);
      events_in_use_[ind] = false;
      gpr_log(GPR_INFO, "[%p] Returning event %d", this, ind);
    } else if (next_block_ != nullptr) {
      next_block_->ReturnEventHandle(handle);
    } else {
      gpr_log(GPR_ERROR, "No block containing event %p", handle);
    }
  }

  void CloseAllOnFork() {
    for (size_t i = 0; i < events_in_use_.size(); ++i) {
      if (events_in_use_[i]) {
        close(events_[0].WrappedFd());
      }
    }
    if (next_block_ != nullptr) {
      next_block_->CloseAllOnFork();
    }
  }

  bool AllFree() {
    return events_in_use_.none() &&
           (next_block_ == nullptr || next_block_->AllFree());
  }

  void VisitUsedEventHandles(absl::AnyInvocable<void(EventHandle*)> invocable) {
    for (size_t i = 0; i < events_in_use_.size(); ++i) {
      if (events_in_use_[i]) {
        invocable(&events_[i]);
      }
    }
  }

  EventHandlePool* next_block() const { return next_block_.get(); }

 private:
  EventHandle* GetFreeEventFromBlock() {
    // Short circuit
    if (events_in_use_.all()) {
      return nullptr;
    }
    for (size_t i = 0; i < events_in_use_.size(); ++i) {
      if (!events_in_use_[i]) {
        gpr_log(GPR_INFO, "[%p] Getting event %zu", this, i);
        events_in_use_[i] = true;
        return &events_[i];
      }
    }
    return nullptr;
  }

  Poller* poller_;
  std::array<EventHandle, kBlockSize> events_;
  std::bitset<kBlockSize> events_in_use_;
  std::unique_ptr<EventHandlePool> next_block_;
};

}  // namespace experimental
}  // namespace grpc_event_engine

#endif  // GRPC_SRC_CORE_LIB_EVENT_ENGINE_POSIX_ENGINE_EVENT_HANDLE_POOL_H
