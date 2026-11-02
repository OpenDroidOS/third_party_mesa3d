/* Copyright © 2022 Intel Corporation
 * Copyright 2026 OpenDroidOS
 * SPDX-License-Identifier: MIT
 *
 * Native memory import follows Mesa's existing dedicated DMA-BUF import path.
 */
#include "tu_ohos.h"
#include "tu_device.h"
#include "tu_image.h"
#include "tu_entrypoints.h"
#include "vk_physical_device.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <unistd.h>

/* The native display and Adreno register headers both declare ROTATE_* enums. */
namespace native_display {
#include <display_type.h>
}

static uint32_t
image_drm_format(VkFormat format)
{
   switch (format) {
   case VK_FORMAT_R8G8B8A8_UNORM:
   case VK_FORMAT_R8G8B8A8_SRGB: return DRM_FORMAT_ABGR8888;
   case VK_FORMAT_B8G8R8A8_UNORM:
   case VK_FORMAT_B8G8R8A8_SRGB: return DRM_FORMAT_ARGB8888;
   case VK_FORMAT_R5G6B5_UNORM_PACK16: return DRM_FORMAT_RGB565;
   case VK_FORMAT_B5G6R5_UNORM_PACK16: return DRM_FORMAT_BGR565;
   case VK_FORMAT_R8G8B8_UNORM:
   case VK_FORMAT_R8G8B8_SRGB: return DRM_FORMAT_BGR888;
   case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return DRM_FORMAT_ABGR2101010;
   case VK_FORMAT_R16G16B16A16_SFLOAT: return DRM_FORMAT_ABGR16161616F;
   case VK_FORMAT_G8_B8R8_2PLANE_420_UNORM: return DRM_FORMAT_NV12;
   case VK_FORMAT_G8_B8_R8_3PLANE_420_UNORM: return DRM_FORMAT_YUV420;
   case VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16: return DRM_FORMAT_P010;
   default: return 0;
   }
}

VkResult
tu_ohos_buffer_layout(struct tu_device *device, const VkImageCreateInfo *info,
                       const BufferHandle *handle, uint64_t *modifier,
                       VkSubresourceLayout *planes, struct tu_ohos_buffer *buffer)
{
   QcomBufferLayout layout;
   if (qcom_buffer_read_layout(handle, &layout) != 0 ||
       info->imageType != VK_IMAGE_TYPE_2D || info->extent.depth != 1 ||
       (info->tiling != VK_IMAGE_TILING_OPTIMAL && info->tiling != VK_IMAGE_TILING_LINEAR) ||
       info->mipLevels != 1 || info->arrayLayers != 1 ||
       info->samples != VK_SAMPLE_COUNT_1_BIT ||
       (info->flags & (VK_IMAGE_CREATE_SPARSE_BINDING_BIT |
                       VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT |
                       VK_IMAGE_CREATE_SPARSE_ALIASED_BIT |
                       VK_IMAGE_CREATE_DISJOINT_BIT | VK_IMAGE_CREATE_PROTECTED_BIT)) ||
       info->extent.width != (uint32_t)handle->width ||
       info->extent.height != (uint32_t)handle->height ||
       layout.drm_format != image_drm_format(info->format) ||
       layout.plane_count != tu6_plane_count(info->format) ||
       layout.plane_count > TU_MAX_PLANE_COUNT ||
       (layout.modifier != DRM_FORMAT_MOD_LINEAR &&
        layout.modifier != DRM_FORMAT_MOD_QCOM_COMPRESSED))
      return vk_error(device, VK_ERROR_INVALID_EXTERNAL_HANDLE);

   VkPhysicalDeviceImageFormatInfo2 query = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
      .format = info->format,
      .type = info->imageType,
      .tiling = info->tiling,
      .usage = info->usage,
      .flags = info->flags,
   };
   VkImageFormatProperties2 properties = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
   };
   VkResult result = device->vk.physical->dispatch_table.GetPhysicalDeviceImageFormatProperties2(
      vk_physical_device_to_handle(device->vk.physical), &query, &properties);
   if (result != VK_SUCCESS)
      return result;
   if (info->extent.width > properties.imageFormatProperties.maxExtent.width ||
       info->extent.height > properties.imageFormatProperties.maxExtent.height ||
       (layout.plane_count > 1 && ((info->extent.width & 1) || (info->extent.height & 1))))
      return vk_error(device, VK_ERROR_INVALID_EXTERNAL_HANDLE);

   memset(planes, 0, TU_MAX_PLANE_COUNT * sizeof(*planes));
   *modifier = layout.modifier;
   for (uint32_t i = 0; i < layout.plane_count; i++) {
      uint32_t width = vk_format_get_plane_width(info->format, i, info->extent.width);
      uint32_t height = vk_format_get_plane_height(info->format, i, info->extent.height);
      uint32_t minimum_pitch = util_format_get_stride(tu6_plane_format(info->format, i), width);
      if (layout.planes[i].offset > UINT32_MAX ||
          layout.planes[i].row_pitch < minimum_pitch ||
          layout.planes[i].size < layout.planes[i].row_pitch * height)
         return vk_error(device, VK_ERROR_INVALID_EXTERNAL_HANDLE);
      planes[i].offset = layout.planes[i].offset;
      planes[i].rowPitch = layout.planes[i].row_pitch;
   }
   buffer->fd = handle->fd;
   buffer->layout = layout;
   return VK_SUCCESS;
}

VkResult
tu_ohos_import_buffer(struct tu_device *device, struct tu_image *image,
                       const struct tu_ohos_buffer *buffer,
                       const VkAllocationCallbacks *alloc)
{
   const QcomBufferLayout &layout = buffer->layout;
   if (layout.plane_count > TU_MAX_PLANE_COUNT ||
       layout.plane_count != tu6_plane_count(image->vk.format))
      return vk_error(device, VK_ERROR_INVALID_EXTERNAL_HANDLE);
   for (uint32_t i = 0; i < layout.plane_count; i++) {
      if (image->layout[i].size > layout.planes[i].offset + layout.planes[i].size ||
          (layout.modifier == DRM_FORMAT_MOD_QCOM_COMPRESSED && !image->layout[i].ubwc))
         return vk_error(device, VK_ERROR_INVALID_EXTERNAL_HANDLE);
   }

   VkDevice dev = tu_device_to_handle(device);
   VkMemoryRequirements requirements;
   device->vk.dispatch_table.GetImageMemoryRequirements(dev, tu_image_to_handle(image), &requirements);
   if (requirements.size > layout.allocation_size)
      return vk_error(device, VK_ERROR_INVALID_EXTERNAL_HANDLE);

   int fd;
   do {
      fd = fcntl(buffer->fd, F_DUPFD_CLOEXEC, 0);
   } while (fd < 0 && errno == EINTR);
   if (fd < 0)
      return vk_error(device, errno == EMFILE || errno == ENFILE ?
                                VK_ERROR_TOO_MANY_OBJECTS : VK_ERROR_OUT_OF_HOST_MEMORY);
   off_t size = lseek(fd, 0, SEEK_END);
   if (size < 0 || (uint64_t)size != layout.allocation_size) {
      close(fd);
      return vk_error(device, VK_ERROR_INVALID_EXTERNAL_HANDLE);
   }

   VkMemoryFdPropertiesKHR fd_properties = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
   };
   VkResult result = device->vk.dispatch_table.GetMemoryFdPropertiesKHR(
      dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fd_properties);
   uint32_t types = fd_properties.memoryTypeBits & requirements.memoryTypeBits;
   if (result != VK_SUCCESS || types == 0) {
      close(fd);
      return result != VK_SUCCESS ? result : vk_error(device, VK_ERROR_INVALID_EXTERNAL_HANDLE);
   }
   VkMemoryDedicatedAllocateInfo dedicated = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
      .image = tu_image_to_handle(image),
   };
   VkImportMemoryFdInfoKHR import = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
      .pNext = &dedicated,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
      .fd = fd,
   };
   VkMemoryAllocateInfo allocation = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &import,
      .allocationSize = layout.allocation_size,
      .memoryTypeIndex = (uint32_t)__builtin_ctz(types),
   };
   VkDeviceMemory memory = VK_NULL_HANDLE;
   result = device->vk.dispatch_table.AllocateMemory(dev, &allocation, alloc, &memory);
   if (result != VK_SUCCESS) {
      close(fd); /* A failed Vulkan import did not consume the duplicate. */
      return result;
   }
   image->ohos_memory = memory;
   VkBindImageMemoryInfo bind = {
      .sType = VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO,
      .image = tu_image_to_handle(image),
      .memory = image->ohos_memory,
   };
   result = device->vk.dispatch_table.BindImageMemory2(dev, 1, &bind);
   if (result != VK_SUCCESS)
      tu_ohos_free_buffer(device, image, alloc);
   return result;
}

void
tu_ohos_free_buffer(struct tu_device *device, struct tu_image *image,
                     const VkAllocationCallbacks *alloc)
{
   if (image->ohos_memory != VK_NULL_HANDLE) {
      device->vk.dispatch_table.FreeMemory(tu_device_to_handle(device), image->ohos_memory, alloc);
      image->ohos_memory = VK_NULL_HANDLE;
   }
}

VKAPI_ATTR VkResult VKAPI_CALL
tu_GetSwapchainGrallocUsageOHOS(VkDevice _device, VkFormat format,
                                VkImageUsageFlags imageUsage, uint64_t *grallocUsage)
{
   VK_FROM_HANDLE(tu_device, device, _device);
   if (image_drm_format(format) == 0)
      return VK_ERROR_FORMAT_NOT_SUPPORTED;
   VkPhysicalDeviceImageFormatInfo2 info = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
      .format = format,
      .type = VK_IMAGE_TYPE_2D,
      .tiling = VK_IMAGE_TILING_OPTIMAL,
      .usage = imageUsage,
   };
   VkImageFormatProperties2 properties = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
   };
   VkResult result = device->vk.physical->dispatch_table.GetPhysicalDeviceImageFormatProperties2(
      vk_physical_device_to_handle(device->vk.physical), &info, &properties);
   if (result != VK_SUCCESS)
      return result;
   uint64_t usage = native_display::HBM_USE_MEM_DMA | native_display::HBM_USE_HW_COMPOSER;
   if (imageUsage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                     VK_IMAGE_USAGE_STORAGE_BIT))
      usage |= native_display::HBM_USE_HW_RENDER;
   if (imageUsage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT))
      usage |= native_display::HBM_USE_HW_TEXTURE;
   *grallocUsage |= usage;
   return VK_SUCCESS;
}
