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
#ifndef ONEFLOW_USER_KERNELS_COLLECTIVE_COMMUNICATION_INCLUDE_ALL_REDUCE_H_
#define ONEFLOW_USER_KERNELS_COLLECTIVE_COMMUNICATION_INCLUDE_ALL_REDUCE_H_

#include "oneflow/user/kernels/collective_communication/include/collective_communication.h"

namespace oneflow {

namespace ccl {

class AllReduce : public CollectiveCommunication {
 public:
  OF_DISALLOW_COPY_AND_MOVE(AllReduce);
  AllReduce() = default;
  ~AllReduce() override = default;

  virtual void Init(DataType dtype, ReduceType reduce_type) = 0;

  virtual void Launch(ep::Stream* stream, const void* in, void* out, size_t elem_cnt,
                      const std::shared_ptr<CommunicationContext>& communicator) const = 0;
};

inline bool IsAllReduceRegistered(DeviceType device_type) {
  return IsClassRegistered<DeviceType, AllReduce>(device_type);
}

}  // namespace ccl

}  // namespace oneflow

#endif  // ONEFLOW_USER_KERNELS_COLLECTIVE_COMMUNICATION_INCLUDE_ALL_REDUCE_H_
