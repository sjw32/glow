/**
 * Copyright (c) 2017-present, Facebook, Inc.
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
 */

// Silence Apple's warning about the deprecation of OpenCL.
#define CL_SILENCE_DEPRECATION

// Silence warnings about using deprecated OpenCL 1.2 functions.
#define CL_USE_DEPRECATED_OPENCL_1_2_APIS

#include "OpenCLDeviceManager.h"
#include "OpenCL.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "opencl"

using namespace glow;
using namespace glow::runtime;

extern llvm::cl::opt<unsigned> clPlatformId;
extern llvm::cl::opt<unsigned> clDeviceId;
extern llvm::cl::opt<bool> clDoProfile;

namespace glow {
namespace runtime {
DeviceManager *createOCLDeviceManager(std::unique_ptr<DeviceConfig> config) {
  return new OpenCLDeviceManager(std::move(config));
}
} // namespace runtime
} // namespace glow

cl_mem OpenCLDeviceManager::allocDeviceBuffer(uint64_t size) {
  const uint64_t alignment = 128;
  // Always allocate buffers properly aligned to hold values of any type.
  size = alignedSize(size, alignment);
  auto buf =
      clCreateBuffer(context_, CL_MEM_READ_WRITE, size, nullptr, nullptr);
  GLOW_ASSERT(buf && "Allocation failed!");
  return buf;
}
OpenCLDeviceManager::OpenCLDeviceManager(std::unique_ptr<DeviceConfig> config)
    : QueueBackedDeviceManager(BackendKind::OpenCL, std::move(config)) {}

llvm::Error OpenCLDeviceManager::init() {
  // The OpenCl Backend defines three command line options: doProfile, deviceId,
  // and platformId. If an OpenCLDeviceConfig is not provided we use the CL
  // options from the OpenCl Backend.

  if (config_ && config_->getBackendKind() == BackendKind::OpenCL) {
    OpenCLDeviceConfig *config =
        static_cast<OpenCLDeviceConfig *>(config_.get());
    clDeviceId = config->deviceId;
    clPlatformId = config->platformId;
    clDoProfile = config->doProfile;
  }

  cl_uint numPlatforms{0};
  cl_int err = clGetPlatformIDs(0, NULL, &numPlatforms);
  if (err != CL_SUCCESS) {
    RETURN_ERR("clGetPlatformIDs Failed.");
  }

  if (numPlatforms < clPlatformId) {
    RETURN_ERR("Should have at least one platform for running OpenCL");
  }

  std::vector<cl_platform_id> platform_ids(numPlatforms);
  err = clGetPlatformIDs(numPlatforms, platform_ids.data(), NULL);

  cl_platform_id platform_id_used = platform_ids[clPlatformId];
  cl_uint num{0};
  err = clGetDeviceIDs(platform_id_used, CL_DEVICE_TYPE_ALL, 0, nullptr, &num);
  if (err != CL_SUCCESS) {
    RETURN_ERR("clGetDeviceIDs Failed");
  }
  if (num < clDeviceId) {
    RETURN_ERR("Should have at least one GPU/CPU/FPGA for running OpenCL");
  }
  std::vector<cl_device_id> devices(num);
  err = clGetDeviceIDs(platform_id_used, CL_DEVICE_TYPE_ALL, num,
                       devices.data(), nullptr);
  if (err != CL_SUCCESS) {
    RETURN_ERR("clGetDeviceIDs Failed");
  }
  deviceId_ = devices[clDeviceId];
  context_ = clCreateContext(nullptr, 1, &deviceId_, nullptr, nullptr, nullptr);
  if (!context_) {
    RETURN_ERR("clCreateContext Failed");
  }
  commands_ = clCreateCommandQueue(
      context_, deviceId_, (clDoProfile) ? CL_QUEUE_PROFILING_ENABLE : 0, &err);
  if (!commands_) {
    RETURN_ERR("clCreateCommandQueue Failed");
  }
  cl_ulong mem_size;
  err = clGetDeviceInfo(deviceId_, CL_DEVICE_GLOBAL_MEM_SIZE, sizeof(cl_ulong),
                        &mem_size, NULL);
  if (err != CL_SUCCESS) {
    RETURN_ERR("Error getting device memory limit");
  }
  maxMemoryBytes_ = mem_size;

  return llvm::Error::success();
}

OpenCLDeviceManager::~OpenCLDeviceManager() {
  clReleaseCommandQueue(commands_);
  clReleaseContext(context_);
  buffers_.clear();
}
uint64_t OpenCLDeviceManager::getMaximumMemory() const {
  return maxMemoryBytes_;
}

uint64_t OpenCLDeviceManager::getAvailableMemory() const {
  return maxMemoryBytes_ - usedMemoryBytes_;
}

bool OpenCLDeviceManager::isMemoryAvailable(uint64_t estimate) const {
  return maxMemoryBytes_ >= (usedMemoryBytes_ + estimate);
}

void OpenCLDeviceManager::addNetworkImpl(const Module *module,
                                         FunctionMapTy functions,
                                         ReadyCBTy readyCB) {
  // First check for uniqueness of the function name.
  for (const auto &func : functions) {
    if (functions_.count(func.first) != 0) {
      readyCB(
          module,
          MAKE_ERR(
              llvm::formatv(
                  "Failed to add network: already have a function called {}",
                  func.first)
                  .str()));
      return;
    }

    if (func.second->getCompileBackendKind() != BackendKind::OpenCL) {
      readyCB(
          module,
          MAKE_ERR(
              llvm::formatv(
                  "Failed to add network: function {} is not a OpenCL Function",
                  func.first)
                  .str()));
    }
  }
  // Collect constants once, since currently the bundle grabs everything in the
  // module.
  auto &bundle = functions.begin()->second->getRuntimeBundle();
  if (bundle.getConstants() == nullptr) {
    bundle.collectConstants(module);
  }
  size_t sizeInBytes = bundle.getConstantWeightSize();
  if (usedMemoryBytes_ + sizeInBytes > maxMemoryBytes_) {
    // Free the constants.
    bundle.freeConstants();
    readyCB(module, MAKE_ERR(GlowErr::ErrorCode::RUNTIME_OUT_OF_DEVICE_MEMORY,
                             "Failed to add network: not enough memory"));
    return;
  }

  // Copy constants to device.
  auto size = bundle.getConstantWeightSize() + bundle.getMutableWeightSize() +
              bundle.getActivationsSize();
  auto deviceBuffer = allocDeviceBuffer(size);
  auto buffer = std::make_shared<OpenCLBuffer>(deviceBuffer, size);
  if (bundle.getConstants()) {
    auto buf = bundle.getConstants();
    size_t valueOffset = 0;
    cl_event event{nullptr};
    cl_int err = clEnqueueWriteBuffer(
        commands_, buffer->getBuffer(), /* blocking_write */ CL_FALSE,
        valueOffset, sizeInBytes, buf, /* num_events_in_wait_list */ 0,
        /* event_list */ nullptr, /* event */ doProfile_ ? &event : nullptr);
    GLOW_ASSERT(err == CL_SUCCESS && "Unable to copy data to the device");
    clFinish(commands_);
  }
  usedMemoryBytes_ += sizeInBytes;
  // Add to the function name lookup map.
  // Add shared pointer to buffer to buffers. This way buffer will be freed
  // after last reference is removed.
  for (const auto &func : functions) {
    functions_.emplace(func.first, func.second);
    buffers_.emplace(func.first, buffer);
    buffer->incrementUsers();
  }

  assert(usedMemoryBytes_ <= maxMemoryBytes_);

  // Fire the ready CB.
  readyCB(module, llvm::Error::success());
}

void OpenCLDeviceManager::evictNetworkImpl(std::string functionName,
                                           EvictFunctionCBTy evictCB) {
  llvm::Error err = llvm::Error::success();

  if (functions_.erase(functionName)) {
    auto buffer = buffers_[functionName];
    auto users = buffer->decrementUsers();
    auto size = buffer->getSize();
    buffers_.erase(functionName);
    if (users == 0) {
      assert(usedMemoryBytes_ >= size);
      usedMemoryBytes_ -= size;
    }
  } else {
    err =
        MAKE_ERR(GlowErr::ErrorCode::RUNTIME_NET_NOT_FOUND,
                 llvm::formatv("Could not find function with name {} to evict",
                               functionName)
                     .str());
  }

  if (evictCB) {
    evictCB(functionName, std::move(err));
  } else {
    llvm::errs() << llvm::toString(std::move(err));
  }
}

void OpenCLDeviceManager::runFunctionImpl(
    RunIdentifierTy id, std::string function,
    std::unique_ptr<ExecutionContext> context, ResultCBTy resultCB) {
  auto funcIt = functions_.find(function);
  if (funcIt == functions_.end()) {
    resultCB(id,
             MAKE_ERR(GlowErr::ErrorCode::RUNTIME_NET_NOT_FOUND,
                      llvm::formatv("Function {} not found", function).str()),
             std::move(context));
    return;
  }

  CompiledFunction *func = funcIt->second;
  PlaceholderBindings &bindings = *context->getPlaceholderBindings();

  // Run that function.
  // Until we have executionInfo object need to call setup/teardown and pin to
  // single device.
  func->setupRuns();
  func->beforeRun(bindings);
  func->execute(context.get());
  func->afterRun(bindings);

  // Fire the resultCB.
  resultCB(id, llvm::Error::success(), std::move(context));
}
