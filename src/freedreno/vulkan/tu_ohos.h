/* Copyright 2026 OpenDroidOS
 * SPDX-License-Identifier: MIT
 */
#ifndef TU_OHOS_H
#define TU_OHOS_H

#include "tu_common.h"
#include "qcom/native_buffer.h"

struct tu_ohos_buffer {
   int fd;
   const BufferHandle *handle;
   QcomBufferLayout layout;
};

VkResult tu_ohos_buffer_layout(struct tu_device *device,
                               const VkImageCreateInfo *info,
                               const BufferHandle *handle, uint64_t *modifier,
                               VkSubresourceLayout *planes, struct tu_ohos_buffer *buffer);
VkResult tu_ohos_import_buffer(struct tu_device *device, struct tu_image *image,
                               const struct tu_ohos_buffer *buffer,
                               const VkAllocationCallbacks *alloc);
void tu_ohos_free_buffer(struct tu_device *device, struct tu_image *image,
                         const VkAllocationCallbacks *alloc);

#endif
