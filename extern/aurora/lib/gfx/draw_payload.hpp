#pragma once

// Inline draw payloads: how a draw's data is stored in a DrawCommand and
// encoded back out. Shared by the recorder (which builds the commands) and the
// encoders (the mono pass encoder and the stereo eye pass encoder, which has
// to recognise GX and clear draws to re-issue them per eye).

#include "frame_packet.hpp"
#include "clear.hpp"
#include "../gx/pipeline.hpp"

#include <cstring>
#include <new>
#include <type_traits>

namespace aurora::gfx::detail {

template <class T>
concept InlinePayload =
    std::is_trivially_copyable_v<T> && std::is_aggregate_v<T> && std::is_trivially_destructible_v<T>;

template <InlinePayload T>
T& inline_payload(void* ptr) noexcept {
  // C++20 equivalent of C++23's std::start_lifetime_as
  return *std::launder(static_cast<T*>(std::memmove(ptr, ptr, sizeof(T))));
}

template <InlinePayload T>
const T& inline_payload(const void* ptr) noexcept {
  return inline_payload<T>(const_cast<void*>(ptr));
}

template <auto Renderer, InlinePayload T>
void encode_draw(void* payload, const wgpu::RenderPassEncoder& pass, const RenderPass& passInfo) {
  const T& data = inline_payload<T>(payload);
  if constexpr (std::is_invocable_v<decltype(Renderer), const T&, const wgpu::RenderPassEncoder&,
                                    const wgpu::Extent3D&>) {
    Renderer(data, pass, passInfo.colorAttachments[SceneColorAttachmentIndex].size);
  } else {
    static_assert(std::is_invocable_v<decltype(Renderer), const T&, const wgpu::RenderPassEncoder&>);
    Renderer(data, pass);
  }
}

// What kind of draw a payload is, for the encoders that treat some specially.
template <class T>
inline constexpr DrawKind draw_kind_v = DrawKind::Other;
template <>
inline constexpr DrawKind draw_kind_v<gx::DrawData> = DrawKind::GX;
template <>
inline constexpr DrawKind draw_kind_v<clear::DrawData> = DrawKind::Clear;

template <auto Renderer, InlinePayload T>
DrawCommand make_draw_command(const T& data) {
  static_assert(sizeof(T) <= InlineDrawPayloadSize);
  static_assert(alignof(T) <= alignof(std::max_align_t));
  DrawCommand command{.encoder = encode_draw<Renderer, T>, .kind = draw_kind_v<T>};
  std::memcpy(command.payload.data(), &data, sizeof(data));
  return command;
}

} // namespace aurora::gfx::detail
