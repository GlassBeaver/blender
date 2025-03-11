/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup overlay
 */

#pragma once

#include "DNA_world_types.h"

#include "draw_cache.hh"

#include "overlay_next_base.hh"

#include "BLI_subprocess.hh"

#include "GPU_texture.hh"
#include "gpu_texture_private.hh"

namespace blender::draw::overlay {

struct CGlassLinkShared final {
  void *SharedTexHandles[3]{nullptr, nullptr, nullptr};
  void *SharedFenceHandle{nullptr};
  size_t SharedTexSize{0};
  int Width{0};
  int Height{0};
};

/**
 * Draw background color .
 */
class Background : Overlay {
 private:
  PassSimple bg_ps_ = {"Background"};

  GPUFrameBuffer *framebuffer_ref_ = nullptr;

  GPUTexture *GlassLinkTexs[3]{nullptr, nullptr, nullptr};
  CGlassLinkShared GlassLinkShared;
  SharedMemory SharedMem = SharedMemory("Global\\GlassLinkShared", sizeof(GlassLinkShared), false);

 public:
  void begin_sync(Resources &res, const State &state) final
  {
    // init GlassLink and check if there's a new set of textures because it got restarted
    {
      if (SharedMem.get_size() == 0)
        SharedMem.Init(sizeof(GlassLinkShared));

      void *pSharedData = SharedMem.get_data();
      if (pSharedData) {
        CGlassLinkShared newGlassLinkShared = *reinterpret_cast<CGlassLinkShared *>(pSharedData);
        CGlassLinkShared oldGlassLinkShared = GlassLinkShared;

        if (memcmp(&GlassLinkShared, &newGlassLinkShared, sizeof(GlassLinkShared)) != 0) {
          GlassLinkShared = newGlassLinkShared;

          if (GlassLinkShared.SharedTexHandles[0] && GlassLinkShared.SharedTexSize != 0) {
            // TODO: remove
            printf("creating glasslink textures from handles %p     %p     %p     %p\n",
                   GlassLinkShared.SharedFenceHandle,
                   GlassLinkShared.SharedTexHandles[0],
                   GlassLinkShared.SharedTexHandles[1],
                   GlassLinkShared.SharedTexHandles[2]);

            for (int i = 0; i < 3; i++) {
              if (GlassLinkTexs[i]) {
                GPU_texture_free(GlassLinkTexs[i]);
                GlassLinkTexs[i] = nullptr;
              }

              GlassLinkTexs[i] = GPU_texture_create_2d("glassLinkTex",
                                                       GlassLinkShared.Width,
                                                       GlassLinkShared.Height,
                                                       1,
                                                       GPU_R11F_G11F_B10F,
                                                       GPU_TEXTURE_USAGE_SHADER_READ,
                                                       NULL,
                                                       GlassLinkShared.SharedTexHandles[i],
                                                       GlassLinkShared.SharedTexSize,
                                                       GlassLinkShared.SharedFenceHandle);
            }
          }
          else {
            // TODO: remove
            printf("deleting glasslink textures %p     %p     %p\n",
                   GlassLinkTexs[0],
                   GlassLinkTexs[1],
                   GlassLinkTexs[2]);

            for (int i = 0; i < 3; i++)
              if (GlassLinkTexs[i]) {
                GPU_texture_free(GlassLinkTexs[i]);
                GlassLinkTexs[i] = nullptr;
              }

            SharedMem.Release();
          }
        }
      }
    }

    DRWState pass_state = DRW_STATE_WRITE_COLOR | DRW_STATE_BLEND_BACKGROUND;
    float4 color_override(0.0f, 0.0f, 0.0f, 0.0f);
    int background_type;

    if (state.is_viewport_image_render && !state.draw_background) {
      background_type = BG_SOLID;
      color_override[3] = 1.0f;
    }
    else if (state.is_space_image()) {
      background_type = BG_SOLID_CHECKER;
    }
    else if (state.is_space_node()) {
      background_type = BG_MASK;
      pass_state = DRW_STATE_WRITE_COLOR | DRW_STATE_BLEND_MUL;
    }
    else if (!state.draw_background) {
      background_type = BG_CHECKER;
    }
    else if (state.v3d->shading.background_type == V3D_SHADING_BACKGROUND_WORLD &&
             state.scene->world)
    {
      background_type = BG_SOLID;
      /* TODO(fclem): this is a scene referred linear color. we should convert
       * it to display linear here. */
      color_override = float4(UNPACK3(&state.scene->world->horr), 1.0f);
    }
    else if (state.v3d->shading.background_type == V3D_SHADING_BACKGROUND_VIEWPORT &&
             state.v3d->shading.type <= OB_SOLID)
    {
      background_type = BG_SOLID;
      color_override = float4(UNPACK3(state.v3d->shading.background_color), 1.0f);
    }
    else {
      switch (UI_GetThemeValue(TH_BACKGROUND_TYPE)) {
        case TH_BACKGROUND_GRADIENT_LINEAR:
          background_type = BG_GRADIENT;
          break;
        case TH_BACKGROUND_GRADIENT_RADIAL:
          background_type = BG_RADIAL;
          break;
        default:
        case TH_BACKGROUND_SINGLE_COLOR:
          background_type = GlassLinkTexs[0] ? BG_GLASSLINK : BG_SOLID;
          break;
      }
    }

    bg_ps_.init();
    bg_ps_.framebuffer_set(&framebuffer_ref_);

    static int iGlassLinkTex = 0; // a guess since I'm not transporting the fence value over from Horu and opengl has no way of querying the last signaled value from the DX12 fence
    if (GlassLinkTexs[iGlassLinkTex])
      reinterpret_cast<blender::gpu::Texture *>(GlassLinkTexs[iGlassLinkTex])->WaitOnGlassLinkSemaphore();

    if ((state.clipping_plane_count != 0) && (state.rv3d) && (state.rv3d->clipbb)) {
      Span<float3> bbox(reinterpret_cast<float3 *>(state.rv3d->clipbb->vec[0]), 8);

      bg_ps_.state_set(DRW_STATE_WRITE_COLOR | DRW_STATE_BLEND_ALPHA | DRW_STATE_CULL_BACK);
      bg_ps_.shader_set(res.shaders.background_clip_bound.get());
      bg_ps_.push_constant("ucolor", res.theme_settings.color_clipping_border);
      bg_ps_.push_constant("boundbox", bbox.data(), 8);
      bg_ps_.draw(res.shapes.cube_solid.get());
    }

    bg_ps_.state_set(pass_state);
    bg_ps_.shader_set(res.shaders.background_fill.get());
    bg_ps_.bind_ubo(OVERLAY_GLOBALS_SLOT, &res.globals_buf);
    bg_ps_.bind_ubo(DRW_CLIPPING_UBO_SLOT, &res.clip_planes_buf);
    bg_ps_.bind_texture("colorBuffer", &res.color_render_tx);
    bg_ps_.bind_texture("depthBuffer", &res.depth_tx);

    if (GlassLinkTexs[iGlassLinkTex])
      bg_ps_.bind_texture("glassLink", &GlassLinkTexs[iGlassLinkTex]);

    bg_ps_.push_constant("colorOverride", color_override);
    bg_ps_.push_constant("bgType", background_type);
    bg_ps_.draw_procedural(GPU_PRIM_TRIS, 1, 3);

    iGlassLinkTex++;
    iGlassLinkTex = iGlassLinkTex % 3;
  }

  void draw_output(Framebuffer &framebuffer, Manager &manager, View &view) final
  {
    framebuffer_ref_ = framebuffer;
    manager.submit(bg_ps_, view);
  }
};

}  // namespace blender::draw::overlay
