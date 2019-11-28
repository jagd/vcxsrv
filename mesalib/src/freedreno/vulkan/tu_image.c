/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 *
 * based in part on anv driver which is:
 * Copyright © 2015 Intel Corporation
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
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "tu_private.h"

#include "util/debug.h"
#include "util/u_atomic.h"
#include "vk_format.h"
#include "vk_util.h"
#include "drm-uapi/drm_fourcc.h"

static inline bool
image_level_linear(struct tu_image *image, int level, bool ubwc)
{
   unsigned w = u_minify(image->extent.width, level);
   /* all levels are tiled/compressed with UBWC */
   return ubwc ? false : (w < 16);
}

enum a6xx_tile_mode
tu6_get_image_tile_mode(struct tu_image *image, int level)
{
   if (image_level_linear(image, level, !!image->ubwc_size))
      return TILE6_LINEAR;
   else
      return image->tile_mode;
}

/* indexed by cpp, including msaa 2x and 4x: */
static const struct {
   uint8_t pitchalign;
   uint8_t heightalign;
   uint8_t ubwc_blockwidth;
   uint8_t ubwc_blockheight;
} tile_alignment[] = {
/* TODO:
 * cpp=1 UBWC needs testing at larger texture sizes
 * missing UBWC blockwidth/blockheight for npot+64 cpp
 * missing 96/128 CPP for 8x MSAA with 32_32_32/32_32_32_32
 */
   [1]  = { 128, 32, 16, 4 },
   [2]  = { 128, 16, 16, 4 },
   [3]  = {  64, 32 },
   [4]  = {  64, 16, 16, 4 },
   [6]  = {  64, 16 },
   [8]  = {  64, 16, 8, 4, },
   [12] = {  64, 16 },
   [16] = {  64, 16, 4, 4, },
   [24] = {  64, 16 },
   [32] = {  64, 16, 4, 2 },
   [48] = {  64, 16 },
   [64] = {  64, 16 },
   /* special case for r8g8: */
   [0]  = { 64, 32, 16, 4 },
};

static void
setup_slices(struct tu_image *image,
             const VkImageCreateInfo *pCreateInfo,
             bool ubwc_enabled)
{
#define RGB_TILE_WIDTH_ALIGNMENT 64
#define RGB_TILE_HEIGHT_ALIGNMENT 16
#define UBWC_PLANE_SIZE_ALIGNMENT 4096
   VkFormat format = pCreateInfo->format;
   enum util_format_layout layout = vk_format_description(format)->layout;
   uint32_t layer_size = 0;
   uint32_t ubwc_size = 0;
   int ta = image->cpp;

   /* The r8g8 format seems to not play by the normal tiling rules: */
   if (image->cpp == 2 && vk_format_get_nr_components(format) == 2)
      ta = 0;

   for (unsigned level = 0; level < pCreateInfo->mipLevels; level++) {
      struct tu_image_level *slice = &image->levels[level];
      struct tu_image_level *ubwc_slice = &image->ubwc_levels[level];
      uint32_t width = u_minify(pCreateInfo->extent.width, level);
      uint32_t height = u_minify(pCreateInfo->extent.height, level);
      uint32_t depth = u_minify(pCreateInfo->extent.depth, level);
      uint32_t aligned_height = height;
      uint32_t blocks;
      uint32_t pitchalign;

      if (image->tile_mode && !image_level_linear(image, level, ubwc_enabled)) {
         /* tiled levels of 3D textures are rounded up to PoT dimensions: */
         if (pCreateInfo->imageType == VK_IMAGE_TYPE_3D) {
            width = util_next_power_of_two(width);
            height = aligned_height = util_next_power_of_two(height);
         }
         pitchalign = tile_alignment[ta].pitchalign;
         aligned_height = align(aligned_height, tile_alignment[ta].heightalign);
      } else {
         pitchalign = 64;
      }

      /* The blits used for mem<->gmem work at a granularity of
       * 32x32, which can cause faults due to over-fetch on the
       * last level.  The simple solution is to over-allocate a
       * bit the last level to ensure any over-fetch is harmless.
       * The pitch is already sufficiently aligned, but height
       * may not be:
       */
      if (level + 1 == pCreateInfo->mipLevels)
         aligned_height = align(aligned_height, 32);

      if (layout == UTIL_FORMAT_LAYOUT_ASTC)
         slice->pitch =
            util_align_npot(width, pitchalign * vk_format_get_blockwidth(format));
      else
         slice->pitch = align(width, pitchalign);

      slice->offset = layer_size;
      blocks = vk_format_get_block_count(format, slice->pitch, aligned_height);

      /* 1d array and 2d array textures must all have the same layer size
       * for each miplevel on a6xx. 3d textures can have different layer
       * sizes for high levels, but the hw auto-sizer is buggy (or at least
       * different than what this code does), so as soon as the layer size
       * range gets into range, we stop reducing it.
       */
      if (pCreateInfo->imageType == VK_IMAGE_TYPE_3D) {
         if (level < 1 || image->levels[level - 1].size > 0xf000) {
            slice->size = align(blocks * image->cpp, 4096);
         } else {
            slice->size = image->levels[level - 1].size;
         }
      } else {
         slice->size = blocks * image->cpp;
      }

      layer_size += slice->size * depth;
      if (ubwc_enabled) {
         /* with UBWC every level is aligned to 4K */
         layer_size = align(layer_size, 4096);

         uint32_t block_width = tile_alignment[ta].ubwc_blockwidth;
         uint32_t block_height = tile_alignment[ta].ubwc_blockheight;
         uint32_t meta_pitch = align(DIV_ROUND_UP(width, block_width), RGB_TILE_WIDTH_ALIGNMENT);
         uint32_t meta_height = align(DIV_ROUND_UP(height, block_height), RGB_TILE_HEIGHT_ALIGNMENT);

         /* it looks like mipmaps need alignment to power of two
          * TODO: needs testing with large npot textures
          * (needed for the first level?)
          */
         if (pCreateInfo->mipLevels > 1) {
            meta_pitch = util_next_power_of_two(meta_pitch);
            meta_height = util_next_power_of_two(meta_height);
         }

         ubwc_slice->pitch = meta_pitch;
         ubwc_slice->offset = ubwc_size;
         ubwc_size += align(meta_pitch * meta_height, UBWC_PLANE_SIZE_ALIGNMENT);
      }
   }
   image->layer_size = align(layer_size, 4096);

   VkDeviceSize offset = ubwc_size * pCreateInfo->arrayLayers;
   for (unsigned level = 0; level < pCreateInfo->mipLevels; level++)
      image->levels[level].offset += offset;

   image->size = offset + image->layer_size * pCreateInfo->arrayLayers;
   image->ubwc_size = ubwc_size;
}

VkResult
tu_image_create(VkDevice _device,
                const VkImageCreateInfo *pCreateInfo,
                const VkAllocationCallbacks *alloc,
                VkImage *pImage,
                uint64_t modifier)
{
   TU_FROM_HANDLE(tu_device, device, _device);
   struct tu_image *image = NULL;
   assert(pCreateInfo->sType == VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);

   tu_assert(pCreateInfo->mipLevels > 0);
   tu_assert(pCreateInfo->arrayLayers > 0);
   tu_assert(pCreateInfo->samples > 0);
   tu_assert(pCreateInfo->extent.width > 0);
   tu_assert(pCreateInfo->extent.height > 0);
   tu_assert(pCreateInfo->extent.depth > 0);

   image = vk_zalloc2(&device->alloc, alloc, sizeof(*image), 8,
                      VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!image)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   image->type = pCreateInfo->imageType;

   image->vk_format = pCreateInfo->format;
   image->tiling = pCreateInfo->tiling;
   image->usage = pCreateInfo->usage;
   image->flags = pCreateInfo->flags;
   image->extent = pCreateInfo->extent;
   image->level_count = pCreateInfo->mipLevels;
   image->layer_count = pCreateInfo->arrayLayers;
   image->samples = pCreateInfo->samples;
   image->cpp = vk_format_get_blocksize(image->vk_format) * image->samples;

   image->exclusive = pCreateInfo->sharingMode == VK_SHARING_MODE_EXCLUSIVE;
   if (pCreateInfo->sharingMode == VK_SHARING_MODE_CONCURRENT) {
      for (uint32_t i = 0; i < pCreateInfo->queueFamilyIndexCount; ++i)
         if (pCreateInfo->pQueueFamilyIndices[i] ==
             VK_QUEUE_FAMILY_EXTERNAL)
            image->queue_family_mask |= (1u << TU_MAX_QUEUE_FAMILIES) - 1u;
         else
            image->queue_family_mask |=
               1u << pCreateInfo->pQueueFamilyIndices[i];
   }

   image->shareable =
      vk_find_struct_const(pCreateInfo->pNext,
                           EXTERNAL_MEMORY_IMAGE_CREATE_INFO) != NULL;

   image->tile_mode = TILE6_3;
   bool ubwc_enabled = true;

   /* disable tiling when linear is requested and for compressed formats */
   if (pCreateInfo->tiling == VK_IMAGE_TILING_LINEAR ||
       modifier == DRM_FORMAT_MOD_LINEAR ||
       vk_format_is_compressed(image->vk_format)) {
      image->tile_mode = TILE6_LINEAR;
      ubwc_enabled = false;
   }

   /* using UBWC with D24S8 breaks the "stencil read" copy path (why?)
    * (causes any deqp tests that need to check stencil to fail)
    * disable UBWC for this format until we properly support copy aspect masks
    */
   if (image->vk_format == VK_FORMAT_D24_UNORM_S8_UINT)
      ubwc_enabled = false;

   /* UBWC can't be used with E5B9G9R9 */
   if (image->vk_format == VK_FORMAT_E5B9G9R9_UFLOAT_PACK32)
      ubwc_enabled = false;

   if (image->extent.depth > 1) {
      tu_finishme("UBWC with 3D textures");
      ubwc_enabled = false;
   }

   if (!tile_alignment[image->cpp].ubwc_blockwidth) {
      tu_finishme("UBWC for cpp=%d", image->cpp);
      ubwc_enabled = false;
   }

   /* expect UBWC enabled if we asked for it */
   assert(modifier != DRM_FORMAT_MOD_QCOM_COMPRESSED || ubwc_enabled);

   setup_slices(image, pCreateInfo, ubwc_enabled);

   *pImage = tu_image_to_handle(image);

   return VK_SUCCESS;
}

static enum a6xx_tex_fetchsize
tu6_fetchsize(VkFormat format)
{
   if (vk_format_description(format)->layout == UTIL_FORMAT_LAYOUT_ASTC)
      return TFETCH6_16_BYTE;

   switch (vk_format_get_blocksize(format) / vk_format_get_blockwidth(format)) {
   case 1: return TFETCH6_1_BYTE;
   case 2: return TFETCH6_2_BYTE;
   case 4: return TFETCH6_4_BYTE;
   case 8: return TFETCH6_8_BYTE;
   case 16: return TFETCH6_16_BYTE;
   default:
      unreachable("bad block size");
   }
}

static uint32_t
tu6_texswiz(const VkComponentMapping *comps, const unsigned char *fmt_swiz)
{
   unsigned char swiz[4] = {comps->r, comps->g, comps->b, comps->a};
   unsigned char vk_swizzle[] = {
      [VK_COMPONENT_SWIZZLE_ZERO] = A6XX_TEX_ZERO,
      [VK_COMPONENT_SWIZZLE_ONE]  = A6XX_TEX_ONE,
      [VK_COMPONENT_SWIZZLE_R] = A6XX_TEX_X,
      [VK_COMPONENT_SWIZZLE_G] = A6XX_TEX_Y,
      [VK_COMPONENT_SWIZZLE_B] = A6XX_TEX_Z,
      [VK_COMPONENT_SWIZZLE_A] = A6XX_TEX_W,
   };
   for (unsigned i = 0; i < 4; i++) {
      swiz[i] = (swiz[i] == VK_COMPONENT_SWIZZLE_IDENTITY) ? i : vk_swizzle[swiz[i]];
      /* if format has 0/1 in channel, use that (needed for bc1_rgb) */
      if (swiz[i] < 4) {
         switch (fmt_swiz[swiz[i]]) {
         case PIPE_SWIZZLE_0: swiz[i] = A6XX_TEX_ZERO; break;
         case PIPE_SWIZZLE_1: swiz[i] = A6XX_TEX_ONE;  break;
         }
      }
   }

   return A6XX_TEX_CONST_0_SWIZ_X(swiz[0]) |
          A6XX_TEX_CONST_0_SWIZ_Y(swiz[1]) |
          A6XX_TEX_CONST_0_SWIZ_Z(swiz[2]) |
          A6XX_TEX_CONST_0_SWIZ_W(swiz[3]);
}

static enum a6xx_tex_type
tu6_tex_type(VkImageViewType type)
{
   switch (type) {
   default:
   case VK_IMAGE_VIEW_TYPE_1D:
   case VK_IMAGE_VIEW_TYPE_1D_ARRAY:
      return A6XX_TEX_1D;
   case VK_IMAGE_VIEW_TYPE_2D:
   case VK_IMAGE_VIEW_TYPE_2D_ARRAY:
      return A6XX_TEX_2D;
   case VK_IMAGE_VIEW_TYPE_3D:
      return A6XX_TEX_3D;
   case VK_IMAGE_VIEW_TYPE_CUBE:
   case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:
      return A6XX_TEX_CUBE;
   }
}

void
tu_image_view_init(struct tu_image_view *iview,
                   struct tu_device *device,
                   const VkImageViewCreateInfo *pCreateInfo)
{
   TU_FROM_HANDLE(tu_image, image, pCreateInfo->image);
   const VkImageSubresourceRange *range = &pCreateInfo->subresourceRange;

   switch (image->type) {
   case VK_IMAGE_TYPE_1D:
   case VK_IMAGE_TYPE_2D:
      assert(range->baseArrayLayer + tu_get_layerCount(image, range) <=
             image->layer_count);
      break;
   case VK_IMAGE_TYPE_3D:
      assert(range->baseArrayLayer + tu_get_layerCount(image, range) <=
             tu_minify(image->extent.depth, range->baseMipLevel));
      break;
   default:
      unreachable("bad VkImageType");
   }

   iview->image = image;
   iview->type = pCreateInfo->viewType;
   iview->vk_format = pCreateInfo->format;
   iview->aspect_mask = pCreateInfo->subresourceRange.aspectMask;

   if (iview->aspect_mask == VK_IMAGE_ASPECT_STENCIL_BIT) {
      iview->vk_format = vk_format_stencil_only(iview->vk_format);
   } else if (iview->aspect_mask == VK_IMAGE_ASPECT_DEPTH_BIT) {
      iview->vk_format = vk_format_depth_only(iview->vk_format);
   }

   // should we minify?
   iview->extent = image->extent;

   iview->base_layer = range->baseArrayLayer;
   iview->layer_count = tu_get_layerCount(image, range);
   iview->base_mip = range->baseMipLevel;
   iview->level_count = tu_get_levelCount(image, range);

   memset(iview->descriptor, 0, sizeof(iview->descriptor));

   const struct tu_native_format *fmt = tu6_get_native_format(iview->vk_format);
   uint64_t base_addr = tu_image_base(image, iview->base_mip, iview->base_layer);
   uint64_t ubwc_addr = tu_image_ubwc_base(image, iview->base_mip, iview->base_layer);

   uint32_t pitch = tu_image_stride(image, iview->base_mip) / vk_format_get_blockwidth(iview->vk_format);
   enum a6xx_tile_mode tile_mode = tu6_get_image_tile_mode(image, iview->base_mip);
   uint32_t width = u_minify(image->extent.width, iview->base_mip);
   uint32_t height = u_minify(image->extent.height, iview->base_mip);

   iview->descriptor[0] =
      A6XX_TEX_CONST_0_TILE_MODE(tile_mode) |
      COND(vk_format_is_srgb(iview->vk_format), A6XX_TEX_CONST_0_SRGB) |
      A6XX_TEX_CONST_0_FMT(fmt->tex) |
      A6XX_TEX_CONST_0_SAMPLES(tu_msaa_samples(image->samples)) |
      A6XX_TEX_CONST_0_SWAP(image->tile_mode ? WZYX : fmt->swap) |
      tu6_texswiz(&pCreateInfo->components, vk_format_description(iview->vk_format)->swizzle) |
      A6XX_TEX_CONST_0_MIPLVLS(iview->level_count - 1);
   iview->descriptor[1] = A6XX_TEX_CONST_1_WIDTH(width) | A6XX_TEX_CONST_1_HEIGHT(height);
   iview->descriptor[2] =
      A6XX_TEX_CONST_2_FETCHSIZE(tu6_fetchsize(iview->vk_format)) |
      A6XX_TEX_CONST_2_PITCH(pitch) |
      A6XX_TEX_CONST_2_TYPE(tu6_tex_type(pCreateInfo->viewType));
   iview->descriptor[3] = A6XX_TEX_CONST_3_ARRAY_PITCH(tu_layer_size(image, iview->base_mip));
   iview->descriptor[4] = base_addr;
   iview->descriptor[5] = base_addr >> 32;

   if (image->ubwc_size) {
      uint32_t block_width = tile_alignment[image->cpp].ubwc_blockwidth;
      uint32_t block_height = tile_alignment[image->cpp].ubwc_blockheight;

      iview->descriptor[3] |= A6XX_TEX_CONST_3_FLAG | A6XX_TEX_CONST_3_TILE_ALL;
      iview->descriptor[7] = ubwc_addr;
      iview->descriptor[8] = ubwc_addr >> 32;
      iview->descriptor[9] |= A6XX_TEX_CONST_9_FLAG_BUFFER_ARRAY_PITCH(tu_image_ubwc_size(image, iview->base_mip) >> 2);
      iview->descriptor[10] |=
         A6XX_TEX_CONST_10_FLAG_BUFFER_PITCH(tu_image_ubwc_pitch(image, iview->base_mip)) |
         A6XX_TEX_CONST_10_FLAG_BUFFER_LOGW(util_logbase2_ceil(DIV_ROUND_UP(width, block_width))) |
         A6XX_TEX_CONST_10_FLAG_BUFFER_LOGH(util_logbase2_ceil(DIV_ROUND_UP(height, block_height)));
   }

   if (pCreateInfo->viewType != VK_IMAGE_VIEW_TYPE_3D) {
      iview->descriptor[5] |= A6XX_TEX_CONST_5_DEPTH(iview->layer_count);
   } else {
      iview->descriptor[3] |=
         A6XX_TEX_CONST_3_MIN_LAYERSZ(image->levels[image->level_count - 1].size);
      iview->descriptor[5] |=
         A6XX_TEX_CONST_5_DEPTH(u_minify(image->extent.depth, iview->base_mip));
   }
}

unsigned
tu_image_queue_family_mask(const struct tu_image *image,
                           uint32_t family,
                           uint32_t queue_family)
{
   if (!image->exclusive)
      return image->queue_family_mask;
   if (family == VK_QUEUE_FAMILY_EXTERNAL)
      return (1u << TU_MAX_QUEUE_FAMILIES) - 1u;
   if (family == VK_QUEUE_FAMILY_IGNORED)
      return 1u << queue_family;
   return 1u << family;
}

VkResult
tu_CreateImage(VkDevice device,
               const VkImageCreateInfo *pCreateInfo,
               const VkAllocationCallbacks *pAllocator,
               VkImage *pImage)
{
#ifdef ANDROID
   const VkNativeBufferANDROID *gralloc_info =
      vk_find_struct_const(pCreateInfo->pNext, NATIVE_BUFFER_ANDROID);

   if (gralloc_info)
      return tu_image_from_gralloc(device, pCreateInfo, gralloc_info,
                                   pAllocator, pImage);
#endif

   const struct wsi_image_create_info *wsi_info =
      vk_find_struct_const(pCreateInfo->pNext, WSI_IMAGE_CREATE_INFO_MESA);
   uint64_t modifier = DRM_FORMAT_MOD_INVALID;

   if (wsi_info) {
      modifier = DRM_FORMAT_MOD_LINEAR;
      for (unsigned i = 0; i < wsi_info->modifier_count; i++) {
         if (wsi_info->modifiers[i] == DRM_FORMAT_MOD_QCOM_COMPRESSED)
            modifier = DRM_FORMAT_MOD_QCOM_COMPRESSED;
      }
   }

   return tu_image_create(device, pCreateInfo, pAllocator, pImage, modifier);
}

void
tu_DestroyImage(VkDevice _device,
                VkImage _image,
                const VkAllocationCallbacks *pAllocator)
{
   TU_FROM_HANDLE(tu_device, device, _device);
   TU_FROM_HANDLE(tu_image, image, _image);

   if (!image)
      return;

   if (image->owned_memory != VK_NULL_HANDLE)
      tu_FreeMemory(_device, image->owned_memory, pAllocator);

   vk_free2(&device->alloc, pAllocator, image);
}

void
tu_GetImageSubresourceLayout(VkDevice _device,
                             VkImage _image,
                             const VkImageSubresource *pSubresource,
                             VkSubresourceLayout *pLayout)
{
   TU_FROM_HANDLE(tu_image, image, _image);

   const uint32_t layer_offset = image->layer_size * pSubresource->arrayLayer;
   const struct tu_image_level *level =
      image->levels + pSubresource->mipLevel;

   pLayout->offset = layer_offset + level->offset;
   pLayout->size = level->size;
   pLayout->rowPitch =
      level->pitch * vk_format_get_blocksize(image->vk_format);
   pLayout->arrayPitch = image->layer_size;
   pLayout->depthPitch = level->size;

   if (image->ubwc_size) {
      /* UBWC starts at offset 0 */
      pLayout->offset = 0;
      /* UBWC scanout won't match what the kernel wants if we have levels/layers */
      assert(image->level_count == 1 && image->layer_count == 1);
   }
}

VkResult
tu_CreateImageView(VkDevice _device,
                   const VkImageViewCreateInfo *pCreateInfo,
                   const VkAllocationCallbacks *pAllocator,
                   VkImageView *pView)
{
   TU_FROM_HANDLE(tu_device, device, _device);
   struct tu_image_view *view;

   view = vk_alloc2(&device->alloc, pAllocator, sizeof(*view), 8,
                    VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (view == NULL)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   tu_image_view_init(view, device, pCreateInfo);

   *pView = tu_image_view_to_handle(view);

   return VK_SUCCESS;
}

void
tu_DestroyImageView(VkDevice _device,
                    VkImageView _iview,
                    const VkAllocationCallbacks *pAllocator)
{
   TU_FROM_HANDLE(tu_device, device, _device);
   TU_FROM_HANDLE(tu_image_view, iview, _iview);

   if (!iview)
      return;
   vk_free2(&device->alloc, pAllocator, iview);
}

void
tu_buffer_view_init(struct tu_buffer_view *view,
                    struct tu_device *device,
                    const VkBufferViewCreateInfo *pCreateInfo)
{
   TU_FROM_HANDLE(tu_buffer, buffer, pCreateInfo->buffer);

   view->range = pCreateInfo->range == VK_WHOLE_SIZE
                    ? buffer->size - pCreateInfo->offset
                    : pCreateInfo->range;
   view->vk_format = pCreateInfo->format;
}

VkResult
tu_CreateBufferView(VkDevice _device,
                    const VkBufferViewCreateInfo *pCreateInfo,
                    const VkAllocationCallbacks *pAllocator,
                    VkBufferView *pView)
{
   TU_FROM_HANDLE(tu_device, device, _device);
   struct tu_buffer_view *view;

   view = vk_alloc2(&device->alloc, pAllocator, sizeof(*view), 8,
                    VK_SYSTEM_ALLOCATION_SCOPE_OBJECT);
   if (!view)
      return vk_error(device->instance, VK_ERROR_OUT_OF_HOST_MEMORY);

   tu_buffer_view_init(view, device, pCreateInfo);

   *pView = tu_buffer_view_to_handle(view);

   return VK_SUCCESS;
}

void
tu_DestroyBufferView(VkDevice _device,
                     VkBufferView bufferView,
                     const VkAllocationCallbacks *pAllocator)
{
   TU_FROM_HANDLE(tu_device, device, _device);
   TU_FROM_HANDLE(tu_buffer_view, view, bufferView);

   if (!view)
      return;

   vk_free2(&device->alloc, pAllocator, view);
}
