/* TODO */

#include <stdlib.h>
#include <string.h>

#include <vulkan/vulkan.h>

#include "dri_util.h"
#include "egl_dri2.h"
#include "kopper_interface.h"

#include "display_type.h"
#include "external_window.h"

#ifdef HAVE_OHOS_NATIVE_BUFFER
#include "platform_ohos_buffer.h"
#endif

static _EGLSurface *
ohos_create_surface(_EGLDisplay *disp, EGLint type, _EGLConfig *conf,
                    void *native_window, const EGLint *attrib_list)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   struct dri2_egl_config *dri2_conf = dri2_egl_config(conf);
   struct dri2_egl_surface *dri2_surf;
   struct NativeWindow *window = native_window;
   const struct dri_config *config;

   dri2_surf = calloc(1, sizeof *dri2_surf);
   if (!dri2_surf) {
      _eglError(EGL_BAD_ALLOC, "ohos_create_surface");
      return NULL;
   }

   if (!dri2_init_surface(&dri2_surf->base, disp, type, conf, attrib_list,
                          false, native_window))
      goto cleanup_surface;

   if (type == EGL_WINDOW_BIT) {
      int format = 0;

      OH_NativeWindow_NativeWindowHandleOpt(native_window, GET_FORMAT, &format);
      if (format < 0) {
         _eglError(EGL_BAD_NATIVE_WINDOW, "ohos_create_surface");
         goto cleanup_surface;
      }

      if (format != dri2_conf->base.NativeVisualID) {
         _eglLog(_EGL_WARNING, "Native format mismatch: 0x%x != 0x%x", format,
                 dri2_conf->base.NativeVisualID);
      }

      OH_NativeWindow_NativeWindowHandleOpt(native_window, GET_BUFFER_GEOMETRY,
                                            &dri2_surf->base.Height,
                                            &dri2_surf->base.Width);

      dri2_surf->window = window;
   }

   config = dri2_get_dri_config(dri2_conf, type, dri2_surf->base.GLColorspace);
   if (!config) {
      _eglError(EGL_BAD_MATCH,
                "Unsupported surfacetype/colorspace configuration");
      goto cleanup_surface;
   }

   if (!dri2_create_drawable(dri2_dpy, config, dri2_surf, dri2_surf))
      goto cleanup_surface;

   if (window)
      OH_NativeWindow_NativeObjectReference(window);

   return &dri2_surf->base;

cleanup_surface:
   free(dri2_surf);
   return NULL;
}

static EGLBoolean
device_destroy_surface(_EGLDisplay *disp, _EGLSurface *surf)
{
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(surf);

   driDestroyDrawable(dri2_surf->dri_drawable);

   if (dri2_surf->window)
      OH_NativeWindow_NativeObjectUnreference(dri2_surf->window);

   dri2_fini_surface(surf);
   free(dri2_surf);
   return EGL_TRUE;
}

static _EGLSurface *
ohos_create_window_surface(_EGLDisplay *disp, _EGLConfig *conf,
                           void *native_window, const EGLint *attrib_list)
{
   return ohos_create_surface(disp, EGL_WINDOW_BIT, conf, native_window,
                              attrib_list);
}

static _EGLSurface *
ohos_create_pbuffer_surface(_EGLDisplay *disp, _EGLConfig *conf,
                            const EGLint *attrib_list)
{
   return ohos_create_surface(disp, EGL_PBUFFER_BIT, conf, NULL, attrib_list);
}

static EGLBoolean
ohos_swap_buffers(_EGLDisplay *disp, _EGLSurface *draw)
{
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(draw);

   driSwapBuffers(dri2_surf->dri_drawable);

   return EGL_TRUE;
}

#ifdef HAVE_OHOS_NATIVE_BUFFER
static _EGLImage *
ohos_create_native_image(_EGLDisplay *disp, _EGLContext *ctx,
                          EGLClientBuffer buffer, const EGLint *attrib_list)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   if (!disp->Extensions.OHOS_image_native_buffer) {
      _eglError(EGL_BAD_PARAMETER, "Native HDI image importing is unavailable");
      return NULL;
   }
   if (ctx) {
      _eglError(EGL_BAD_CONTEXT, "Native HDI images require EGL_NO_CONTEXT");
      return NULL;
   }
   /* The native buffer describes its dimensions and format. Only preservation
    * is meaningful here; texture, DMA-BUF and protected-image attributes do not
    * override that contract.
    */
   if (attrib_list) {
      for (unsigned i = 0; attrib_list[i] != EGL_NONE; i += 2) {
         if (attrib_list[i] != EGL_IMAGE_PRESERVED_KHR ||
             (attrib_list[i + 1] != EGL_TRUE && attrib_list[i + 1] != EGL_FALSE)) {
            _eglError(EGL_BAD_PARAMETER, "Unsupported native HDI image attribute");
            return NULL;
         }
      }
   }

   BufferHandle *handle = NULL;
   struct QcomBufferLayout layout;
   int result = ohos_clone_native_buffer(buffer, &handle, &layout);
   if (result != 0) {
      EGLint error = result == -ENOMEM || result == -EMFILE || result == -ENFILE ?
                        EGL_BAD_ALLOC : result == -EAGAIN ? EGL_BAD_ACCESS : EGL_BAD_PARAMETER;
      _eglError(error, "Invalid or unavailable managed native HDI buffer");
      return NULL;
   }

   /* Multiplanar color conversion needs the buffer's actual color metadata.
    * Keep that unsupported path explicit rather than inventing YUV defaults.
    */
   if (layout.plane_count != 1 || handle->usage & HBM_USE_PROTECTED ||
       layout.planes[0].offset > INT_MAX) {
      ohos_destroy_native_buffer(handle);
      _eglError(EGL_BAD_MATCH, "Unsupported native HDI image layout or protection");
      return NULL;
   }
   int stride = layout.planes[0].row_pitch;
   int offset = layout.planes[0].offset;
   unsigned error = __DRI_IMAGE_ERROR_SUCCESS;
   struct dri_image *image = dri2_from_dma_bufs(
      dri2_dpy->dri_screen_render_gpu, handle->width, handle->height,
      layout.drm_format, layout.modifier, &handle->fd, 1, &stride, &offset,
      0, 0, 0, 0, 0, &error, handle);
   if (!image) {
      ohos_destroy_native_buffer(handle);
      _eglError(error == __DRI_IMAGE_ERROR_BAD_ALLOC ? EGL_BAD_ALLOC : EGL_BAD_MATCH,
                "Native HDI DMA-BUF image import failed");
      return NULL;
   }
   _EGLImage *egl_image = dri2_create_image_from_dri(disp, image);
   if (!egl_image)
      dri2_destroy_image(image);
   return egl_image;
}
#endif

static _EGLImage *
ohos_create_image(_EGLDisplay *disp, _EGLContext *ctx, EGLenum target,
                   EGLClientBuffer buffer, const EGLint *attrib_list)
{
#ifdef HAVE_OHOS_NATIVE_BUFFER
   if (target == EGL_NATIVE_BUFFER_OHOS)
      return ohos_create_native_image(disp, ctx, buffer, attrib_list);
#endif
   return dri2_create_image_khr(disp, ctx, target, buffer, attrib_list);
}

static const struct dri2_egl_display_vtbl dri2_ohos_display_vtbl = {
   .create_window_surface = ohos_create_window_surface,
   .create_pbuffer_surface = ohos_create_pbuffer_surface,
   .destroy_surface = device_destroy_surface,
   .create_image = ohos_create_image,
   .get_dri_drawable = dri2_surface_get_dri_drawable,
   .swap_buffers = ohos_swap_buffers,
};

static void
kopperSetSurfaceCreateInfo(void *_draw, struct kopper_loader_info *out)
{
   struct dri2_egl_surface *dri2_surf = _draw;
   VkSurfaceCreateInfoOHOS *sci = (VkSurfaceCreateInfoOHOS *)&out->bos;

   sci->sType = VK_STRUCTURE_TYPE_SURFACE_CREATE_INFO_OHOS;
   sci->pNext = NULL;
   sci->flags = 0;
   sci->window = dri2_surf->window;

   out->present_opaque = dri2_surf->base.PresentOpaque;
   /* convert to vulkan constants */
   switch (dri2_surf->base.CompressionRate) {
   case EGL_SURFACE_COMPRESSION_FIXED_RATE_NONE_EXT:
      out->compression = 0;
      break;
   case EGL_SURFACE_COMPRESSION_FIXED_RATE_DEFAULT_EXT:
      out->compression = UINT32_MAX;
      break;
#define EGL_VK_COMP(NUM)                                                       \
   case EGL_SURFACE_COMPRESSION_FIXED_RATE_##NUM##BPC_EXT:                     \
      out->compression = VK_IMAGE_COMPRESSION_FIXED_RATE_##NUM##BPC_BIT_EXT;   \
      break
      EGL_VK_COMP(1);
      EGL_VK_COMP(2);
      EGL_VK_COMP(3);
      EGL_VK_COMP(4);
      EGL_VK_COMP(5);
      EGL_VK_COMP(6);
      EGL_VK_COMP(7);
      EGL_VK_COMP(8);
      EGL_VK_COMP(9);
      EGL_VK_COMP(10);
      EGL_VK_COMP(11);
      EGL_VK_COMP(12);
#undef EGL_VK_COMP
   default:
      unreachable("unknown compression rate");
   }
}

static const __DRIkopperLoaderExtension kopper_loader_extension = {
   .base = {__DRI_KOPPER_LOADER, 1},
   .SetSurfaceCreateInfo = kopperSetSurfaceCreateInfo,
};

static void
ohos_drawable_info(struct dri_drawable *draw, int *x, int *y, int *w, int *h,
                   void *loaderPrivate)
{
   struct dri2_egl_surface *dri2_surf = loaderPrivate;

   if (dri2_surf->base.Type == EGL_WINDOW_BIT && dri2_surf->window) {
      OH_NativeWindow_NativeWindowHandleOpt(dri2_surf->window, GET_BUFFER_GEOMETRY,
          &dri2_surf->base.Height, &dri2_surf->base.Width);
   }

   *x = *y = 0;
   *w = dri2_surf->base.Width;
   *h = dri2_surf->base.Height;
}

static const __DRIswrastLoaderExtension swrast_loader_extension = {
   .base = {__DRI_SWRAST_LOADER, 1},
   .getDrawableInfo = ohos_drawable_info,
};

static unsigned
ohos_get_capability(void *loaderPrivate, enum dri_loader_cap cap)
{
   /* Note: loaderPrivate is _EGLDisplay* */
   switch (cap) {
   case DRI_LOADER_CAP_RGBA_ORDERING:
      return 1;
   default:
      return 0;
   }
}

static const __DRIdri2LoaderExtension dri2_loader_extension = {
#ifdef HAVE_OHOS_NATIVE_BUFFER
   .base = {__DRI_DRI2_LOADER, 5},
   .destroyLoaderImageState = ohos_destroy_native_buffer,
#else
   .base = {__DRI_DRI2_LOADER, 4},
#endif
   .getCapability = ohos_get_capability,
};

static const __DRIextension *image_loader_extensions[] = {
   &image_lookup_extension.base,
   &kopper_loader_extension.base,
   &swrast_loader_extension.base,
   &dri2_loader_extension.base,
   NULL,
};

static bool
load_zink_kopper(_EGLDisplay *disp)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);

   dri2_dpy->fd_render_gpu = -1;
   dri2_dpy->fd_display_gpu = -1;
   dri2_dpy->driver_name = strdup("zink");
   if (!dri2_dpy->driver_name)
      return false;

   if (!dri2_load_driver(disp)) {
      free(dri2_dpy->driver_name);
      dri2_dpy->driver_name = NULL;
      return false;
   }

   dri2_dpy->loader_extensions = image_loader_extensions;

   return true;
}

static void
ohos_add_configs_for_visuals(_EGLDisplay *disp)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(disp);
   static const struct {
      int hal_format;
      enum pipe_format pipe_format;
   } visuals[] = {
      {PIXEL_FMT_RGBA_8888, PIPE_FORMAT_RGBA8888_UNORM},
      {PIXEL_FMT_RGBX_8888, PIPE_FORMAT_BGRX8888_UNORM},
      {PIXEL_FMT_RGB_565, PIPE_FORMAT_B5G6R5_UNORM},
      /* This must be after HAL_PIXEL_FMT_RGBA_8888, we only keep BGRA
       * visual if it turns out RGBA visual is not available.
       */
      {PIXEL_FMT_BGRA_8888, PIPE_FORMAT_BGRA8888_UNORM},
   };

   unsigned int format_count[ARRAY_SIZE(visuals)] = {0};

   bool has_rgba = false;
   for (int i = 0; i < ARRAY_SIZE(visuals); i++) {
      if (visuals[i].hal_format == PIXEL_FMT_BGRA_8888 && has_rgba)
         continue;
      for (int j = 0; dri2_dpy->driver_configs[j]; j++) {
         const struct gl_config *gl_config =
            (struct gl_config *)dri2_dpy->driver_configs[j];

         /* Rather than have duplicate table entries for _SRGB formats, just
          * use the linear version of the format for the comparision:
          */
         enum pipe_format linear_format =
            util_format_linear(gl_config->color_format);
         if (linear_format != visuals[i].pipe_format)
            continue;

         const EGLint surface_type = EGL_WINDOW_BIT | EGL_PBUFFER_BIT;
         const EGLint config_attrs[] = {
            EGL_NATIVE_VISUAL_ID, visuals[i].hal_format, EGL_NATIVE_VISUAL_TYPE,
            visuals[i].hal_format, EGL_NONE};

         _eglLog(_EGL_DEBUG, "calling dri2_add_config for %x",
                 visuals[i].hal_format);
         struct dri2_egl_config *dri2_conf = dri2_add_config(
            disp, dri2_dpy->driver_configs[j], surface_type, config_attrs);
         if (dri2_conf)
            format_count[i]++;
      }

      if (visuals[i].hal_format == PIXEL_FMT_RGBA_8888 && format_count[i])
         has_rgba = true;
   }

   for (int i = 0; i < ARRAY_SIZE(format_count); i++) {
      if (!format_count[i]) {
         _eglLog(_EGL_DEBUG, "No DRI config supports native format 0x%x",
                 visuals[i].hal_format);
      }
   }
}

EGLBoolean
dri2_initialize_ohos(_EGLDisplay *disp)
{
   const char *err;
   struct dri2_egl_display *dri2_dpy = dri2_display_create(disp);
   if (!dri2_dpy)
      return EGL_FALSE;

   err = "DRI2: failed to load driver";
   if (!load_zink_kopper(disp))
      goto cleanup;

   if (!dri2_create_screen(disp)) {
      err = "DRI2: failed to create screen";
      goto cleanup;
   }

   dri2_setup_screen(disp);
#ifdef HAVE_OHOS_NATIVE_BUFFER
   disp->Extensions.OHOS_image_native_buffer = dri2_dpy->has_dmabuf_import;
#endif

   /* Create configs *after* enabling extensions because presence of DRI
    * driver extensions can affect the capabilities of EGLConfigs.
    */
   ohos_add_configs_for_visuals(disp);

   disp->Extensions.KHR_image = EGL_TRUE;

   /* Fill vtbl last to prevent accidentally calling virtual function during
    * initialization.
    */
   dri2_dpy->vtbl = &dri2_ohos_display_vtbl;

   return EGL_TRUE;

cleanup:
   dri2_display_destroy(disp);
   return _eglError(EGL_NOT_INITIALIZED, err);
}
