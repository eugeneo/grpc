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

#ifndef GRPC_SRC_CORE_LIB_EVENT_ENGINE_POSIX_ENGINE_EV_EPOLL1_LINUX_H
#define GRPC_SRC_CORE_LIB_EVENT_ENGINE_POSIX_ENGINE_EV_EPOLL1_LINUX_H
#include <bitset>
#include <cstddef>
#include <list>
#include <memory>
#include <string>

#include "absl/base/thread_annotations.h"
#include "absl/container/inlined_vector.h"
#include "absl/functional/function_ref.h"
#include "absl/strings/string_view.h"

#include <grpc/event_engine/event_engine.h>
#include <grpc/support/port_platform.h>

#include "src/core/lib/event_engine/poller.h"
#include "src/core/lib/event_engine/posix_engine/event_poller.h"
#include "src/core/lib/event_engine/posix_engine/internal_errqueue.h"
#include "src/core/lib/event_engine/posix_engine/lockfree_event.h"
#include "src/core/lib/event_engine/posix_engine/wakeup_fd_posix.h"
#include "src/core/lib/gprpp/sync.h"
#include "src/core/lib/iomgr/port.h"

#ifdef GRPC_LINUX_EPOLL
#include <sys/epoll.h>
#endif

#define MAX_EPOLL_EVENTS 100

namespace grpc_event_engine {
namespace experimental {

class Epoll1Poller;

class Epoll1EventHandle : public EventHandle {
 public:
  void ReInit(int fd);
  PosixEventPoller* Poller() override;
  // Specific subclass
  Epoll1Poller* epoll_poller() const { return poller_; }
  bool SetPendingActions(bool pending_read, bool pending_write,
                         bool pending_error);
  int WrappedFd() override { return fd_; }
  void OrphanHandle(PosixEngineClosure* on_done, int* release_fd,
                    absl::string_view reason) override;
  void ShutdownHandle(absl::Status why) override;
  void NotifyOnRead(PosixEngineClosure* on_read) override;
  void NotifyOnWrite(PosixEngineClosure* on_write) override;
  void NotifyOnError(PosixEngineClosure* on_error) override;
  void SetReadable() override;
  void SetWritable() override;
  void SetHasError() override;
  bool IsHandleShutdown() override;
  inline void ExecutePendingActions();
  grpc_core::Mutex* mu() { return &mu_; }
  LockfreeEvent* ReadClosure() { return read_closure_.get(); }
  LockfreeEvent* WriteClosure() { return write_closure_.get(); }
  LockfreeEvent* ErrorClosure() { return error_closure_.get(); }
  ~Epoll1EventHandle() override = default;

 private:
  // This really belongs to Epoll1EventHandlePool class but then there would
  // be a circular dependency...
  static constexpr size_t kBlockSize = 16;
  // These events are only created inside the pool. Need to have a default
  // constructor as the class is non-movable and is allocated in the array
  friend class std::array<Epoll1EventHandle, kBlockSize>;
  Epoll1EventHandle() = default;
  friend class Epoll1EventHandlePool;
  void SetPoller(Epoll1Poller* poller);
  void HandleShutdownInternal(absl::Status why, bool releasing_fd);
  // See Epoll1Poller::ShutdownHandle for explanation on why a mutex is
  // required.
  grpc_core::Mutex mu_;
  int fd_;
  // See Epoll1Poller::SetPendingActions for explanation on why pending_<***>_
  // need to be atomic.
  std::atomic<bool> pending_read_{false};
  std::atomic<bool> pending_write_{false};
  std::atomic<bool> pending_error_{false};
  Epoll1Poller* poller_;
  std::unique_ptr<LockfreeEvent> read_closure_;
  std::unique_ptr<LockfreeEvent> write_closure_;
  std::unique_ptr<LockfreeEvent> error_closure_;
};

template <typename EventHandle>
class Epoll1EventHandlePool {
 public:
  explicit Epoll1EventHandlePool(Epoll1Poller* poller) {
    for (Epoll1EventHandle& handle : events_) {
      handle.SetPoller(poller);
    }
  }

  Epoll1EventHandle* GetFreeEvent() {
    grpc_core::MutexLock lock(&mu_);
    Epoll1EventHandle* handle = GetFreeEventFromBlock();
    if (handle != nullptr) {
      return handle;
    }
    if (next_block_ == nullptr) {  // Check next block
      next_block_ =
          std::make_unique<Epoll1EventHandlePool>(events_[0].epoll_poller());
    }
    return next_block_->GetFreeEvent();
  }

  void ReturnEventHandle(Epoll1EventHandle* handle) {
    grpc_core::MutexLock lock(&mu_);
    if (handle >= &events_.front() && handle <= &events_.back()) {
      int ind = handle - events_.data();
      GPR_ASSERT(events_in_use_[ind]);
      events_in_use_[ind] = false;
      gpr_log(GPR_INFO, "[%p] Returning event %d", this, ind);
    } else if (next_block_ != nullptr) {
      next_block_->ReturnEventHandle(handle);
    } else {
      gpr_log(GPR_ERROR, "No block containing event %p", handle);
    }
  }

  void CloseAllOnFork();

 private:
  Epoll1EventHandle* GetFreeEventFromBlock()
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(&mu_) {
    // Short circuit
    if (events_in_use_.all()) {
      return nullptr;
    }
    for (size_t i = 0; i < events_in_use_.size(); ++i) {
      if (!events_in_use_[i]) {
        events_in_use_[i] = true;
        return &events_[i];
      }
    }
    return nullptr;
  }

  grpc_core::Mutex mu_;
  std::array<Epoll1EventHandle, Epoll1EventHandle::kBlockSize> events_
      ABSL_GUARDED_BY(&mu_);
  std::bitset<Epoll1EventHandle::kBlockSize> events_in_use_
      ABSL_GUARDED_BY(&mu_);
  std::unique_ptr<Epoll1EventHandlePool> next_block_ ABSL_GUARDED_BY(&mu_);
};

// Definition of epoll1 based poller.
class Epoll1Poller : public PosixEventPoller {
 public:
  explicit Epoll1Poller(Scheduler* scheduler);
  EventHandleRef CreateHandle(int fd, absl::string_view name,
                              bool track_err) override;
  Poller::WorkResult Work(
      grpc_event_engine::experimental::EventEngine::Duration timeout,
      absl::FunctionRef<void()> schedule_poll_again) override;
  std::string Name() override { return "epoll1"; }
  void Kick() override;
  Scheduler* GetScheduler() { return scheduler_; }
  void Shutdown() override;
  bool CanTrackErrors() const override {
#ifdef GRPC_POSIX_SOCKET_TCP
    return KernelSupportsErrqueue();
#else
    return false;
#endif
  }
  ~Epoll1Poller() override;

  // Forkable
  void PrepareFork() override;
  void PostforkParent() override;
  void PostforkChild() override;

  void Close();
  void CloseOnFork();

 private:
  // This initial vector size may need to be tuned
  using Events = absl::InlinedVector<Epoll1EventHandle*, 5>;
  // Process the epoll events found by DoEpollWait() function.
  // - g_epoll_set.cursor points to the index of the first event to be processed
  // - This function then processes up-to max_epoll_events_to_handle and
  //   updates the g_epoll_set.cursor.
  // It returns true, it there was a Kick that forced invocation of this
  // function. It also returns the list of closures to run to take action
  // on file descriptors that became readable/writable.
  bool ProcessEpollEvents(int max_epoll_events_to_handle,
                          Events& pending_events);

  //  Do epoll_wait and store the events in g_epoll_set.events field. This does
  //  not "process" any of the events yet; that is done in ProcessEpollEvents().
  //  See ProcessEpollEvents() function for more details. It returns the number
  // of events generated by epoll_wait.
  int DoEpollWait(
      grpc_event_engine::experimental::EventEngine::Duration timeout);
  friend class Epoll1EventHandle;
#ifdef GRPC_LINUX_EPOLL
  struct EpollSet {
    int epfd;

    // The epoll_events after the last call to epoll_wait()
    struct epoll_event events[MAX_EPOLL_EVENTS];

    // The number of epoll_events after the last call to epoll_wait()
    int num_events;

    // Index of the first event in epoll_events that has to be processed. This
    // field is only valid if num_events > 0
    int cursor;
  };
#else
  struct EpollSet {};
#endif
  grpc_core::Mutex mu_;
  Scheduler* scheduler_;
  // A singleton epoll set
  EpollSet g_epoll_set_;
  bool was_kicked_ ABSL_GUARDED_BY(mu_);
  Epoll1EventHandlePool events_;
  std::unique_ptr<WakeupFd> wakeup_fd_;
  bool closed_;
};

// Return an instance of a epoll1 based poller tied to the specified event
// engine.
std::shared_ptr<Epoll1Poller> MakeEpoll1Poller(Scheduler* scheduler);

}  // namespace experimental
}  // namespace grpc_event_engine

#endif  // GRPC_SRC_CORE_LIB_EVENT_ENGINE_POSIX_ENGINE_EV_EPOLL1_LINUX_H
