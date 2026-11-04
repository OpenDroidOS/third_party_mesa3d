/* Copyright 2026 OpenDroidOS
 * SPDX-License-Identifier: MIT
 */
#ifndef PLATFORM_OHOS_BUFFER_H
#define PLATFORM_OHOS_BUFFER_H

#include "qcom/native_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

struct NativeWindowBuffer;

/* Retain pixels and shared metadata. Importing an image does not begin or end a
 * CPU session; callers must complete ownership/fence handoff before GPU access.
 */
int ohos_clone_native_buffer(struct NativeWindowBuffer *buffer,
                             BufferHandle **handle, struct QcomBufferLayout *layout);
void ohos_destroy_native_buffer(void *handle);

#ifdef __cplusplus
}
#endif
#endif
