/* Copyright 2026 OpenDroidOS
 * SPDX-License-Identifier: MIT
 */
#include "platform_ohos_buffer.h"

#include "qcom/buffer_metadata.h"
#include "qcom/native_buffer_export.h"
#include "window.h"

int
ohos_clone_native_buffer(struct NativeWindowBuffer *buffer,
                          BufferHandle **handle, struct QcomBufferLayout *layout)
{
   if (!handle || !layout)
      return -EINVAL;
   *handle = nullptr;
   if (!buffer || OH_NativeWindow_GetNativeObjectMagic(buffer) != NATIVE_OBJECT_MAGIC_WINDOW_BUFFER)
      return -EINVAL;
   const BufferHandle *source = OH_NativeWindow_GetBufferHandleFromNative(buffer);
   if (!source)
      return -EINVAL;
   BufferHandle *retained = nullptr;
   int error = OHOS::Qcom::BufferMetadata::Clone(*source, retained);
   if (error != 0)
      return error;
   error = qcom_buffer_read_layout(retained, layout);
   if (error != 0) {
      OHOS::Qcom::FreeNativeBuffer(retained);
      return error;
   }
   *handle = retained;
   return 0;
}

void
ohos_destroy_native_buffer(void *handle)
{
   OHOS::Qcom::FreeNativeBuffer(static_cast<BufferHandle *>(handle));
}
