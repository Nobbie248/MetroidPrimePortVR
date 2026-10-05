#include "port_map_batch.h"

#include "Kyoto/Graphics/CGX.hpp"
#include "Kyoto/Graphics/CGraphics.hpp"
#include <dolphin/gx/GXGeometry.h>
#include <dolphin/gx/GXAurora.h>
#include <dolphin/gx/GXGet.h>

#include <cstdlib>

namespace PortMapBatch {
bool Enabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("MP_MINIMAP_BATCH");
    return value == nullptr || value[0] != '0';
  }();
  return enabled;
}

void Draw(const Geometry& geometry, const CTransform4f& model) {
  const auto& vertices = geometry.Vertices();
  if (!vertices.empty()) {
    constexpr GXVtxFmt format = GX_VTXFMT7;
    GXVtxAttrFmtList saved[4]{};
    const GXAttr attrs[] = {GX_VA_POS, GX_VA_NRM, GX_VA_CLR0, GX_VA_TEX0};
    for (size_t i = 0; i < 4; ++i) {
      saved[i].attr = attrs[i];
      GXGetVtxAttrFmt(format, attrs[i], &saved[i].cnt, &saved[i].type, &saved[i].frac);
    }
    GXSetVtxAttrFmt(format, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(format, GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0);
    GXSetVtxAttrFmt(format, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GXSetVtxAttrFmt(format, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    const GXVtxDescList desc[] = {{GX_VA_POS, GX_DIRECT}, {GX_VA_NRM, GX_DIRECT},
                                {GX_VA_CLR0, GX_DIRECT}, {GX_VA_TEX0, GX_DIRECT},
                                {GX_VA_NULL, GX_NONE}};
    CGX::SetVtxDescv(desc);
    CGX::SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
    CGX::SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_RASC);
    CGX::SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
    CGraphics::SetModelMatrix(model);
    AuroraSetMapBatch(GX_TRUE);
    for (size_t first = 0; first < vertices.size(); first += kDrawVertices) {
      const size_t count = std::min(kDrawVertices, vertices.size() - first);
      CGX::Begin(GX_TRIANGLES, format, static_cast<ushort>(count));
      for (size_t i = first; i < first + count; ++i) {
        const auto& vertex = vertices[i];
        GXPosition3f32(vertex.first[0], vertex.first[1], vertex.first[2]);
        GXNormal3f32(vertex.second[0], vertex.second[1], vertex.second[2]);
        GXColor4u8(vertex.color[0], vertex.color[1], vertex.color[2], vertex.color[3]);
        GXTexCoord2f32(vertex.signedWidth, vertex.endpoint);
      }
      CGX::End();
    }
    AuroraSetMapBatch(GX_FALSE);
    for (const auto& attr : saved) {
      GXSetVtxAttrFmt(format, attr.attr, attr.cnt, attr.type, attr.frac);
    }
  }
  // Match the state the original surface sequence leaves for subsequent doors.
  if (geometry.LineWidthChanged()) {
    CGX::SetLineWidth(geometry.LineWidth(), GX_TO_ONE);
  }
  if (geometry.HasColor()) {
    const auto& color = geometry.LastColor();
    CGX::SetTevKColor(GX_KCOLOR0, GXColor{color[0], color[1], color[2], color[3]});
  }
}
} // namespace PortMapBatch
