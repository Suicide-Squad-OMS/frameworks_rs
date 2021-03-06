/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "rsovContext.h"

#include <vector>

#include "rsUtils.h"

namespace android {
namespace renderscript {
namespace rsov {

RSoVContext* RSoVContext::mContext = nullptr;
std::once_flag RSoVContext::mInitFlag;

void RSoVContext::Initialize(char const* const name) {
  // Initialize instance
  VkApplicationInfo appInfo = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pNext = nullptr,
      .pApplicationName = name,  // TODO: set to app name
      .applicationVersion = 1,
      .pEngineName = name,
      .engineVersion = 1,
      .apiVersion = VK_API_VERSION_1_0};

  VkInstanceCreateInfo instInfo = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pNext = nullptr,
      .flags = 0,
      .pApplicationInfo = &appInfo,
  };

  VkResult res;
  res = vkCreateInstance(&instInfo, nullptr, &mInstance);
  rsAssert(res == VK_SUCCESS);

  // Enumerate devices
  uint32_t gpu_count;

  res = vkEnumeratePhysicalDevices(mInstance, &gpu_count, nullptr);
  rsAssert(gpu_count > 0);

  std::vector<VkPhysicalDevice> GPUs(gpu_count);

  res = vkEnumeratePhysicalDevices(mInstance, &gpu_count, GPUs.data());
  rsAssert(res == VK_SUCCESS && gpu_count > 0);

  mGPU = GPUs[0];

  // Get device memory properties
  vkGetPhysicalDeviceMemoryProperties(mGPU, &mMemoryProperties);

  // Initialize device

  float queuePriorities[] = {0.0};

  VkDeviceQueueCreateInfo queueInfo = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .pNext = nullptr,
      .queueCount = 1,
      .pQueuePriorities = queuePriorities,
  };

  VkDeviceCreateInfo deviceInfo = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .pNext = nullptr,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &queueInfo,
      .pEnabledFeatures = nullptr,
  };

  res = vkCreateDevice(mGPU, &deviceInfo, nullptr, &mDevice);
  rsAssert(res == VK_SUCCESS);

  // Initialize queue family index
  uint32_t queueCount;

  vkGetPhysicalDeviceQueueFamilyProperties(mGPU, &queueCount, nullptr);
  rsAssert(queueCount > 0);

  std::vector<VkQueueFamilyProperties> queueProps(queueCount);

  vkGetPhysicalDeviceQueueFamilyProperties(mGPU, &queueCount,
                                           queueProps.data());
  rsAssert(queueCount > 0);

  uint32_t queueFamilyIndex = UINT_MAX;
  bool found = false;
  for (unsigned int i = 0; i < queueCount; i++) {
    if (queueProps[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
      queueFamilyIndex = i;
      found = true;
      break;
    }
  }

  rsAssert(found);

  // Create a device queue

  vkGetDeviceQueue(mDevice, queueFamilyIndex, 0, &mQueue);

  // Create command pool

  VkCommandPoolCreateInfo cmd_pool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .pNext = nullptr,
      .queueFamilyIndex = queueFamilyIndex,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
  };

  res = vkCreateCommandPool(mDevice, &cmd_pool_info, nullptr, &mCmdPool);
  rsAssert(res == VK_SUCCESS);
}

bool RSoVContext::MemoryTypeFromProperties(uint32_t typeBits,
                                           VkFlags requirements_mask,
                                           uint32_t* typeIndex) {
  for (uint32_t i = 0; i < 32; i++) {
    if ((typeBits & 1) == 1) {
      const uint32_t prop = mMemoryProperties.memoryTypes[i].propertyFlags;
      if ((prop & requirements_mask) == requirements_mask) {
        *typeIndex = i;
        return true;
      }
    }
    typeBits >>= 1;
  }

  return false;
}

RSoVContext::RSoVContext() {
  char engineName[] = "RSoV";

  Initialize(engineName);
}

RSoVContext::~RSoVContext() {
  vkDestroyCommandPool(mDevice, mCmdPool, nullptr);
  vkDestroyDevice(mDevice, nullptr);
  vkDestroyInstance(mInstance, nullptr);
}

RSoVContext* RSoVContext::create() {
  std::call_once(mInitFlag, []() { mContext = new RSoVContext(); });
  return mContext;
}

}  // namespace rsov
}  // namespace renderscript
}  // namespace android
