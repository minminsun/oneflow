/*
Copyright 2020 The OneFlow Authors. All rights reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
*/
#include <glog/logging.h>
#include <atomic>
#include <thread>
#include "oneflow/core/odb/odb.h"
#include "oneflow/core/odb/breakpoint_anchor.h"

namespace oneflow {
namespace odb {

namespace {

std::atomic<ThreadType>* MutBreakpointThreadType() {
  static std::atomic<ThreadType> thread_type(kInvalidThreadType);
  return &thread_type;
}

}  // namespace

void SetBreakpointThreadType(ThreadType thread_type) { *MutBreakpointThreadType() = thread_type; }

namespace {

ThreadType* MutThreadLocalThreadType() {
  static thread_local ThreadType thread_type = kNormalThreadType;
  return &thread_type;
}

ThreadType GetThreadLocalThreadType() { return *MutThreadLocalThreadType(); }

}  // namespace

void InitThisThreadType(ThreadType thread_type) { *MutThreadLocalThreadType() = thread_type; }

namespace {

void TryRunBreakpointAnchor() {
  if (BreakpointRangeModeGuard::Current() == kDisableBreakpointRange) { return; }
  ThreadType thread_type = GetThreadLocalThreadType();
  BreakpointAnchor();
  if (thread_type == kNormalThreadType) {
    NormalBreakpointAnchor();
  } else if (thread_type == kSchedulerThreadType) {
    SchedulerBreakpointAnchor();
  } else if (thread_type == kWorkerThreadType) {
    WorkerBreakpointAnchor();
  } else {
    LOG(FATAL) << "UNIMPLEMENTED";
  }
}

}  // namespace

BreakpointRange::BreakpointRange() { TryRunBreakpointAnchor(); }

BreakpointRange::~BreakpointRange() {}

namespace {

BreakpointRangeMode* MutThreadLocalBreakpointRangeMode() {
  static thread_local BreakpointRangeMode mode = kEnableBreakpointRange;
  return &mode;
}

}  // namespace

BreakpointRangeModeGuard::BreakpointRangeModeGuard(BreakpointRangeMode mode) {
  prev_mode_ = *MutThreadLocalBreakpointRangeMode();
  *MutThreadLocalBreakpointRangeMode() = mode;
}

BreakpointRangeModeGuard::~BreakpointRangeModeGuard() {
  *MutThreadLocalBreakpointRangeMode() = prev_mode_;
}

/*static*/ BreakpointRangeMode BreakpointRangeModeGuard::Current() {
  return *MutThreadLocalBreakpointRangeMode();
}

namespace {

template<ThreadType thread_type>
std::atomic<bool>* MutStopVMFlag() {
  static std::atomic<bool> flag(false);
  return &flag;
}

}  // namespace

template<ThreadType thread_type>
bool GetStopVMFlag() {
  return MutStopVMFlag<thread_type>()->load(std::memory_order_acquire);
}

template<ThreadType thread_type>
void StopVMThread() {
  *MutStopVMFlag<thread_type>() = true;
}

template<ThreadType thread_type>
void RestartVMThread() {
  *MutStopVMFlag<thread_type>() = false;
}

template<ThreadType thread_type>
void TryBlockVMThread() {
  while (GetStopVMFlag<thread_type>()) { std::this_thread::yield(); }
}

#define SPECIALIZE_VM_THREAD(thread_type)       \
  template bool GetStopVMFlag<thread_type>();   \
  template void StopVMThread<thread_type>();    \
  template void RestartVMThread<thread_type>(); \
  template void TryBlockVMThread<thread_type>();

SPECIALIZE_VM_THREAD(kSchedulerThreadType);
SPECIALIZE_VM_THREAD(kWorkerThreadType);

}  // namespace odb
}  // namespace oneflow
