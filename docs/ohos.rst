OpenHarmony native buffers
==========================

The OHOS EGL platform uses Zink and the native Vulkan window layer. Managed
HDI image importing is selected separately with
``-Degl-ohos-native-buffer=true`` and ``-Dplatforms=ohos``. The option defaults
to false and requires libdrm and the ``qcom-native-buffer`` dependency, containing
the matching native HDI headers and complete managed-buffer metadata archive.
It does not select a DRM/KMS window system or install a device driver. GLES
versions are negotiated from the selected driver; the platform does not force
a version override.

Use the native HDI ``BufferHandle`` ABI. The public SDK window handle has a
different layout and cannot substitute for these headers. The Vulkan provider,
native allocator and Surface implementation must implement the same managed
buffer and ownership protocol.

``EGL_OHOS_image_native_buffer`` is advertised only when the selected Gallium
screen reports DMA-BUF importing. Create these images with
``EGL_NATIVE_BUFFER_OHOS``, ``EGL_NO_CONTEXT`` and an ``OHNativeWindowBuffer``.
``EGL_IMAGE_PRESERVED_KHR`` accepts ``EGL_TRUE`` or ``EGL_FALSE``; other image
attributes cannot override the buffer's layout or protection.

The importer retains validated pixel and shared metadata descriptors. It uses
the allocator's reported format, modifier, pitch and offset, and rejects foreign
or layout-only handles. The initial path accepts single-plane RGB layouts.
Multiplanar color conversion and protected images return ``EGL_BAD_MATCH``;
there is no guessed color metadata, linear-modifier fallback or protection
fallback. The retained descriptors are released with the DRI image, including
failure to allocate the EGL image wrapper.

Image creation does not begin or end a CPU session and does not wait for an
application's producer fence. Native queue users must complete their normal
ownership transfer and wait for the producer fence before accessing the image.
Direct CPU users must wait for prior device work before beginning CPU access,
then end that access before submitting new GL work. These requirements apply
to every use, including cached images; importing once is not synchronization
for later writes. An importer must not end an asynchronous producer's CPU
session before its completion fence.
