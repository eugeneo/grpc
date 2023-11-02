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

#ifndef GRPC_SRC_CORE_EXT_SUSPEND_ADS_READ_HANDLE_H
#define GRPC_SRC_CORE_EXT_SUSPEND_ADS_READ_HANDLE_H

#include "src/core/ext/xds/suspend_ads_read_handle.h"
#include "src/core/ext/xds/xds_transport.h"
#include "src/core/lib/gprpp/orphanable.h"

namespace grpc_core {

// ADS reads are suspended while this object is held by at least one watcher.
class SuspendAdsReadHandle : public InternallyRefCounted<SuspendAdsReadHandle> {
 public:
  SuspendAdsReadHandle(
      RefCountedPtr<XdsTransportFactory::XdsTransport::StreamingCall> call)
      : call_(std::move(call)) {}
  SuspendAdsReadHandle(const SuspendAdsReadHandle&) = delete;
  SuspendAdsReadHandle(SuspendAdsReadHandle&&) = delete;

  void Orphan() override { call_->Read(); }

  // DONOTSUBMIT
  // Added for now for places where it seems like there's no reason to suspend
  // read. Will need to be removed/cleaned up for final version
  static RefCountedPtr<SuspendAdsReadHandle> NoWait() { return nullptr; }

 private:
  RefCountedPtr<XdsTransportFactory::XdsTransport::StreamingCall> call_;
};

}  // namespace grpc_core

#endif  // GRPC_SRC_CORE_EXT_SUSPEND_ADS_READ_HANDLE_H