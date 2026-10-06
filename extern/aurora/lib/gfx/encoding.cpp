#include "encoding.hpp"

#include "frame.hpp"
#include "geometry_buffer.hpp"

#include "clear.hpp"
#include "depth_peek.hpp"
#include "draw_payload.hpp"
#include "stereo_multiview.hpp"
#include "stereo_shadow.hpp"
#include "pipeline_cache.hpp"
#include "resource_cache.hpp"
#include "probe.hpp"
#include "tex_copy_conv.hpp"
#include "tex_palette_conv.hpp"
#include "../gx/gx.hpp"
#include "../gx/pipeline.hpp"
#ifdef AURORA_ENABLE_RMLUI
#include "../rmlui/pipeline.hpp"
#endif
#include "../webgpu/gpu.hpp"
#include "../webgpu/gpu_prof.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include <tracy/Tracy.hpp>

namespace aurora::gfx {
using namespace detail;
using webgpu::g_device;
using webgpu::g_queue;

namespace {
constexpr Module Log{"aurora::gfx"};
PipelineRef g_currentPipeline;
// The uniform bind group and offset the pass's last GX draw bound (bind_gx_uniform),
// null when unknown.
WGPUBindGroup g_currentUniform = nullptr;
uint32_t g_currentUniformOffset = 0;
// ... and the texture bind group and index range (bind_gx_textures, bind_gx_indices).
BindGroupRef g_currentTextures = 0;
uint64_t g_currentIndexOffset = UINT64_MAX;
uint64_t g_currentIndexSize = 0;
wgpu::IndexFormat g_currentIndexFormat = wgpu::IndexFormat::Uint16;
// ... and group 0: the frame's vertex buffer (0), the geometry cache's (1), unknown (-1).
int8_t g_currentGeometry = -1;

// After a draw of another kind, which binds what it needs itself.
void forget_gx_binds() {
  g_currentUniform = nullptr;
  g_currentTextures = 0;
  g_currentIndexOffset = UINT64_MAX;
  g_currentIndexSize = 0;
  g_currentGeometry = -1;
}

// A pass's group 0 at its start: the frame's buffers.
void bind_frame_geometry(const wgpu::RenderPassEncoder& pass) {
  pass.SetBindGroup(0, resources().staticBindGroup);
  g_currentGeometry = 0;
}

// At a pass's start and end, and after a draw that may bind state of its own.
void forget_bound_state() {
  g_currentPipeline = UINTPTR_MAX;
  forget_gx_binds();
}

void apply_viewport(const wgpu::RenderPassEncoder& pass, const Viewport& vp) {
  const float minDepth = gx::UseReversedZ ? 1.f - vp.zfar : vp.znear;
  const float maxDepth = gx::UseReversedZ ? 1.f - vp.znear : vp.zfar;
  pass.SetViewport(vp.left, vp.top, vp.width, vp.height, minDepth, maxDepth);
}

void apply_scissor(const wgpu::RenderPassEncoder& pass, const ClipRect& sc, const wgpu::Extent3D& size) {
  const auto x = std::clamp(static_cast<uint32_t>(sc.x), 0u, size.width);
  const auto y = std::clamp(static_cast<uint32_t>(sc.y), 0u, size.height);
  const auto w = std::clamp(static_cast<uint32_t>(sc.width), 0u, size.width - x);
  const auto h = std::clamp(static_cast<uint32_t>(sc.height), 0u, size.height - y);
  pass.SetScissorRect(x, y, w, h);
}

// --- stereo eye passes (stereo_frame.hpp) ---

// The recorded commands of an EFB pass re-issued into one eye target. GX draws
// bind their eye uniform (gx::render_eye), clear draws cover the eye, viewports
// and scissors scale from the EFB to the eye, and everything else is mono-only.
// A multiview pass (stereo_multiview.hpp) re-issues them once for both eyes,
// whose targets share a size: `eye` is then 0 and the draws bind their
// multiview pipelines.
void render_stereo_eye_pass_commands(const wgpu::RenderPassEncoder& pass, RenderPass& passInfo, uint32_t eye,
                                     bool multiview = false) {
  const auto& eyePass = passInfo.stereo.eyes[eye];
  const auto& sourceSize = passInfo.colorAttachments[SceneColorAttachmentIndex].size;
  const float scaleX =
      sourceSize.width != 0 ? static_cast<float>(eyePass.size.width) / static_cast<float>(sourceSize.width) : 1.f;
  const float scaleY = sourceSize.height != 0
                           ? static_cast<float>(eyePass.size.height) / static_cast<float>(sourceSize.height)
                           : 1.f;
  forget_bound_state();
  bind_frame_geometry(pass);
  pass.SetBindGroup(2, multiview ? gx::g_emptyMultiviewTextureBindGroup : gx::g_emptyTextureBindGroup);

  for (auto& cmd : passInfo.commands) {
    switch (cmd.type) {
    case CommandType::SetViewport: {
      const auto& vp = cmd.data.setViewport;
      apply_viewport(pass, Viewport{
                               .left = vp.left * scaleX,
                               .top = vp.top * scaleY,
                               .width = vp.width * scaleX,
                               .height = vp.height * scaleY,
                               .znear = vp.znear,
                               .zfar = vp.zfar,
                           });
    } break;
    case CommandType::SetScissor: {
      const auto& sc = cmd.data.setScissor;
      const auto left = static_cast<int32_t>(std::floor(static_cast<float>(sc.x) * scaleX));
      const auto top = static_cast<int32_t>(std::floor(static_cast<float>(sc.y) * scaleY));
      const auto right = static_cast<int32_t>(std::ceil(static_cast<float>(sc.x + sc.width) * scaleX));
      const auto bottom = static_cast<int32_t>(std::ceil(static_cast<float>(sc.y + sc.height) * scaleY));
      apply_scissor(pass,
                    ClipRect{
                        .x = std::max(left, 0),
                        .y = std::max(top, 0),
                        .width = std::max(right - std::max(left, 0), 0),
                        .height = std::max(bottom - std::max(top, 0), 0),
                    },
                    eyePass.size);
    } break;
    case CommandType::Draw: {
      auto& draw = cmd.data.draw;
      if (draw.kind == DrawKind::GX) {
        if (multiview) {
          gx::render_multiview(inline_payload<gx::DrawData>(draw.payload.data()), pass);
        } else {
          gx::render_eye(inline_payload<gx::DrawData>(draw.payload.data()), pass, eye);
        }
      } else if (draw.kind == DrawKind::Clear) {
        if (multiview) {
          clear::render_multiview(inline_payload<clear::DrawData>(draw.payload.data()), pass, eyePass.size);
        } else {
          clear::render(inline_payload<clear::DrawData>(draw.payload.data()), pass, eyePass.size);
        }
      }
    } break;
    case CommandType::CustomDraw:
    case CommandType::DebugMarker:
      break;
    }
  }
}

// One eye's render pass over the same attachments semantics as the mono pass
// (its clears and loads mirrored), on the eye target.
void render_stereo_eye_pass(wgpu::CommandEncoder& cmd, RenderPass& passInfo, uint32_t passIndex, uint32_t eye) {
  const auto& eyePass = passInfo.stereo.eyes[eye];
  if (!eyePass.colorView) {
    return;
  }
  const auto& source = passInfo.colorAttachments[SceneColorAttachmentIndex];
  const wgpu::RenderPassColorAttachment attachment{
      .view = eyePass.colorView,
      .resolveTarget = eyePass.resolveView,
      .loadOp = source.loadOp != wgpu::LoadOp::Undefined ? source.loadOp
                                                         : (source.clear ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load),
      .storeOp = source.storeOp,
      .clearValue =
          {
              .r = source.clearValue.x(),
              .g = source.clearValue.y(),
              .b = source.clearValue.z(),
              .a = source.clearValue.w(),
          },
  };
  wgpu::RenderPassDepthStencilAttachment depthStencilAttachment{};
  const wgpu::RenderPassDepthStencilAttachment* depthStencilAttachmentPtr = nullptr;
  if (eyePass.depthView && passInfo.depthStencilView) {
    // After an eyes-only final pass nothing reads the eye's depth (the next frame
    // clears it): a tiled GPU then need not write it back to memory.
    const bool keepDepth = !passInfo.stereo.skipMono;
    depthStencilAttachment = {
        .view = eyePass.depthView,
        .depthLoadOp = passInfo.hasDepth ? (passInfo.depthLoadOp != wgpu::LoadOp::Undefined
                                                ? passInfo.depthLoadOp
                                                : (passInfo.clearDepth ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load))
                                         : wgpu::LoadOp::Undefined,
        .depthStoreOp = passInfo.hasDepth ? (keepDepth ? passInfo.depthStoreOp : wgpu::StoreOp::Discard)
                                          : wgpu::StoreOp::Undefined,
        .depthClearValue = passInfo.clearDepthValue,
        .stencilLoadOp = passInfo.hasStencil ? passInfo.stencilLoadOp : wgpu::LoadOp::Undefined,
        .stencilStoreOp = passInfo.hasStencil ? (keepDepth ? passInfo.stencilStoreOp : wgpu::StoreOp::Discard)
                                              : wgpu::StoreOp::Undefined,
        .stencilClearValue = passInfo.stencilClearValue,
    };
    depthStencilAttachmentPtr = &depthStencilAttachment;
  }
  const auto label = fmt::format("Stereo eye {} pass {}", eye, passIndex);
  const wgpu::RenderPassDescriptor renderPassDescriptor{
      .label = label.c_str(),
      .colorAttachmentCount = 1,
      .colorAttachments = &attachment,
      .depthStencilAttachment = depthStencilAttachmentPtr,
      .timestampWrites = webgpu::gpu_prof::pass_writes(label),
  };
  auto pass = cmd.BeginRenderPass(&renderPassDescriptor);
  render_stereo_eye_pass_commands(pass, passInfo, eye);
  pass.End();
  forget_bound_state();
}

// Both eyes' render pass at once: Vulkan multiview over the eye targets' two
// layers (stereo_multiview.hpp), with the attachment semantics of an eye pass.
void render_stereo_multiview_pass(wgpu::CommandEncoder& cmd, RenderPass& passInfo, uint32_t passIndex) {
#if defined(WGPU_DAWN_RENDER_PASS_MULTIVIEW_INIT)
  const auto& source = passInfo.colorAttachments[SceneColorAttachmentIndex];
  const wgpu::RenderPassColorAttachment attachment{
      .view = passInfo.stereo.multiviewColorView,
      .loadOp = source.loadOp != wgpu::LoadOp::Undefined ? source.loadOp
                                                         : (source.clear ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load),
      .storeOp = source.storeOp,
      .clearValue =
          {
              .r = source.clearValue.x(),
              .g = source.clearValue.y(),
              .b = source.clearValue.z(),
              .a = source.clearValue.w(),
          },
  };
  wgpu::RenderPassDepthStencilAttachment depthStencilAttachment{};
  const wgpu::RenderPassDepthStencilAttachment* depthStencilAttachmentPtr = nullptr;
  if (passInfo.stereo.multiviewDepthView && passInfo.depthStencilView) {
    // After an eyes-only final pass nothing reads the eyes' depth (as render_stereo_eye_pass).
    const bool keepDepth = !passInfo.stereo.skipMono;
    depthStencilAttachment = {
        .view = passInfo.stereo.multiviewDepthView,
        .depthLoadOp = passInfo.hasDepth ? (passInfo.depthLoadOp != wgpu::LoadOp::Undefined
                                                ? passInfo.depthLoadOp
                                                : (passInfo.clearDepth ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load))
                                         : wgpu::LoadOp::Undefined,
        .depthStoreOp = passInfo.hasDepth ? (keepDepth ? passInfo.depthStoreOp : wgpu::StoreOp::Discard)
                                          : wgpu::StoreOp::Undefined,
        .depthClearValue = passInfo.clearDepthValue,
        .stencilLoadOp = passInfo.hasStencil ? passInfo.stencilLoadOp : wgpu::LoadOp::Undefined,
        .stencilStoreOp = passInfo.hasStencil ? (keepDepth ? passInfo.stencilStoreOp : wgpu::StoreOp::Discard)
                                              : wgpu::StoreOp::Undefined,
        .stencilClearValue = passInfo.stencilClearValue,
    };
    depthStencilAttachmentPtr = &depthStencilAttachment;
  }
  wgpu::DawnRenderPassMultiview multiview{};
  multiview.viewMask = stereo_multiview::kViewMask;
  const auto label = fmt::format("Stereo eyes pass {} (multiview)", passIndex);
  // No timestamp writes: a query inside a multiview pass takes one slot per view.
  const wgpu::RenderPassDescriptor renderPassDescriptor{
      .nextInChain = &multiview,
      .label = label.c_str(),
      .colorAttachmentCount = 1,
      .colorAttachments = &attachment,
      .depthStencilAttachment = depthStencilAttachmentPtr,
  };
  auto pass = cmd.BeginRenderPass(&renderPassDescriptor);
  render_stereo_eye_pass_commands(pass, passInfo, 0, true);
  pass.End();
  forget_bound_state();
#else
  (void)cmd;
  (void)passInfo;
  (void)passIndex;
#endif
}

DrawContext make_draw_context(const RenderPass& passInfo) {
  auto& res = resources();
  return {
      .device = g_device,
      .queue = g_queue,
      .vertexBuffer = res.vertexBuffer,
      .indexBuffer = res.indexBuffer,
      .uniformBuffer = res.uniformBuffer,
      .storageBuffer = res.storageBuffer,
      .layout = passInfo.target_layout(),
  };
}

void render_custom_draw(const CustomDrawCommand& draw, const wgpu::RenderPassEncoder& pass,
                        const RenderPass& passInfo) {
  const auto drawType = find_runtime_draw_type(draw.type);
  if (!drawType) {
    // Unregistered between record and replay; the command is a no-op.
    return;
  }

  const auto context = make_draw_context(passInfo);
  drawType->draw(context, pass, draw.payload.data(), draw.payloadSize, drawType->userdata);
}

void execute_encoder_task(wgpu::CommandEncoder& cmd, FramePacket& frame, const EncoderTask& task) {
  const auto taskType = find_runtime_encoder_task_type(task.type);
  if (!taskType) {
    // Unregistered between record and encode; the task is a no-op.
    return;
  }

  auto& res = resources();
  const EncoderTaskContext context{
      .device = g_device,
      .queue = g_queue,
      .vertexBuffer = res.vertexBuffer,
      .indexBuffer = res.indexBuffer,
      .uniformBuffer = res.uniformBuffer,
      .storageBuffer = res.storageBuffer,
  };
  taskType->callback(context, cmd, task.payload.data(), task.payloadSize, taskType->userdata);
  if (taskType->afterSubmit != nullptr) {
    const auto payload = task.payload;
    const auto payloadSize = task.payloadSize;
    const auto callback = taskType->afterSubmit;
    const auto userdata = taskType->userdata;
    frame.afterSubmitCallbacks.emplace_back([payload, payloadSize, callback, userdata] {
      const EncoderTaskCompletionContext completionContext{
          .device = g_device,
          .queue = g_queue,
      };
      callback(completionContext, payload.data(), payloadSize, userdata);
    });
  }
}

void render_pass(const wgpu::RenderPassEncoder& pass, FramePacket& frame, RenderPass& passInfo) {
  ZoneScoped;
  forget_bound_state();
#ifdef AURORA_GFX_DEBUG_GROUPS
  std::vector<std::string> lastDebugGroupStack;
#endif
  Viewport currentViewport{};
  ClipRect currentScissor{};
  bool hasViewport = false;
  bool hasScissor = false;

  // Bind bind group for the whole pass
  bind_frame_geometry(pass);
  pass.SetBindGroup(2, gx::g_emptyTextureBindGroup);

  for (auto& cmd : passInfo.commands) {
#ifdef AURORA_GFX_DEBUG_GROUPS
    {
      size_t firstDiff = lastDebugGroupStack.size();
      for (size_t i = 0; i < lastDebugGroupStack.size(); ++i) {
        if (i >= cmd.debugGroupStack.size() || cmd.debugGroupStack[i] != lastDebugGroupStack[i]) {
          firstDiff = i;
          break;
        }
      }
      for (size_t i = firstDiff; i < lastDebugGroupStack.size(); ++i) {
        pass.PopDebugGroup();
      }
      for (size_t i = firstDiff; i < cmd.debugGroupStack.size(); ++i) {
        pass.PushDebugGroup(cmd.debugGroupStack[i].c_str());
      }
      lastDebugGroupStack = cmd.debugGroupStack;
    }
#endif
    switch (cmd.type) {
    case CommandType::SetViewport: {
      const auto& vp = cmd.data.setViewport;
      apply_viewport(pass, vp);
      currentViewport = vp;
      hasViewport = true;
    } break;
    case CommandType::SetScissor: {
      const auto& sc = cmd.data.setScissor;
      apply_scissor(pass, sc, passInfo.colorAttachments[SceneColorAttachmentIndex].size);
      currentScissor = sc;
      hasScissor = true;
    } break;
    case CommandType::Draw: {
      auto& draw = cmd.data.draw;
      if (draw.encoder != nullptr) {
        draw.encoder(draw.payload.data(), pass, passInfo);
        if (draw.kind == DrawKind::Other) {
          forget_gx_binds(); // RmlUi binds its own groups and index buffers
        }
      }
    } break;
    case CommandType::CustomDraw: {
      render_custom_draw(cmd.data.customDraw, pass, passInfo);
      forget_bound_state();
      bind_frame_geometry(pass);
      pass.SetBindGroup(2, gx::g_emptyTextureBindGroup);
      if (hasViewport) {
        apply_viewport(pass, currentViewport);
      }
      if (hasScissor) {
        apply_scissor(pass, currentScissor, passInfo.colorAttachments[SceneColorAttachmentIndex].size);
      }
    } break;
    case CommandType::DebugMarker: {
#if defined(AURORA_GFX_DEBUG_GROUPS)
      pass.InsertDebugMarker(wgpu::StringView(frame.debugMarkers[cmd.data.debugMarkerIndex]));
#endif
    } break;
    }
  }

#ifdef AURORA_GFX_DEBUG_GROUPS
  for (size_t i = 0; i < lastDebugGroupStack.size(); ++i) {
    pass.PopDebugGroup();
  }
#endif
}

void render(wgpu::CommandEncoder& cmd, FramePacket& frame, RenderPass& passInfo, uint32_t passIndex) {
  ZoneScoped;
  if (!passInfo.sealed) {
    return;
  }

  for (const auto& conv : passInfo.paletteConvs) {
    tex_palette_conv::run(cmd, conv);
  }
  for (const auto& eyeConvs : passInfo.stereo.paletteConvs) {
    for (const auto& conv : eyeConvs) {
      tex_palette_conv::run(cmd, conv);
    }
  }
  if (passInfo.discardable) {
    // This pass has no effect and can be safely discarded (e.g. an empty EFB segment between two back-to-back pass
    // breaks, or an unresolved offscreen pass).
    return;
  }

  std::array<wgpu::RenderPassColorAttachment, MaxColorAttachments> attachments{};
  for (uint32_t i = 0; i < passInfo.colorAttachmentCount; ++i) {
    const auto& source = passInfo.colorAttachments[i];
    attachments[i] = {
        .view = source.view,
        .resolveTarget = source.resolveView,
        .loadOp = source.loadOp != wgpu::LoadOp::Undefined ? source.loadOp
                                                           : (source.clear ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load),
        .storeOp = source.storeOp,
        .clearValue =
            {
                .r = source.clearValue.x(),
                .g = source.clearValue.y(),
                .b = source.clearValue.z(),
                .a = source.clearValue.w(),
            },
    };
  }
  wgpu::RenderPassDepthStencilAttachment depthStencilAttachment{};
  const wgpu::RenderPassDepthStencilAttachment* depthStencilAttachmentPtr = nullptr;
  if (passInfo.depthStencilView) {
    depthStencilAttachment = {
        .view = passInfo.depthStencilView,
        .depthLoadOp = passInfo.hasDepth ? (passInfo.depthLoadOp != wgpu::LoadOp::Undefined
                                                ? passInfo.depthLoadOp
                                                : (passInfo.clearDepth ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load))
                                         : wgpu::LoadOp::Undefined,
        .depthStoreOp = passInfo.hasDepth ? passInfo.depthStoreOp : wgpu::StoreOp::Undefined,
        .depthClearValue = passInfo.clearDepthValue,
        .stencilLoadOp = passInfo.hasStencil ? passInfo.stencilLoadOp : wgpu::LoadOp::Undefined,
        .stencilStoreOp = passInfo.hasStencil ? passInfo.stencilStoreOp : wgpu::StoreOp::Undefined,
        .stencilClearValue = passInfo.stencilClearValue,
    };
    depthStencilAttachmentPtr = &depthStencilAttachment;
  }
  const auto label = passInfo.label.empty() ? fmt::format("Render pass {}", passIndex)
                                            : fmt::format("{} {}", passInfo.label, passIndex);
  const wgpu::RenderPassDescriptor renderPassDescriptor{
      .label = label.c_str(),
      .colorAttachmentCount = passInfo.colorAttachmentCount,
      .colorAttachments = attachments.data(),
      .depthStencilAttachment = depthStencilAttachmentPtr,
      .timestampWrites = webgpu::gpu_prof::pass_writes(label),
  };

  // An immersive frame's final pass on a headset that owns the display: nothing
  // reads its mono image (stereo_frame.hpp StereoPassReplay::skipMono).
  if (!(passInfo.stereo.enabled && passInfo.stereo.skipMono)) {
    auto pass = cmd.BeginRenderPass(&renderPassDescriptor);
    render_pass(pass, frame, passInfo);
    pass.End();
  }

  // The same pass into each eye, before this pass's resolve so an eye draw
  // samples the EFB copies at the same point in the frame as the mono draw did.
  if (passInfo.stereo.enabled && passInfo.stereo.multiview) {
    render_stereo_multiview_pass(cmd, passInfo, passIndex);
  } else if (passInfo.stereo.enabled) {
    for (uint32_t eye = 0; eye < AURORA_STEREO_EYE_COUNT; ++eye) {
      render_stereo_eye_pass(cmd, passInfo, passIndex, eye);
    }
  }

  if (passInfo.captureDepthSnapshot) {
    depth_peek::encode_frame_snapshot(cmd, passInfo.copySourceDepthView,
                                      passInfo.colorAttachments[SceneColorAttachmentIndex].size, passInfo.msaaSamples);
  }

  if (passInfo.resolveTarget) {
    const auto& dstSize = passInfo.resolveTarget->size;
    const bool needsConversion = tex_copy_conv::needs_conversion(passInfo.resolveFormat);
    // A probe face is a layer of a cube texture, which only the blit addresses.
    const bool needsScaling = passInfo.probeFace >= 0 ||
                              dstSize.width != static_cast<uint32_t>(passInfo.resolveRect.width) ||
                              dstSize.height != static_cast<uint32_t>(passInfo.resolveRect.height);
    const bool isDepth = gx::is_depth_format(passInfo.resolveFormat);
    if (isDepth && passInfo.msaaSamples > 1) {
      Log.fatal("Depth tex copies from multisampled EFB targets are not supported");
    }
    const tex_copy_conv::ConvRequest convReq{
        .fmt = passInfo.resolveFormat,
        .srcView = isDepth ? passInfo.copySourceDepthView : passInfo.copySourceView,
        .uniformRange = passInfo.resolveUniformRange,
        .dst = passInfo.resolveTarget,
        .sampleFilter = needsScaling ? tex_copy_conv::SampleFilter::Linear : tex_copy_conv::SampleFilter::Nearest,
    };
    if (needsConversion) {
      tex_copy_conv::run(cmd, convReq);
    } else if (needsScaling) {
      tex_copy_conv::blit(cmd, convReq);
    } else {
      const webgpu::gpu_prof::Zone zone{cmd, "EFB copy"};
      const wgpu::TexelCopyTextureInfo src{
          .texture = passInfo.copySourceTexture,
          .origin =
              wgpu::Origin3D{
                  .x = static_cast<uint32_t>(passInfo.resolveRect.x),
                  .y = static_cast<uint32_t>(passInfo.resolveRect.y),
              },
      };
      const wgpu::TexelCopyTextureInfo dst{
          .texture = passInfo.resolveTarget->texture,
      };
      const wgpu::Extent3D size{
          .width = static_cast<uint32_t>(passInfo.resolveRect.width),
          .height = static_cast<uint32_t>(passInfo.resolveRect.height),
          .depthOrArrayLayers = 1,
      };
      cmd.CopyTextureToTexture(&src, &dst, &size);
    }
    if (passInfo.probeFace >= 0) {
      probe::encode_mips(cmd, passInfo.probeFace, passInfo.probeUniformRange);
    }
  }

  // Stereo replay: the same copy from each eye's image, over the same part of
  // the view (the copy's UV transform is relative), into the eye stand-ins. A
  // copy made under AURORA_STEREO_ROUTE_HEAD_LOCKED_2D has its own rectangle
  // per eye instead: the part of the eye's view behind the copied rectangle of
  // the head-locked plane (stereo_replay.hpp HeadLockedPlane).
  if (passInfo.stereo.enabled && passInfo.resolveTarget && passInfo.probeFace < 0 &&
      passInfo.stereo.copyTargets[0] && !gx::is_depth_format(passInfo.resolveFormat)) {
    const webgpu::gpu_prof::Zone zone{cmd, "Stereo eye copies"};
    for (uint32_t eye = 0; eye < AURORA_STEREO_EYE_COUNT; ++eye) {
      const auto& eyePass = passInfo.stereo.eyes[eye];
      // The eye's sampleable view: the pass may have rendered through a
      // foveated, attachment-only view of the same image (stereo_foveation.hpp).
      const auto& source = eyePass.resolveView      ? eyePass.resolveView
                           : eyePass.copySourceView ? eyePass.copySourceView
                                                    : eyePass.colorView;
      if (!source || !passInfo.stereo.copyTargets[eye]) {
        continue;
      }
      const auto& eyeUniform = passInfo.stereo.copyUniformRanges[eye];
      const tex_copy_conv::ConvRequest eyeReq{
          .fmt = passInfo.resolveFormat,
          .srcView = source,
          .uniformRange = eyeUniform.size != 0 ? eyeUniform : passInfo.resolveUniformRange,
          .dst = passInfo.stereo.copyTargets[eye],
          .sampleFilter = tex_copy_conv::SampleFilter::Linear,
      };
      if (tex_copy_conv::needs_conversion(passInfo.resolveFormat)) {
        tex_copy_conv::run(cmd, eyeReq);
      } else {
        tex_copy_conv::blit(cmd, eyeReq);
      }
    }
  }

  if (passInfo.snapshotColorDst) {
    const webgpu::gpu_prof::Zone zone{cmd, "Pass snapshot"};
    const wgpu::TexelCopyTextureInfo src{
        .texture = passInfo.copySourceTexture,
    };
    const wgpu::TexelCopyTextureInfo dst{
        .texture = passInfo.snapshotColorDst,
    };
    const wgpu::Extent3D size{
        .width = passInfo.colorAttachments[SceneColorAttachmentIndex].size.width,
        .height = passInfo.colorAttachments[SceneColorAttachmentIndex].size.height,
        .depthOrArrayLayers = 1,
    };
    cmd.CopyTextureToTexture(&src, &dst, &size);
  }
  if (passInfo.snapshotDepthDst) {
    tex_copy_conv::snapshot_depth(cmd, passInfo.copySourceDepthView, passInfo.msaaSamples, passInfo.snapshotDepthDst);
  }
}

constexpr uint32_t align_down_copy_offset(uint32_t value) noexcept { return value & ~3u; }

void copy_staging_buffer_range(wgpu::CommandEncoder& cmd, const FramePacket& frame, uint32_t& copied,
                               uint32_t highWater, uint64_t stagingOffset, const wgpu::Buffer& dst) {
  if (highWater <= copied) {
    return;
  }
  const uint32_t copyStart = align_down_copy_offset(copied);
  const uint32_t copyEnd = AURORA_ALIGN(highWater, 4);
  cmd.CopyBufferToBuffer(staging_buffer(frame.stagingBuffer), stagingOffset + copyStart, dst, copyStart,
                         copyEnd - copyStart);
  copied = highWater;
}

bool needs_staging_copy(const FramePacket& frame, const FrameOp& op) {
  const auto& highWater = op.highWater;
  if (highWater.verts > frame.copied.verts || highWater.uniforms > frame.copied.uniforms ||
      highWater.indices > frame.copied.indices || highWater.storage > frame.copied.storage) {
    return true;
  }
  if constexpr (UseTextureBuffer) {
    return op.textureUploads.size() > frame.copied.textureUploadCount;
  }
  return false;
}

void copy_staging_to_high_water(wgpu::CommandEncoder& cmd, FramePacket& frame, const FrameOp& op) {
  if (!needs_staging_copy(frame, op)) {
    return;
  }
  const webgpu::gpu_prof::Zone zone{cmd, "Staging copies"};
  const auto& highWater = op.highWater;
  auto& res = resources();
  const auto& sizes = frame_buffer_sizes();
  const uint64_t VertexStagingOffset = 0;
  const uint64_t UniformStagingOffset = VertexStagingOffset + sizes.vertex;
  const uint64_t IndexStagingOffset = UniformStagingOffset + sizes.uniform;
  const uint64_t StorageStagingOffset = IndexStagingOffset + sizes.index;
  const uint64_t TextureUploadStagingOffset = StorageStagingOffset + sizes.storage;
  copy_staging_buffer_range(cmd, frame, frame.copied.verts, highWater.verts, VertexStagingOffset, res.vertexBuffer);
  copy_staging_buffer_range(cmd, frame, frame.copied.uniforms, highWater.uniforms, UniformStagingOffset,
                            res.uniformBuffer);
  copy_staging_buffer_range(cmd, frame, frame.copied.indices, highWater.indices, IndexStagingOffset, res.indexBuffer);
  copy_staging_buffer_range(cmd, frame, frame.copied.storage, highWater.storage, StorageStagingOffset,
                            res.storageBuffer);
  // The geometry cache's new blocks, from the vertex staging to its own buffer
  for (size_t i = frame.copied.geometryUploadCount; i < op.geometryUploads.size(); ++i) {
    const auto& upload = *op.geometryUploads[i];
    cmd.CopyBufferToBuffer(staging_buffer(frame.stagingBuffer), VertexStagingOffset + upload.src,
                           detail::geometry_buffer(), upload.dst, upload.size);
  }
  frame.copied.geometryUploadCount = op.geometryUploads.size();

  if constexpr (UseTextureBuffer) {
    for (size_t i = frame.copied.textureUploadCount; i < op.textureUploads.size(); ++i) {
      const auto& item = *op.textureUploads[i];
      const wgpu::TexelCopyBufferInfo buf{
          .layout =
              wgpu::TexelCopyBufferLayout{
                  .offset = item.buffer ? item.layout.offset : item.layout.offset + TextureUploadStagingOffset,
                  .bytesPerRow = AURORA_ALIGN(item.layout.bytesPerRow, 256),
                  .rowsPerImage = item.layout.rowsPerImage,
              },
          .buffer = item.buffer ? item.buffer : staging_buffer(frame.stagingBuffer),
      };
      cmd.CopyBufferToTexture(&buf, &item.tex, &item.size);
    }
    frame.copied.textureUpload = highWater.textureUpload;
    frame.copied.textureUploadCount = op.textureUploads.size();
  }
}
} // namespace

namespace detail {
void encode_op(wgpu::CommandEncoder& cmd, FramePacket& frame, const FrameOp& op) {
  copy_staging_to_high_water(cmd, frame, op);
  switch (op.type) {
  case FrameOpType::RenderPass:
    if (op.renderPass != nullptr) {
      render(cmd, frame, *op.renderPass, op.index);
    }
    break;
  case FrameOpType::TextureCopy:
    if (op.textureCopy != nullptr) {
      const webgpu::gpu_prof::Zone zone{cmd, "Texture copy"};
      cmd.CopyTextureToTexture(&op.textureCopy->src, &op.textureCopy->dst, &op.textureCopy->size);
    }
    break;
  case FrameOpType::EncoderTask:
    if (op.encoderTask != nullptr) {
      execute_encoder_task(cmd, frame, *op.encoderTask);
    }
    break;
  }
}
} // namespace detail

bool bind_pipeline(PipelineRef ref, const wgpu::RenderPassEncoder& pass) {
  if (ref == g_currentPipeline) {
    return true;
  }
  wgpu::RenderPipeline pipeline;
  if (!get_pipeline(ref, pipeline) || !pipeline) {
    return false;
  }
  pass.SetPipeline(pipeline);
  g_currentPipeline = ref;
  return true;
}

void bind_gx_uniform(const wgpu::RenderPassEncoder& pass, const wgpu::BindGroup& bindGroup, uint32_t offset) {
  if (bindGroup.Get() == g_currentUniform && offset == g_currentUniformOffset) {
    return;
  }
  pass.SetBindGroup(1, bindGroup, 1, &offset);
  g_currentUniform = bindGroup.Get();
  g_currentUniformOffset = offset;
}

void bind_gx_textures(const wgpu::RenderPassEncoder& pass, BindGroupRef bindGroup) {
  if (bindGroup == 0 || bindGroup == g_currentTextures) {
    return;
  }
  pass.SetBindGroup(2, find_bind_group(bindGroup));
  g_currentTextures = bindGroup;
}

void bind_gx_indices(const wgpu::RenderPassEncoder& pass, const wgpu::Buffer& buffer, uint64_t offset,
                     uint64_t size, wgpu::IndexFormat format) {
  if (offset == g_currentIndexOffset && size == g_currentIndexSize && format == g_currentIndexFormat) {
    return;
  }
  pass.SetIndexBuffer(buffer, format, offset, size);
  g_currentIndexOffset = offset;
  g_currentIndexSize = size;
  g_currentIndexFormat = format;
}

void bind_gx_geometry(const wgpu::RenderPassEncoder& pass, bool cached) {
  const int8_t wanted = cached ? 1 : 0;
  if (wanted == g_currentGeometry) {
    return;
  }
  pass.SetBindGroup(0, cached ? detail::geometry_bind_group() : resources().staticBindGroup);
  g_currentGeometry = wanted;
}
} // namespace aurora::gfx
