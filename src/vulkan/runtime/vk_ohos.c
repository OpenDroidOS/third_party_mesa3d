/*
 * Copyright © 2022 Intel Corporation
 * Copyright 2026 OpenDroidOS
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

/* The native layer uses the same sync-file transfer rules as Mesa's Android
 * native-buffer path. Keep synchronization independent of the buffer layout.
 */
#include "vk_ohos.h"

#include "vk_common_entrypoints.h"
#include "vk_device.h"
#include "vk_log.h"
#include "vk_queue.h"
#include "vk_util.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

VKAPI_ATTR VkResult VKAPI_CALL
vk_common_AcquireImageOHOS(VkDevice _device, VkImage image,
                           int nativeFenceFd, VkSemaphore semaphore,
                           VkFence fence)
{
   VK_FROM_HANDLE(vk_device, device, _device);
   VkResult result = VK_SUCCESS;
   int semaphore_fd = -1, fence_fd = -1;

   /* Acquire owns the incoming fd even when importing it fails. The Vulkan
    * external-fd imports consume their fd only on success.
    */
   if (nativeFenceFd >= 0) {
      if (semaphore != VK_NULL_HANDLE && fence != VK_NULL_HANDLE) {
         semaphore_fd = nativeFenceFd;
         do {
            fence_fd = fcntl(nativeFenceFd, F_DUPFD_CLOEXEC, 0);
         } while (fence_fd < 0 && errno == EINTR);
         if (fence_fd < 0) {
            result = errno == EMFILE || errno == ENFILE ?
               VK_ERROR_TOO_MANY_OBJECTS : VK_ERROR_OUT_OF_HOST_MEMORY;
            goto finish;
         }
      } else if (semaphore != VK_NULL_HANDLE) {
         semaphore_fd = nativeFenceFd;
      } else if (fence != VK_NULL_HANDLE) {
         fence_fd = nativeFenceFd;
      } else {
         close(nativeFenceFd);
      }
   }

   if (semaphore != VK_NULL_HANDLE) {
      const VkImportSemaphoreFdInfoKHR info = {
         .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
         .semaphore = semaphore,
         .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
         .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
         .fd = semaphore_fd,
      };
      result = device->dispatch_table.ImportSemaphoreFdKHR ?
         device->dispatch_table.ImportSemaphoreFdKHR(_device, &info) :
         VK_ERROR_FEATURE_NOT_PRESENT;
      if (result == VK_SUCCESS)
         semaphore_fd = -1;
   }

   if (result == VK_SUCCESS && fence != VK_NULL_HANDLE) {
      const VkImportFenceFdInfoKHR info = {
         .sType = VK_STRUCTURE_TYPE_IMPORT_FENCE_FD_INFO_KHR,
         .fence = fence,
         .flags = VK_FENCE_IMPORT_TEMPORARY_BIT,
         .handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT,
         .fd = fence_fd,
      };
      result = device->dispatch_table.ImportFenceFdKHR ?
         device->dispatch_table.ImportFenceFdKHR(_device, &info) :
         VK_ERROR_FEATURE_NOT_PRESENT;
      if (result == VK_SUCCESS)
         fence_fd = -1;
   }

finish:
   if (semaphore_fd >= 0)
      close(semaphore_fd);
   if (fence_fd >= 0)
      close(fence_fd);
   return result;
}

static VkResult
export_release_fence(struct vk_queue *queue, int *fd)
{
   struct vk_device *device = queue->base.device;
   const VkSemaphoreGetFdInfoKHR info = {
      .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
      .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
      .semaphore = queue->ohos_release_semaphore,
   };
   int exported_fd = -1;
   VkResult result = device->dispatch_table.GetSemaphoreFdKHR(
      vk_device_to_handle(device), &info, &exported_fd);
   if (result == VK_SUCCESS) {
      queue->ohos_release_pending = false;
      *fd = exported_fd;
   }
   return result;
}

VKAPI_ATTR VkResult VKAPI_CALL
vk_common_QueueSignalReleaseImageOHOS(VkQueue _queue,
                                      uint32_t waitSemaphoreCount,
                                      const VkSemaphore *pWaitSemaphores,
                                      VkImage image, int *pNativeFenceFd)
{
   VK_FROM_HANDLE(vk_queue, queue, _queue);
   struct vk_device *device = queue->base.device;
   VkResult result;

   *pNativeFenceFd = -1;
   if (!device->dispatch_table.CreateSemaphore ||
       !device->dispatch_table.GetSemaphoreFdKHR ||
       !device->dispatch_table.QueueSubmit)
      return VK_ERROR_FEATURE_NOT_PRESENT;

   STACK_ARRAY(VkPipelineStageFlags, stages, waitSemaphoreCount);
   if (stages == NULL)
      return vk_error(device, VK_ERROR_OUT_OF_HOST_MEMORY);
   for (uint32_t i = 0; i < waitSemaphoreCount; i++)
      stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

   /* A failed export did not consume the previous binary payload. Export it
    * before the next submission rather than signaling the same payload twice.
    */
   if (queue->ohos_release_pending) {
      int previous_fd = -1;
      result = export_release_fence(queue, &previous_fd);
      if (result != VK_SUCCESS)
         goto finish;
      if (previous_fd >= 0)
         close(previous_fd);
   }

   if (queue->ohos_release_semaphore == VK_NULL_HANDLE) {
      const VkExportSemaphoreCreateInfo export_info = {
         .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
         .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
      };
      const VkSemaphoreCreateInfo create_info = {
         .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
         .pNext = &export_info,
      };
      result = device->dispatch_table.CreateSemaphore(
         vk_device_to_handle(device), &create_info, NULL,
         &queue->ohos_release_semaphore);
      if (result != VK_SUCCESS)
         goto finish;
   }

   const VkSubmitInfo submit = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .waitSemaphoreCount = waitSemaphoreCount,
      .pWaitSemaphores = pWaitSemaphores,
      .pWaitDstStageMask = stages,
      .signalSemaphoreCount = 1,
      .pSignalSemaphores = &queue->ohos_release_semaphore,
   };
   result = device->dispatch_table.QueueSubmit(_queue, 1, &submit, VK_NULL_HANDLE);
   if (result != VK_SUCCESS)
      goto finish;
   queue->ohos_release_pending = true;
   result = export_release_fence(queue, pNativeFenceFd);

finish:
   STACK_ARRAY_FINISH(stages);
   return result;
}

void
vk_ohos_queue_finish(struct vk_queue *queue)
{
   if (queue->ohos_release_semaphore != VK_NULL_HANDLE) {
      struct vk_device *device = queue->base.device;
      device->dispatch_table.DestroySemaphore(vk_device_to_handle(device),
                                              queue->ohos_release_semaphore, NULL);
      queue->ohos_release_semaphore = VK_NULL_HANDLE;
      queue->ohos_release_pending = false;
   }
}
