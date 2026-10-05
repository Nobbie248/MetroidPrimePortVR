#include "../gfx/hash.hpp"
#include "../gfx/probe.hpp"
#include "../gfx/types.hpp"

#include "../internal.hpp"
#include "../webgpu/gpu.hpp"
#include "gx.hpp"
#include "gx_fmt.hpp"
#include "shader_info.hpp"

#include <dolphin/gx/GXEnum.h>

#include <absl/container/flat_hash_set.h>
#include <cctype>
#include <mutex>
#include <string_view>
#include <utility>

#include "tracy/Tracy.hpp"

namespace aurora::gx {
using namespace fmt::literals;
using namespace std::string_literals;
using namespace std::string_view_literals;

namespace {
constexpr Module Log{"aurora::gfx::gx"};

std::string_view chan_comp(GXTevColorChan chan) noexcept {
  switch (chan) {
  case GX_CH_RED:
    return "r";
  case GX_CH_GREEN:
    return "g";
  case GX_CH_BLUE:
    return "b";
  case GX_CH_ALPHA:
    return "a";
  default:
    return "?";
  }
}

bool is_alpha_bump_channel(GXChannelID id) noexcept { return id == GX_ALPHA_BUMP || id == GX_ALPHA_BUMPN; }

std::string tev_mask_expr(const std::string& value, u32 mask) {
  // t_IndTexCoord is already expanded into the 0..255 indirect sample domain.
  return fmt::format("(f32(u32({}) & 0x{:X}u) / 255.0)", value, mask);
}

std::string alpha_bump_sel(size_t stageIdx, const ShaderConfig& config, const TevStage& stage) {
  if (stage.indTexStage >= config.numIndStages || stage.indTexAlphaSel == GX_ITBA_OFF) {
    return "0.0";
  }

  std::string baseCoord;
  switch (stage.indTexAlphaSel) {
    DEFAULT_FATAL("invalid indTexAlphaSel {} for stage {}", underlying(stage.indTexAlphaSel), stageIdx);
  case GX_ITBA_S:
    baseCoord = fmt::format("t_IndTexCoord{}.x", underlying(stage.indTexStage));
    break;
  case GX_ITBA_T:
    baseCoord = fmt::format("t_IndTexCoord{}.y", underlying(stage.indTexStage));
    break;
  case GX_ITBA_U:
    baseCoord = fmt::format("t_IndTexCoord{}.z", underlying(stage.indTexStage));
    break;
  case GX_ITBA_OFF:
    return "0.0";
  }

  switch (stage.indTexFormat) {
    DEFAULT_FATAL("invalid indirect format {} for stage {}", underlying(stage.indTexFormat), stageIdx);
  case GX_ITF_8:
    return tev_mask_expr(baseCoord, 0xF8u);
  case GX_ITF_5:
    return tev_mask_expr(baseCoord, 0xE0u);
  case GX_ITF_4:
    return tev_mask_expr(baseCoord, 0xF0u);
  case GX_ITF_3:
    return tev_mask_expr(baseCoord, 0xF8u);
  }
}

bool uses_texture_sample(const TevStage& stage) noexcept {
  if (stage.texMapId == GX_TEXMAP_NULL) {
    return false;
  }
  const auto& c = stage.colorPass;
  const auto& a = stage.alphaPass;
  return c.a == GX_CC_TEXC || c.a == GX_CC_TEXA || c.b == GX_CC_TEXC || c.b == GX_CC_TEXA || c.c == GX_CC_TEXC ||
         c.c == GX_CC_TEXA || c.d == GX_CC_TEXC || c.d == GX_CC_TEXA || a.a == GX_CA_TEXA || a.b == GX_CA_TEXA ||
         a.c == GX_CA_TEXA || a.d == GX_CA_TEXA;
}

std::string color_arg_reg(GXTevColorArg arg, size_t stageIdx, const ShaderConfig& config, const TevStage& stage) {
  switch (arg) {
    DEFAULT_FATAL("invalid color arg {}", underlying(arg));
  case GX_CC_CPREV:
    return "prev.rgb";
  case GX_CC_APREV:
    return "vec3f(prev.a)";
  case GX_CC_C0:
    return "tevreg0.rgb";
  case GX_CC_A0:
    return "vec3f(tevreg0.a)";
  case GX_CC_C1:
    return "tevreg1.rgb";
  case GX_CC_A1:
    return "vec3f(tevreg1.a)";
  case GX_CC_C2:
    return "tevreg2.rgb";
  case GX_CC_A2:
    return "vec3f(tevreg2.a)";
  case GX_CC_TEXC: {
    if (stage.texMapId == GX_TEXMAP_NULL) {
      return "vec3f(1.0)";
    }
    CHECK(stage.texMapId >= GX_TEXMAP0 && stage.texMapId <= GX_TEXMAP7, "invalid texture {} for stage {}",
          underlying(stage.texMapId), stageIdx);
    const auto& swap = config.tevSwapTable[stage.tevSwapTex];
    return fmt::format("sampled{}.{}{}{}", stageIdx, chan_comp(swap.red), chan_comp(swap.green), chan_comp(swap.blue));
  }
  case GX_CC_TEXA: {
    if (stage.texMapId == GX_TEXMAP_NULL) {
      return "vec3f(1.0)";
    }
    CHECK(stage.texMapId >= GX_TEXMAP0 && stage.texMapId <= GX_TEXMAP7, "invalid texture {} for stage {}",
          underlying(stage.texMapId), stageIdx);
    const auto& swap = config.tevSwapTable[stage.tevSwapTex];
    return fmt::format("vec3f(sampled{}.{})", stageIdx, chan_comp(swap.alpha));
  }
  case GX_CC_RASC: {
    // CHECK(stage.channelId != GX_COLOR_NULL, "unmapped color channel for stage {}", stageIdx);
    if (stage.channelId == GX_COLOR_ZERO || stage.channelId == GX_COLOR_NULL) {
      return "vec3f(0.0)";
    }
    if (is_alpha_bump_channel(stage.channelId)) {
      std::string alpha = alpha_bump_sel(stageIdx, config, stage);
      if (stage.channelId == GX_ALPHA_BUMPN) {
        alpha = fmt::format("({} * (255.0 / 248.0))", alpha);
      }
      return fmt::format("vec3f({})", alpha);
    }
    u32 idx = color_channel(stage.channelId);
    const auto& swap = config.tevSwapTable[stage.tevSwapRas];
    return fmt::format("rast{}.{}{}{}", idx, chan_comp(swap.red), chan_comp(swap.green), chan_comp(swap.blue));
  }
  case GX_CC_RASA: {
    // CHECK(stage.channelId != GX_COLOR_NULL, "unmapped color channel for stage {}", stageIdx);
    if (stage.channelId == GX_COLOR_ZERO || stage.channelId == GX_COLOR_NULL) {
      return "vec3f(0.0)";
    }
    if (is_alpha_bump_channel(stage.channelId)) {
      std::string alpha = alpha_bump_sel(stageIdx, config, stage);
      if (stage.channelId == GX_ALPHA_BUMPN) {
        alpha = fmt::format("({} * (255.0 / 248.0))", alpha);
      }
      return fmt::format("vec3f({})", alpha);
    }
    u32 idx = color_channel(stage.channelId);
    const auto& swap = config.tevSwapTable[stage.tevSwapRas];
    return fmt::format("vec3f(rast{}.{})", idx, chan_comp(swap.alpha));
  }
  case GX_CC_ONE:
    return "vec3f(1.0)";
  case GX_CC_HALF:
    return "vec3f(0.5)";
  case GX_CC_KONST: {
    switch (stage.kcSel) {
      DEFAULT_FATAL("invalid kcSel {}", underlying(stage.kcSel));
    case GX_TEV_KCSEL_8_8:
      return "vec3f(1.0)";
    case GX_TEV_KCSEL_7_8:
      return "vec3f(7.0/8.0)";
    case GX_TEV_KCSEL_6_8:
      return "vec3f(6.0/8.0)";
    case GX_TEV_KCSEL_5_8:
      return "vec3f(5.0/8.0)";
    case GX_TEV_KCSEL_4_8:
      return "vec3f(4.0/8.0)";
    case GX_TEV_KCSEL_3_8:
      return "vec3f(3.0/8.0)";
    case GX_TEV_KCSEL_2_8:
      return "vec3f(2.0/8.0)";
    case GX_TEV_KCSEL_1_8:
      return "vec3f(1.0/8.0)";
    case GX_TEV_KCSEL_K0:
      return "ubuf.kcolor0.rgb";
    case GX_TEV_KCSEL_K1:
      return "ubuf.kcolor1.rgb";
    case GX_TEV_KCSEL_K2:
      return "ubuf.kcolor2.rgb";
    case GX_TEV_KCSEL_K3:
      return "ubuf.kcolor3.rgb";
    case GX_TEV_KCSEL_K0_R:
      return "vec3f(ubuf.kcolor0.r)";
    case GX_TEV_KCSEL_K1_R:
      return "vec3f(ubuf.kcolor1.r)";
    case GX_TEV_KCSEL_K2_R:
      return "vec3f(ubuf.kcolor2.r)";
    case GX_TEV_KCSEL_K3_R:
      return "vec3f(ubuf.kcolor3.r)";
    case GX_TEV_KCSEL_K0_G:
      return "vec3f(ubuf.kcolor0.g)";
    case GX_TEV_KCSEL_K1_G:
      return "vec3f(ubuf.kcolor1.g)";
    case GX_TEV_KCSEL_K2_G:
      return "vec3f(ubuf.kcolor2.g)";
    case GX_TEV_KCSEL_K3_G:
      return "vec3f(ubuf.kcolor3.g)";
    case GX_TEV_KCSEL_K0_B:
      return "vec3f(ubuf.kcolor0.b)";
    case GX_TEV_KCSEL_K1_B:
      return "vec3f(ubuf.kcolor1.b)";
    case GX_TEV_KCSEL_K2_B:
      return "vec3f(ubuf.kcolor2.b)";
    case GX_TEV_KCSEL_K3_B:
      return "vec3f(ubuf.kcolor3.b)";
    case GX_TEV_KCSEL_K0_A:
      return "vec3f(ubuf.kcolor0.a)";
    case GX_TEV_KCSEL_K1_A:
      return "vec3f(ubuf.kcolor1.a)";
    case GX_TEV_KCSEL_K2_A:
      return "vec3f(ubuf.kcolor2.a)";
    case GX_TEV_KCSEL_K3_A:
      return "vec3f(ubuf.kcolor3.a)";
    }
  }
  case GX_CC_ZERO:
    return "vec3f(0.0)";
  }
}

std::string alpha_arg_reg(GXTevAlphaArg arg, size_t stageIdx, const ShaderConfig& config, const TevStage& stage) {
  switch (arg) {
    DEFAULT_FATAL("invalid alpha arg {}", underlying(arg));
  case GX_CA_APREV:
    return "prev.a";
  case GX_CA_A0:
    return "tevreg0.a";
  case GX_CA_A1:
    return "tevreg1.a";
  case GX_CA_A2:
    return "tevreg2.a";
  case GX_CA_TEXA: {
    if (stage.texMapId == GX_TEXMAP_NULL) {
      return "1.0";
    }
    CHECK(stage.texMapId >= GX_TEXMAP0 && stage.texMapId <= GX_TEXMAP7, "invalid texture {} for stage {}",
          underlying(stage.texMapId), stageIdx);
    const auto& swap = config.tevSwapTable[stage.tevSwapTex];
    return fmt::format("sampled{}.{}", stageIdx, chan_comp(swap.alpha));
  }
  case GX_CA_RASA: {
    // CHECK(stage.channelId != GX_COLOR_NULL, "unmapped color channel for stage {}", stageIdx);
    if (stage.channelId == GX_COLOR_ZERO || stage.channelId == GX_COLOR_NULL) {
      return "0.0";
    }
    if (is_alpha_bump_channel(stage.channelId)) {
      std::string alpha = alpha_bump_sel(stageIdx, config, stage);
      if (stage.channelId == GX_ALPHA_BUMPN) {
        alpha = fmt::format("({} * (255.0 / 248.0))", alpha);
      }
      return alpha;
    }
    u32 idx = color_channel(stage.channelId);
    const auto& swap = config.tevSwapTable[stage.tevSwapRas];
    return fmt::format("rast{}.{}", idx, chan_comp(swap.alpha));
  }
  case GX_CA_KONST: {
    switch (stage.kaSel) {
      DEFAULT_FATAL("invalid kaSel {}", underlying(stage.kaSel));
    case GX_TEV_KASEL_8_8:
      return "1.0";
    case GX_TEV_KASEL_7_8:
      return "(7.0/8.0)";
    case GX_TEV_KASEL_6_8:
      return "(6.0/8.0)";
    case GX_TEV_KASEL_5_8:
      return "(5.0/8.0)";
    case GX_TEV_KASEL_4_8:
      return "(4.0/8.0)";
    case GX_TEV_KASEL_3_8:
      return "(3.0/8.0)";
    case GX_TEV_KASEL_2_8:
      return "(2.0/8.0)";
    case GX_TEV_KASEL_1_8:
      return "(1.0/8.0)";
    case GX_TEV_KASEL_K0_R:
      return "ubuf.kcolor0.r";
    case GX_TEV_KASEL_K1_R:
      return "ubuf.kcolor1.r";
    case GX_TEV_KASEL_K2_R:
      return "ubuf.kcolor2.r";
    case GX_TEV_KASEL_K3_R:
      return "ubuf.kcolor3.r";
    case GX_TEV_KASEL_K0_G:
      return "ubuf.kcolor0.g";
    case GX_TEV_KASEL_K1_G:
      return "ubuf.kcolor1.g";
    case GX_TEV_KASEL_K2_G:
      return "ubuf.kcolor2.g";
    case GX_TEV_KASEL_K3_G:
      return "ubuf.kcolor3.g";
    case GX_TEV_KASEL_K0_B:
      return "ubuf.kcolor0.b";
    case GX_TEV_KASEL_K1_B:
      return "ubuf.kcolor1.b";
    case GX_TEV_KASEL_K2_B:
      return "ubuf.kcolor2.b";
    case GX_TEV_KASEL_K3_B:
      return "ubuf.kcolor3.b";
    case GX_TEV_KASEL_K0_A:
      return "ubuf.kcolor0.a";
    case GX_TEV_KASEL_K1_A:
      return "ubuf.kcolor1.a";
    case GX_TEV_KASEL_K2_A:
      return "ubuf.kcolor2.a";
    case GX_TEV_KASEL_K3_A:
      return "ubuf.kcolor3.a";
    }
  }
  case GX_CA_ZERO:
    return "0.0";
  }
}

bool tev_color_arg_is_normalized(GXTevColorArg arg, const std::array<bool, MaxTevRegs>& colorNormalized,
                                 const std::array<bool, MaxTevRegs>& alphaNormalized) {
  switch (arg) {
  case GX_CC_CPREV:
    return colorNormalized[GX_TEVPREV];
  case GX_CC_APREV:
    return alphaNormalized[GX_TEVPREV];
  case GX_CC_C0:
    return colorNormalized[GX_TEVREG0];
  case GX_CC_A0:
    return alphaNormalized[GX_TEVREG0];
  case GX_CC_C1:
    return colorNormalized[GX_TEVREG1];
  case GX_CC_A1:
    return alphaNormalized[GX_TEVREG1];
  case GX_CC_C2:
    return colorNormalized[GX_TEVREG2];
  case GX_CC_A2:
    return alphaNormalized[GX_TEVREG2];
  default:
    return true;
  }
}

bool tev_alpha_arg_is_normalized(GXTevAlphaArg arg, const std::array<bool, MaxTevRegs>& alphaNormalized) {
  switch (arg) {
  case GX_CA_APREV:
    return alphaNormalized[GX_TEVPREV];
  case GX_CA_A0:
    return alphaNormalized[GX_TEVREG0];
  case GX_CA_A1:
    return alphaNormalized[GX_TEVREG1];
  case GX_CA_A2:
    return alphaNormalized[GX_TEVREG2];
  default:
    return true;
  }
}

std::string tev_op(GXTevOp op, std::string_view bias, std::string_view scale, std::string_view a, std::string_view b,
                   std::string_view c, std::string_view d, std::string_view zero) {
  switch (op) {
    DEFAULT_FATAL("unimplemented tev op {}", underlying(op));
  case GX_TEV_ADD:
  case GX_TEV_SUB: {
    std::string_view neg = op == GX_TEV_SUB ? "-"sv : ""sv;
    return fmt::format("(({0}mix({1}, {2}, {3}) + {4}){5}){6}", neg, a, b, c, d, bias, scale);
  }
  case GX_TEV_COMP_R8_GT:
    return fmt::format("select({3}, {2}, round({0}.r * 255.0) > round({1}.r * 255.0)) + {4}", a, b, c, zero, d);
  case GX_TEV_COMP_R8_EQ:
    return fmt::format("select({3}, {2}, round({0}.r * 255.0) == round({1}.r * 255.0)) + {4}", a, b, c, zero, d);
  case GX_TEV_COMP_GR16_GT:
    return fmt::format(
        "select({3}, {2}, round(dot({0}.rg * 255.0, vec2(1.0, 256.0))) > round(dot({1}.rg * 255.0, vec2(1.0, 256.0))))"
        " + {4}",
        a, b, c, zero, d);
  case GX_TEV_COMP_GR16_EQ:
    return fmt::format(
        "select({3}, {2}, round(dot({0}.rg * 255.0, vec2(1.0, 256.0))) == round(dot({1}.rg * 255.0, vec2(1.0, 256.0))))"
        " + {4}",
        a, b, c, zero, d);
  case GX_TEV_COMP_BGR24_GT:
    return fmt::format(
        "select({3}, {2}, round(dot({0}.rgb * 255.0, vec3(1.0, 256.0, 65536.0))) > round(dot({1}.rgb * 255.0, "
        "vec3(1.0, 256.0, 65536.0)))) + {4}",
        a, b, c, zero, d);
  case GX_TEV_COMP_BGR24_EQ:
    return fmt::format(
        "select({3}, {2}, round(dot({0}.rgb * 255.0, vec3(1.0, 256.0, 65536.0))) == round(dot({1}.rgb * 255.0, "
        "vec3(1.0, 256.0, 65536.0)))) + {4}",
        a, b, c, zero, d);
  case GX_TEV_COMP_RGB8_GT:
    return fmt::format("select({3}, {2}, round({0} * 255.0) > round({1} * 255.0)) + {4}", a, b, c, zero, d);
  case GX_TEV_COMP_RGB8_EQ:
    return fmt::format("select({3}, {2}, round({0} * 255.0) == round({1} * 255.0)) + {4}", a, b, c, zero, d);
  }
}

std::string tev_color_op(GXTevOp op, std::string_view bias, std::string_view scale, bool clamp, std::string_view a,
                         std::string_view b, std::string_view c, std::string_view d) {
  std::string expr = tev_op(op, bias, scale, a, b, c, d, "vec3(0)"sv);
  return clamp ? fmt::format("clamp({}, vec3f(0.0), vec3f(1.0))", expr)
               : fmt::format("clamp({}, vec3f(-4.0), vec3f(4.0))", expr);
}

std::string tev_alpha_op(GXTevOp op, std::string_view bias, std::string_view scale, bool clamp, std::string_view a,
                         std::string_view b, std::string_view c, std::string_view d) {
  std::string expr = tev_op(op, bias, scale, a, b, c, d, "0.0"sv);
  return clamp ? fmt::format("clamp({}, 0.0, 1.0)", expr) : fmt::format("clamp({}, -4.0, 4.0)", expr);
}

std::string_view tev_bias(GXTevBias bias) {
  switch (bias) {
    DEFAULT_FATAL("invalid tev bias {}", underlying(bias));
  case GX_TB_ZERO:
    return ""sv;
  case GX_TB_ADDHALF:
    return " + 0.5"sv;
  case GX_TB_SUBHALF:
    return " - 0.5"sv;
  }
}

struct AlphaCompareExpr {
  std::string expr;
  int constant = -1;
};

AlphaCompareExpr alpha_compare_const(bool value) { return {value ? "true"s : "false"s, value ? 1 : 0}; }

AlphaCompareExpr alpha_compare_not(const AlphaCompareExpr& expr) {
  if (expr.constant != -1) {
    return alpha_compare_const(expr.constant == 0);
  }
  return {fmt::format("!{}", expr.expr), -1};
}

AlphaCompareExpr alpha_compare_and(const AlphaCompareExpr& lhs, const AlphaCompareExpr& rhs) {
  if (lhs.constant == 0 || rhs.constant == 0) {
    return alpha_compare_const(false);
  }
  if (lhs.constant == 1) {
    return rhs;
  }
  if (rhs.constant == 1) {
    return lhs;
  }
  return {fmt::format("({} && {})", lhs.expr, rhs.expr), -1};
}

AlphaCompareExpr alpha_compare_or(const AlphaCompareExpr& lhs, const AlphaCompareExpr& rhs) {
  if (lhs.constant == 1 || rhs.constant == 1) {
    return alpha_compare_const(true);
  }
  if (lhs.constant == 0) {
    return rhs;
  }
  if (rhs.constant == 0) {
    return lhs;
  }
  return {fmt::format("({} || {})", lhs.expr, rhs.expr), -1};
}

AlphaCompareExpr alpha_compare_xor(const AlphaCompareExpr& lhs, const AlphaCompareExpr& rhs) {
  if (lhs.constant != -1 && rhs.constant != -1) {
    return alpha_compare_const(lhs.constant != rhs.constant);
  }
  if (lhs.constant == 0) {
    return rhs;
  }
  if (rhs.constant == 0) {
    return lhs;
  }
  if (lhs.constant == 1) {
    return alpha_compare_not(rhs);
  }
  if (rhs.constant == 1) {
    return alpha_compare_not(lhs);
  }
  return {fmt::format("({} != {})", lhs.expr, rhs.expr), -1};
}

AlphaCompareExpr alpha_compare_xnor(const AlphaCompareExpr& lhs, const AlphaCompareExpr& rhs) {
  if (lhs.constant != -1 && rhs.constant != -1) {
    return alpha_compare_const(lhs.constant == rhs.constant);
  }
  if (lhs.constant == 0) {
    return alpha_compare_not(rhs);
  }
  if (rhs.constant == 0) {
    return alpha_compare_not(lhs);
  }
  if (lhs.constant == 1) {
    return rhs;
  }
  if (rhs.constant == 1) {
    return lhs;
  }
  return {fmt::format("({} == {})", lhs.expr, rhs.expr), -1};
}

AlphaCompareExpr alpha_compare(GXCompare comp, u8 ref) {
  const auto iref = static_cast<u32>(ref);
  switch (comp) {
    DEFAULT_FATAL("invalid alpha comp {}", underlying(comp));
  case GX_NEVER:
    return alpha_compare_const(false);
  case GX_LESS:
    if (ref == 0) {
      return alpha_compare_const(false);
    }
    return {fmt::format("(alphaCompare < {}u)", iref), -1};
  case GX_LEQUAL:
    if (ref == 255) {
      return alpha_compare_const(true);
    }
    return {fmt::format("(alphaCompare <= {}u)", iref), -1};
  case GX_EQUAL:
    return {fmt::format("(alphaCompare == {}u)", iref), -1};
  case GX_NEQUAL:
    return {fmt::format("(alphaCompare != {}u)", iref), -1};
  case GX_GEQUAL:
    if (ref == 0) {
      return alpha_compare_const(true);
    }
    return {fmt::format("(alphaCompare >= {}u)", iref), -1};
  case GX_GREATER:
    if (ref == 255) {
      return alpha_compare_const(false);
    }
    return {fmt::format("(alphaCompare > {}u)", iref), -1};
  case GX_ALWAYS:
    return alpha_compare_const(true);
  }
}

std::string_view tev_scale(GXTevScale scale) {
  switch (scale) {
    DEFAULT_FATAL("invalid tev scale {}", underlying(scale));
  case GX_CS_SCALE_1:
    return ""sv;
  case GX_CS_SCALE_2:
    return " * 2.0"sv;
  case GX_CS_SCALE_4:
    return " * 4.0"sv;
  case GX_CS_DIVIDE_2:
    return " / 2.0"sv;
  }
}

std::string vtx_attr(const ShaderConfig& config, GXAttr attr) {
  const auto type = config.attrs[attr].attrType;
  if (type == GX_NONE) {
    if (attr == GX_VA_PNMTXIDX) {
      return fmt::format("{}u", config.currentPnMtx);
    }
    if (attr == GX_VA_NRM) {
      // Default normal
      return "vec3f(1.0, 0.0, 0.0)"s;
    }
    if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) {
      return "vec4f(0.0, 0.0, 0.0, 0.0)"s;
    }
    if (attr >= GX_VA_TEX0 && attr <= GX_VA_TEX7) {
      // A texgen reading a UV set the vertices don't carry (seen with a mod model): draw
      // it with zero UVs rather than abort the game. Shaders are cached, so this logs once
      // per shader.
      Log.warn("unmapped vtx attr {} (texcoord {}), using zero UVs", underlying(attr), attr - GX_VA_TEX0);
      return "vec2f(0.0, 0.0)"s;
    }
    UNLIKELY FATAL("unmapped vtx attr {}", underlying(attr));
  }
  if (attr == GX_VA_POS) {
    return "in_pos"s;
  }
  if (attr == GX_VA_NRM) {
    return "in_nrm"s;
  }
  if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1) {
    const auto idx = attr - GX_VA_CLR0;
    return fmt::format("in_clr{}", idx);
  }
  if (attr >= GX_VA_TEX0 && attr <= GX_VA_TEX7) {
    const auto idx = attr - GX_VA_TEX0;
    return fmt::format("in_tex{}_uv", idx);
  }
  if (attr == GX_VA_PNMTXIDX) {
    return "in_pnmtxidx"s;
  }
  if (attr >= GX_VA_TEX0MTXIDX && attr <= GX_VA_TEX7MTXIDX) {
    const auto idx = attr - GX_VA_TEX0MTXIDX;
    return fmt::format("in_texmtxidx{}", idx);
  }
  UNLIKELY FATAL("unhandled vtx attr {}", underlying(attr));
}

constexpr std::array<std::string_view, GX_CC_ZERO + 1> TevColorArgNames{
    "CPREV"sv, "APREV"sv, "C0"sv,   "A0"sv,   "C1"sv,  "A1"sv,   "C2"sv,    "A2"sv,
    "TEXC"sv,  "TEXA"sv,  "RASC"sv, "RASA"sv, "ONE"sv, "HALF"sv, "KONST"sv, "ZERO"sv,
};
constexpr std::array<std::string_view, GX_CA_ZERO + 1> TevAlphaArgNames{
    "APREV"sv, "A0"sv, "A1"sv, "A2"sv, "TEXA"sv, "RASA"sv, "KONST"sv, "ZERO"sv,
};

auto fetch_fixed16_attr(std::string_view fetchFn, const AttrConfig& mapping, std::string_view buf,
                        std::string_view offs, bool le) -> std::string {
  // Some Adreno drivers appear sensitive to generated shaders that route 2- and
  // 3-component fixed-16 vertex attributes through reusable vector fetch helpers.
  // Emitting scalar component fetches at the call site avoids observed artifacts.
  if (mapping.cnt == 2) {
    const auto comp0 = fmt::format("{}_1(&{}, {} + 0u, {}u, {})", fetchFn, buf, offs, mapping.frac, le);
    const auto comp1 = fmt::format("{}_1(&{}, {} + 2u, {}u, {})", fetchFn, buf, offs, mapping.frac, le);
    return fmt::format("vec2f({}, {})", comp0, comp1);
  }
  if (mapping.cnt == 3) {
    const auto comp0 = fmt::format("{}_1(&{}, {} + 0u, {}u, {})", fetchFn, buf, offs, mapping.frac, le);
    const auto comp1 = fmt::format("{}_1(&{}, {} + 2u, {}u, {})", fetchFn, buf, offs, mapping.frac, le);
    const auto comp2 = fmt::format("{}_1(&{}, {} + 4u, {}u, {})", fetchFn, buf, offs, mapping.frac, le);
    return fmt::format("vec3f({}, {}, {})", comp0, comp1, comp2);
  }
  return fmt::format("{}_{}(&{}, {}, {}u, {})", fetchFn, mapping.cnt, buf, offs, mapping.frac, le);
}

auto fetch_attr(const AttrConfig& mapping, std::string_view buf, std::string_view offs, bool le) -> std::string {
  switch (mapping.compType) {
  case GX_U8:
    return fmt::format("fetch_u8_{}(&{}, {}, {}, {})", mapping.cnt, buf, offs, mapping.frac, le);
  case GX_S8:
    return fmt::format("fetch_s8_{}(&{}, {}, {}, {})", mapping.cnt, buf, offs, mapping.frac, le);
  case GX_U16:
    return fetch_fixed16_attr("fetch_u16"sv, mapping, buf, offs, le);
  case GX_S16:
    return fetch_fixed16_attr("fetch_s16"sv, mapping, buf, offs, le);
  case GX_F32:
    return fmt::format("fetch_f32_{}(&{}, {}, {})", mapping.cnt, buf, offs, le);
  case GX_RGBA8:
    return fmt::format("unpack4x8unorm(load_u32_raw(&{}, {}))", buf, offs);
  default:
    Log.fatal("fetch_attr: Unimplemented {}", static_cast<GXCompType>(mapping.compType));
  }
}

auto fetch_color_attr(const AttrConfig& mapping, std::string_view buf, std::string_view offs, bool le) -> std::string {
  switch (mapping.compType) {
  case GX_RGB565:
    return fmt::format("fetch_rgb565(&{}, {}, {})", buf, offs, le);
  case GX_RGB8:
    return fmt::format("fetch_rgb8(&{}, {}, {})", buf, offs, le);
  case GX_RGBX8:
    return fmt::format("fetch_rgbx8(&{}, {}, {})", buf, offs, le);
  case GX_RGBA4:
    return fmt::format("fetch_rgba4(&{}, {}, {})", buf, offs, le);
  case GX_RGBA6:
    return fmt::format("fetch_rgba6(&{}, {}, {})", buf, offs, le);
  case GX_RGBA8:
    return fmt::format("fetch_rgba8(&{}, {}, {})", buf, offs, le);
  default:
    Log.fatal("fetch_color_attr: Unimplemented {}", static_cast<GXCompType>(mapping.compType));
  }
}

struct AttrAddress {
  std::string offs;
  std::string_view buf;
  bool le;
};

// Immediates cannot contain arrays, so array_start is packed as three vec4u.
std::string imm_array_start(GXAttr attr) noexcept {
  const u32 idx = attr - GX_VA_POS;
  return fmt::format("imm.array_start{}.{}", idx / 4, "xyzw"[idx % 4]);
}

auto attr_address(const AttrConfig& mapping, GXAttr attr, std::string_view vidx, u32 vtxStride, u32 dlExtra, u32 within)
    -> AttrAddress {
  const u32 dlOffset = mapping.offset + dlExtra;
  if (mapping.attrType == GX_INDEX8) {
    return {fmt::format("{} + raw_fetch_u8_1(&vbuf, imm.vtx_start + {} * {}u + {}u) * {}u + {}u", imm_array_start(attr),
                        vidx, vtxStride, dlOffset, mapping.stride, within),
            "abuf"sv, mapping.le};
  }
  if (mapping.attrType == GX_INDEX16) {
    return {fmt::format("{} + raw_fetch_u16_1(&vbuf, imm.vtx_start + {} * {}u + {}u, false) * {}u + {}u",
                        imm_array_start(attr), vidx, vtxStride, dlOffset, mapping.stride, within),
            "abuf"sv, mapping.le};
  }
  // Direct data is big-endian as the game wrote it, or an array element the FIFO
  // processor inlined (GXState::deindexVertices) in that array's order
  return {fmt::format("imm.vtx_start + {} * {}u + {}u", vidx, vtxStride, dlOffset + within), "vbuf"sv, mapping.le};
}

auto attr_load(const ShaderConfig& config, GXAttr attr, std::string_view vidx) -> std::string {
  const auto& mapping = config.attrs[attr];
  if (mapping.attrType == GX_NONE) {
    return vtx_attr(config, attr);
  }
  const auto [offs, buf, le] = attr_address(mapping, attr, vidx, config.vtxStride, 0u, 0u);
  switch (attr) {
  case GX_VA_PNMTXIDX:
    return fmt::format("(raw_fetch_u8_1(&{}, {}) / 3u)", buf, offs);
  case GX_VA_TEX0MTXIDX:
  case GX_VA_TEX1MTXIDX:
  case GX_VA_TEX2MTXIDX:
  case GX_VA_TEX3MTXIDX:
  case GX_VA_TEX4MTXIDX:
  case GX_VA_TEX5MTXIDX:
  case GX_VA_TEX6MTXIDX:
  case GX_VA_TEX7MTXIDX:
    return fmt::format("raw_fetch_u8_1(&{}, {})", buf, offs);
  case GX_VA_POS: {
    const auto posLoad = fetch_attr(mapping, buf, offs, le);
    if (mapping.cnt == 2) {
      return fmt::format("vec3f({}, 0.0)", posLoad);
    }
    return posLoad;
  }
  case GX_VA_NRM:
    // NBT: normal only here; binormal/tangent loaded via attr_load_nbt_slice
    if (mapping.cnt > 3) {
      auto nrmMapping = mapping;
      nrmMapping.cnt = 3;
      return fetch_attr(nrmMapping, buf, offs, le);
    }
    return fetch_attr(mapping, buf, offs, le);
  case GX_VA_CLR0:
  case GX_VA_CLR1:
    return fetch_color_attr(mapping, buf, offs, le);
  case GX_VA_TEX0:
  case GX_VA_TEX1:
  case GX_VA_TEX2:
  case GX_VA_TEX3:
  case GX_VA_TEX4:
  case GX_VA_TEX5:
  case GX_VA_TEX6:
  case GX_VA_TEX7: {
    const auto texLoad = fetch_attr(mapping, buf, offs, le);
    if (mapping.cnt == 1) {
      return fmt::format("vec2f({}, 0.0)", texLoad);
    }
    return texLoad;
  }
  default:
    Log.fatal("attr_load: Unimplemented {}", attr);
  }
}

enum class NbtSlice : u8 {
  N,
  B,
  T,
};

auto attr_load_nbt_slice(const ShaderConfig& config, NbtSlice slice, std::string_view vidx) -> std::string {
  const auto& mapping = config.attrs[GX_VA_NRM];
  if (mapping.attrType == GX_NONE || mapping.cnt != 9) {
    Log.fatal("attr_load_nbt_slice: GX_TG_BINRM/TANGENT requires GX_NRM_NBT or GX_NRM_NBT3");
  }
  const auto sliceIdx = static_cast<u32>(slice);
  const auto compsize = comp_type_size(GX_VA_NRM, static_cast<GXCompType>(mapping.compType));
  u32 dlExtra = 0;
  if (mapping.nbt3) {
    if (mapping.attrType == GX_INDEX8) {
      dlExtra = sliceIdx;
    } else if (mapping.attrType == GX_INDEX16) {
      dlExtra = sliceIdx * 2u;
    }
  }
  const u32 within = sliceIdx * 3u * compsize;
  const auto [offs, buf, le] = attr_address(mapping, GX_VA_NRM, vidx, config.vtxStride, dlExtra, within);
  auto sliceMapping = mapping;
  sliceMapping.cnt = 3;
  return fetch_attr(sliceMapping, buf, offs, le);
}

constexpr std::string_view nbt_slice_local(NbtSlice slice) noexcept {
  return slice == NbtSlice::B ? "in_binrm" : "in_tangent";
}

constexpr bool is_emboss_texgen(GXTexGenType type) noexcept { return type >= GX_TG_BUMP0 && type <= GX_TG_BUMP7; }

auto lighting_func(const ShaderConfig& config, const ColorChannelConfig& cc, u8 i, bool alpha) -> std::string {
  std::string_view swizzle = alpha ? ".a"sv : ""sv;
  std::string outVar;
  std::string_view posVar;
  if (UsePerPixelLighting) {
    outVar = fmt::format("rast{}", i);
    posVar = "in.mv_pos"sv;
  } else {
    outVar = fmt::format("out.cc{}", i);
    posVar = "mv_pos"sv;
  }
  std::string ambSrc, matSrc;
  if (cc.ambSrc == GX_SRC_VTX) {
    if (UsePerPixelLighting) {
      ambSrc = fmt::format("in.clr{}", i);
    } else {
      ambSrc = vtx_attr(config, static_cast<GXAttr>(GX_VA_CLR0 + i));
    }
  } else if (cc.ambSrc == GX_SRC_REG) {
    ambSrc = fmt::format("ubuf.cc{0}{1}_amb", i, alpha ? "a"sv : ""sv);
  }
  if (cc.matSrc == GX_SRC_VTX) {
    if (UsePerPixelLighting) {
      matSrc = fmt::format("in.clr{}", i);
    } else {
      matSrc = vtx_attr(config, static_cast<GXAttr>(GX_VA_CLR0 + i));
    }
  } else if (cc.matSrc == GX_SRC_REG) {
    matSrc = fmt::format("ubuf.cc{0}{1}_mat", i, alpha ? "a"sv : ""sv);
  }
  if (!cc.lightingEnabled) {
    return fmt::format("\n    {0}{2} = {1}{2};", outVar, matSrc, swizzle);
  }
  GXDiffuseFn diffFn = cc.diffFn;
  std::string lightAttnFn;
  if (cc.attnFn == GX_AF_NONE) {
    lightAttnFn = "attn = 1.0;"s;
  } else if (cc.attnFn == GX_AF_SPOT) {
    lightAttnFn = fmt::format(R"""(
          var cosine = max(0.0, dot(ldir, light.dir));
          var cos_attn = dot(light.cos_att, vec3f(1.0, cosine, cosine * cosine));
          var dist_attn = dot(light.dist_att, vec3f(1.0, dist, dist2));
          attn = max(0.0, cos_attn / dist_attn);)""");
  } else if (cc.attnFn == GX_AF_SPEC) {
    std::string_view normal = UsePerPixelLighting ? "in.mv_nrm"sv : "mv_nrm"sv;
    std::string dist_attn = diffFn != GX_DF_NONE
                                ? "max(0.0, dot(normalize(light.dist_att), vec3f(1.0, attn, attn * attn)));"
                                : "max(0.0, dot(light.dist_att, vec3f(1.0, attn, attn * attn)));";
    lightAttnFn = fmt::format(R"""(
          attn = select(0.0, max(0.0, dot({0}, light.dir)), dot({0}, ldir) >= 0.0);
          var cos_attn = dot(light.cos_att, vec3f(1.0, attn, attn * attn));
          var dist_attn = {1};
          attn = max(0.0, cos_attn / dist_attn);)""",
                              normal, dist_attn);
  }
  std::string_view lightDiffFn;
  if (diffFn == GX_DF_NONE) {
    lightDiffFn = "1.0"sv;
  } else if (diffFn == GX_DF_SIGN) {
    if (UsePerPixelLighting) {
      lightDiffFn = "dot(ldir, in.mv_nrm)"sv;
    } else {
      lightDiffFn = "dot(ldir, mv_nrm)"sv;
    }
  } else if (diffFn == GX_DF_CLAMP) {
    if (UsePerPixelLighting) {
      lightDiffFn = "max(0.0, dot(ldir, in.mv_nrm))"sv;
    } else {
      lightDiffFn = "max(0.0, dot(ldir, mv_nrm))"sv;
    }
  }
  return fmt::format(R"""(
    {{
      var lighting = {5};
      for (var i = 0u; i < {1}u; i++) {{
          if ((ubuf.lightState{0}{9} & (1u << i)) == 0u) {{ continue; }}
          var light = ubuf.lights[i];
          var ldir = light.pos - {6};
          var dist2 = dot(ldir, ldir);
          var dist = sqrt(dist2);
          ldir = ldir / dist;
          var attn: f32;{2}
          var diff = {3};
          lighting = lighting + (attn * diff * light.color);
      }}
      {7}{8} = ({4} * clamp(lighting, vec4f(0.0), vec4f(1.0))){8};
    }})""",
                     i, GX::MaxLights, lightAttnFn, lightDiffFn, matSrc, ambSrc, posVar, outVar, swizzle,
                     alpha ? "a"sv : ""sv);
}

// GX_AURORA_SET_PBR: replaces the TEV colour result with a metal/roughness evaluation of
// texture maps 0-3 (base colour, glTF occlusion/roughness/metal in R/G/B, two-channel
// tangent-space normal, emissive) lit by channel 0's GX lights and ambient. TEV alpha is
// kept. The lobe is Metroid Prime Remastered's, as its model shader evaluates it: GGX with
// alpha = roughness^2 floored at roughness 0.02, visibility 1/4((N.L(1-k)+k)(N.V(1-k)+k))
// with k = alpha/2, Schlick Fresnel on the specular only, and occlusion on the direct
// diffuse as well as the ambient. Each map is read from the TEV stage that samples it, so the material's fallback
// TEV must reference all four; a missing map gets a neutral default. The tangent frame
// comes from screen-space derivatives, so no tangent attribute is needed. Maths is done
// on linearised colours and converted back, since the rest of the pipeline is gamma.
// Channel 1's lights (bar the model shadow's, below) and a vertex-sourced ambient are not
// used (vertex ambient falls back to a constant). With channel 0 unlit there are no lights
// to sum and the channel's material colour, which is all an unlit surface shows, is the ambient; the game draws a model that
// way when no light reaches it, and a room lit by an ambient volume always. A material
// without channel 0 keeps its TEV result. A vertex colour, where the vertex format has
// one, multiplies the diffuse albedo (not the specular) as a linear value, and its alpha
// the output's: that is what Remastered's shader does with it.
auto pbr_func(const ShaderConfig& config, const ShaderInfo& info, std::string& vtxOutAttrs,
              std::string& vtxXfrAttrs, size_t& vtxOutIdx) -> std::string {
  const auto& cc = config.colorChannels[GX_COLOR0];
  if (!info.sampledColorChannels.test(0)) {
    return {};
  }
  const bool lit = info.lightingEnabled && cc.lightingEnabled;
  // The game's projected shadow (CCubeMaterial's model shadow) takes the first stage:
  // channel 1, lit by the shadow-casting light alone, times the shadow map. That light
  // is then summed with channel 0's lights, scaled by the map's sample.
  const bool shadowed = config.tevStageCount > 2 && config.tevStages[0].channelId == GX_COLOR1A1 &&
                        config.tevStages[0].texMapId != GX_TEXMAP_NULL &&
                        config.tevStages[0].texCoordId != GX_TEXCOORD_NULL &&
                        config.colorChannels[GX_COLOR1].lightingEnabled;
  std::array<int, 7> mapStage{-1, -1, -1, -1, -1, -1, -1};
  for (int i = shadowed ? 1 : 0; i < config.tevStageCount; ++i) {
    const auto& stage = config.tevStages[i];
    const u32 map = underlying(stage.texMapId);
    if (map < mapStage.size() && mapStage[map] == -1 && uses_texture_sample(stage) &&
        stage.texCoordId != GX_TEXCOORD_NULL && stage.indTexMtxId == GX_ITM_OFF) {
      mapStage[map] = i;
    }
  }
  if (mapStage[0] == -1) {
    return {};
  }
  // Glass (kind 8) also samples map 7, a copy of what is behind it on screen. The
  // shadow's stage samples map 7 too, so a shadowed surface goes without.
  bool screen = false;
  for (int i = 0; i < config.tevStageCount && !shadowed; ++i) {
    const auto& stage = config.tevStages[i];
    if (stage.texMapId == GX_TEXMAP7 && uses_texture_sample(stage) && stage.texCoordId != GX_TEXCOORD_NULL) {
      screen = true;
    }
  }
  const auto sampled = [&](int map, std::string_view fallback) {
    return mapStage[map] == -1 ? std::string(fallback) : fmt::format("sampled{}", mapStage[map]);
  };
  vtxOutAttrs += fmt::format("\n    @location({}) pbr_pos: vec3f,", vtxOutIdx++);
  vtxOutAttrs += fmt::format("\n    @location({}) pbr_nrm: vec3f,", vtxOutIdx++);
  vtxXfrAttrs += "\n    out.pbr_pos = mv_pos;\n    out.pbr_nrm = mv_nrm;";
  // A vertex colour is the surface's tint where the material says so (mode 4), whatever
  // the channel does with it: the game points an unlit channel at its material register,
  // which would lose it. A retail model's colours are not a tint.
  // A special surface (the kind in pbr_layer.y) reads the colour its own way, tint or not.
  std::string tint, tintAlpha, vclr = "vec4f(1.0)";
  if (config.attrs[GX_VA_CLR0].attrType != GX_NONE) {
    vtxOutAttrs += fmt::format("\n    @location({}) pbr_vclr: vec4f,", vtxOutIdx++);
    vtxXfrAttrs += fmt::format("\n    out.pbr_vclr = {};", vtx_attr(config, GX_VA_CLR0));
    tint = " * pbr_vc.rgb";
    tintAlpha = " * pbr_vc.a";
    vclr = "in.pbr_vclr";
  }

  // A second layer (maps 4-6: base, MR, normal) over the first. The vertex alpha says
  // where, and the two base maps' alphas are heights that decide which layer shows
  // through first across the edge: this is Remastered's blend.
  const bool layered = mapStage[4] != -1;
  std::string base = sampled(0, ""), orm = sampled(1, "vec4f(1.0, 0.6, 0.0, 1.0)");
  std::string normalXy = mapStage[2] == -1 ? std::string() : fmt::format("sampled{}.rg", mapStage[2]);
  std::string layer = fmt::format(R"""(
      let pbr_ng = normalize(in.pbr_nrm);
      let pbr_kind = ubuf.pbr_layer.y;
      let pbr_vraw = {};
      let pbr_vc = select(vec4f(1.0), pbr_vraw, ubuf.pbr_backlight.w > 3.5);)""",
                                  vclr);
  const bool framed = mapStage[2] != -1 || layered;
  if (framed) {
    // Cotangent frame (Schüler): pbr_t and pbr_b are the directions U and V grow in. WebGPU's
    // framebuffer Y runs down, so the Y derivatives are negated to get GL's (view space Y up)
    // orientation.
    layer += fmt::format(R"""(
      let pbr_dp1 = dpdx(in.pbr_pos);
      let pbr_dp2 = -dpdy(in.pbr_pos);
      let pbr_duv1 = dpdx(tex{0}_uv);
      let pbr_duv2 = -dpdy(tex{0}_uv);
      let pbr_dp2perp = cross(pbr_dp2, pbr_ng);
      let pbr_dp1perp = cross(pbr_ng, pbr_dp1);
      let pbr_t = pbr_dp2perp * pbr_duv1.x + pbr_dp1perp * pbr_duv2.x;
      let pbr_b = pbr_dp2perp * pbr_duv1.y + pbr_dp1perp * pbr_duv2.y;
      let pbr_tlen = max(dot(pbr_t, pbr_t), dot(pbr_b, pbr_b));)""",
                         underlying(config.tevStages[mapStage[mapStage[2] != -1 ? 2 : 0]].texCoordId));
  }
  std::string kinds = "\n      var pbr_kglow = vec3f(0.0);";
  if (layered) {
    // Kind 1 lays the second layer on what faces up: the weight is the first layer's own
    // normal along world up, lifted by the vertex alpha.
    std::string first = "\n              var pbr_n1 = pbr_ng;";
    if (mapStage[2] != -1) {
      first += fmt::format(R"""(
              let pbr_ts1 = sampled{}.rg * 2.0 - 1.0;
              if (pbr_tlen > 1e-24) {{
                  let pbr_s1 = inverseSqrt(pbr_tlen);
                  pbr_n1 = normalize(pbr_t * (pbr_s1 * pbr_ts1.x) - pbr_b * (pbr_s1 * pbr_ts1.y) +
                                     pbr_ng * sqrt(max(0.0, 1.0 - dot(pbr_ts1, pbr_ts1))));
              }})""",
                           mapStage[2]);
    }
    const auto& inner = config.tevStages[mapStage[4]];
    // Kind 4 sees map 4 inside the surface: where the view ray is once it has gone the
    // base map's alpha deep. The sample is taken whatever the kind, as one in a branch has
    // no derivatives.
    layer += fmt::format(R"""(
      var pbr_poff = vec2f(0.0);
      if (pbr_kind > 3.5 && pbr_kind < 4.5 && pbr_tlen > 1e-24) {{
          let pbr_pz = min(dot(pbr_ng, in.pbr_pos), -0.05 * length(in.pbr_pos));
          pbr_poff = vec2f(dot(pbr_t * inverseSqrt(max(dot(pbr_t, pbr_t), 1e-30)), in.pbr_pos),
                           dot(pbr_b * inverseSqrt(max(dot(pbr_b, pbr_b), 1e-30)), in.pbr_pos)) *
                     ({0}.a * ubuf.pbr_param.w / pbr_pz);
      }}
      let pbr_inner = pow(max(textureSampleBias(tex{4}, tex{4}_samp, tex{5}_uv + pbr_poff, ubuf.tex{4}_size_bias.z).rgb,
                              vec3f(0.0)), vec3f(2.2));
      var pbr_ls = 0.0;
      if (ubuf.pbr_layer.x > 0.0) {{
          var pbr_lw = pbr_vraw.a * 2.0 - 1.0;
          if (pbr_kind > 0.5 && pbr_kind < 1.5) {{{3}
              let pbr_va2 = max(pbr_vraw.a * 2.0, 1e-4);
              pbr_lw = max(0.0, (dot(pbr_n1, ubuf.pbr_up.xyz) + pbr_va2 - 1.0) / pbr_va2) * 2.0 - 1.0;
          }}
          let pbr_lh = clamp(sampled{2}.a * ubuf.pbr_layer_height.z + ubuf.pbr_layer_height.w, 0.0, 1.0) -
                       clamp({0}.a * ubuf.pbr_layer_height.x + ubuf.pbr_layer_height.y, 0.0, 1.0);
          let pbr_lt = ubuf.pbr_layer.x;
          let pbr_lx = clamp((pbr_lh + pbr_lw * pbr_lt + pbr_lw + pbr_lt) * 0.5 / pbr_lt, 0.0, 1.0);
          pbr_ls = pbr_lx * pbr_lx * (3.0 - 2.0 * pbr_lx);
      }})""",
                         base, 0, mapStage[4], first, underlying(inner.texMapId), underlying(inner.texCoordId));
    // Kind 2: map 4 is a detail map, grey where it leaves the base alone. Kind 4: the
    // inside shows where the surface is seen edge on (a fresnel that also takes it to the
    // vertex colour) and where the base map's alpha says it is clear, and glows there.
    kinds += fmt::format(R"""(
      if (pbr_kind > 1.5 && pbr_kind < 2.5) {{
          pbr_base = pbr_base * pow(max(sampled{1}.rgb, vec3f(0.0)), vec3f(2.2)) * 2.0;
      }}
      if (pbr_kind > 3.5 && pbr_kind < 4.5) {{
          let pbr_kf = pow(clamp(dot(pbr_ng, normalize(-in.pbr_pos)), 0.0, 1.0), max(ubuf.pbr_param.x, 1e-4)) *
                       ubuf.pbr_param.y;
          let pbr_ki = mix(pbr_inner, pbr_vraw.rgb, pbr_kf);
          let pbr_ka = {0}.a * {0}.a * pbr_vraw.a;
          pbr_base = mix(pbr_base, pbr_ki, clamp(pbr_kf + pbr_ka - 1.0, 0.0, 1.0));
          pbr_kglow = pbr_ki * (pbr_ka * ubuf.pbr_layer.z);
      }})""",
                         base, mapStage[4]);
    // Kind 7, falling water: map 4's three channels are sheets that scroll at speeds of
    // their own (pbr_layer_height the first two, pbr_layer.x and pbr_param.y the third,
    // pbr_param.x the time). The vertex colour says how much of each there is, and their
    // sum picks the colour and opacity from map 0, a ramp whose row is the vertex alpha
    // (pbr_param.z and w and pbr_layer.z are the sum's softness and the ramp's offsets,
    // pbr_emissive the colour). It is unlit.
    {
      const auto ramp = underlying(config.tevStages[mapStage[0]].texMapId);
      layer += fmt::format(R"""(
      let pbr_fuv1 = dpdx(tex{0}_uv);
      let pbr_fuv2 = dpdy(tex{0}_uv);)""",
                           underlying(inner.texCoordId));
      kinds += fmt::format(R"""(
      var pbr_kalpha = 1.0;
      if (pbr_kind > 6.5 && pbr_kind < 7.5) {{
          let pbr_ft = ubuf.pbr_param.x;
          let pbr_fr = fract(vec2f(ubuf.pbr_layer_height.x, ubuf.pbr_layer_height.y) * pbr_ft) * vec2f(1.0, -1.0);
          let pbr_fg = fract(vec2f(ubuf.pbr_layer_height.z, ubuf.pbr_layer_height.w) * pbr_ft) * vec2f(1.0, -1.0);
          let pbr_fb = fract(vec2f(ubuf.pbr_layer.x, ubuf.pbr_param.y) * pbr_ft) * vec2f(1.0, -1.0);
          let pbr_fs = vec3f(textureSampleGrad(tex{0}, tex{0}_samp, tex{1}_uv + pbr_fr, pbr_fuv1, pbr_fuv2).r,
                             textureSampleGrad(tex{0}, tex{0}_samp, tex{1}_uv + pbr_fg, pbr_fuv1, pbr_fuv2).g,
                             textureSampleGrad(tex{0}, tex{0}_samp, tex{1}_uv + pbr_fb, pbr_fuv1, pbr_fuv2).b);
          let pbr_fw = max(pbr_fs + pbr_vraw.rgb * 2.0 + ubuf.pbr_param.w - 1.0, vec3f(0.0));
          let pbr_fsum = pbr_fw.x + pbr_fw.y + pbr_fw.z;
          let pbr_framp = textureSampleLevel(tex{2}, tex{2}_samp,
                                             clamp(vec2f(pbr_fsum / max(pbr_fsum + ubuf.pbr_param.z, 1.0),
                                                         pbr_vraw.a + ubuf.pbr_layer.z),
                                                   vec2f(0.02), vec2f(0.98)), 0.0);
          pbr_base = vec3f(0.0);
          pbr_kglow = pow(max(pbr_framp.rgb, vec3f(0.0)), vec3f(2.2)) * max(ubuf.pbr_emissive.rgb, vec3f(0.0));
          pbr_kalpha = pbr_framp.a;
      }})""",
                           underlying(inner.texMapId), underlying(inner.texCoordId), ramp);
    }
    if (mapStage[5] != -1) {
      orm = fmt::format("mix({}, sampled{}, pbr_ls)", orm, mapStage[5]);
    }
    // Glass keeps its roughness in map 4's blue: map 5 is its distortion noise.
    orm = fmt::format("select({}, vec4f(1.0, sampled{}.b, 0.0, 1.0), pbr_kind > 7.5 && pbr_kind < 8.5)", orm,
                      mapStage[4]);
    if (mapStage[6] != -1 && mapStage[2] != -1) {
      normalXy = fmt::format("mix({}, sampled{}.rg, pbr_ls)", normalXy, mapStage[6]);
    }
    // The vertex alpha is the layers' weight there and no opacity.
    tintAlpha.clear();
  }
  // Kinds 5 and 6 are a liquid's surface. Their maps move, so they are sampled at
  // coordinates of their own inside the branch, with the derivatives taken outside it.
  std::string liquid;
  if (layered && mapStage[5] != -1 && mapStage[6] != -1) {
    const auto mapOf = [&](int map) { return underlying(config.tevStages[mapStage[map]].texMapId); };
    const auto uv = underlying(config.tevStages[mapStage[0]].texCoordId);
    layer += fmt::format(R"""(
      let pbr_quv1 = dpdx(tex{0}_uv);
      let pbr_quv2 = dpdy(tex{0}_uv);)""",
                         uv);
    // Kind 6, a lava pool: map 4 is a pattern carried along map 6's flow in two phases half
    // a period apart, each fading out as it wraps, and map 5's noise offsets the phase so
    // that the pool does not pulse as one. The pattern's two channels and the heat (the
    // vertex alpha and the flow's speed) pick the colour from map 0, a ramp. pbr_param is
    // the phase, the flow's reach and the pattern's scale, pbr_layer_height.x the noise's.
    kinds += fmt::format(R"""(
      if (pbr_kind > 5.5 && pbr_kind < 6.5) {{
          let pbr_qs = vec2f(ubuf.pbr_param.z, ubuf.pbr_param.w);
          let pbr_qns = vec2f(pbr_qs.x * ubuf.pbr_layer_height.x / max(pbr_qs.y, 1e-4), ubuf.pbr_layer_height.x);
          let pbr_qnoise = textureSampleGrad(tex{2}, tex{2}_samp, tex{4}_uv * pbr_qns, pbr_quv1 * pbr_qns,
                                             pbr_quv2 * pbr_qns).r;
          let pbr_qflow = sampled{3}.rg * 2.0 - 1.0;
          let pbr_qph = ubuf.pbr_param.x + pbr_qnoise;
          let pbr_qp0 = fract(pbr_qph);
          let pbr_qp1 = fract(pbr_qph + 0.5);
          let pbr_quv = tex{4}_uv * pbr_qs;
          let pbr_qa = textureSampleGrad(tex{1}, tex{1}_samp, pbr_quv - pbr_qflow * (ubuf.pbr_param.y * pbr_qp0),
                                         pbr_quv1 * pbr_qs, pbr_quv2 * pbr_qs).rg;
          let pbr_qb = textureSampleGrad(tex{1}, tex{1}_samp, pbr_quv - pbr_qflow * (ubuf.pbr_param.y * pbr_qp1) + 0.5,
                                         pbr_quv1 * pbr_qs, pbr_quv2 * pbr_qs).rg;
          let pbr_qxy = mix(pbr_qa, pbr_qb, abs(pbr_qp0 - 0.5) * 2.0);
          let pbr_qheat = clamp(pbr_vraw.a * 2.0 + min(length(pbr_qflow), 1.0) - 1.0, 0.0, 1.0);
          let pbr_qd = pbr_qxy.y - pbr_qxy.x;
          let pbr_qt = clamp((pbr_qheat * ubuf.pbr_layer_height.y + pbr_qd) * 2.5 - 2.5, 0.0, 1.0);
          let pbr_qramp = textureSampleLevel(tex{0}, tex{0}_samp,
                                             clamp(vec2f(pbr_qxy.x + pbr_qd * (3.0 - 2.0 * pbr_qt) * pbr_qt * pbr_qt, pbr_qheat),
                                                   vec2f(0.02), vec2f(0.98)), 0.0).rgb;
          pbr_base = vec3f(0.0);
          pbr_kglow = pow(max(pbr_qramp, vec3f(0.0)), vec3f(2.2)) * ubuf.pbr_layer.z;
      }})""",
                         mapOf(0), mapOf(4), mapOf(5), mapStage[6], uv);
    // Kind 5, water: the normal is two copies of map 2 moving across each other
    // (pbr_layer_height is their speeds, pbr_param.x the time, pbr_layer.z the strength).
    // Lava has no normal map.
    if (mapStage[2] != -1) {
      liquid = fmt::format(R"""(
        if (pbr_kind > 4.5 && pbr_kind < 5.5 && pbr_tlen > 1e-24) {{
            let pbr_qw0 = textureSampleGrad(tex{0}, tex{0}_samp, tex{1}_uv + ubuf.pbr_layer_height.xy * ubuf.pbr_param.x,
                                            pbr_quv1, pbr_quv2).rg * 2.0 - 1.0;
            let pbr_qw1 = textureSampleGrad(tex{0}, tex{0}_samp,
                                            tex{1}_uv.yx * 0.73 + ubuf.pbr_layer_height.zw * ubuf.pbr_param.x,
                                            pbr_quv1.yx * 0.73, pbr_quv2.yx * 0.73).gr * 2.0 - 1.0;
            let pbr_qwn = (pbr_qw0 + pbr_qw1) * ubuf.pbr_layer.z;
            let pbr_qws = inverseSqrt(pbr_tlen);
            pbr_n = normalize(pbr_t * (pbr_qws * pbr_qwn.x) - pbr_b * (pbr_qws * pbr_qwn.y) + pbr_ng);
        }})""",
                           mapOf(2), uv);
    }
  }
  const std::string baseRgb =
      layered ? fmt::format("mix(pow(max({0}.rgb, vec3f(0.0)), vec3f(2.2)), pow(max(sampled{1}.rgb, vec3f(0.0)), "
                            "vec3f(2.2)), pbr_ls)",
                            base, mapStage[4])
              : fmt::format("pow(max({}.rgb, vec3f(0.0)), vec3f(2.2))", base);
  std::string normal;
  if (mapStage[2] != -1) {
    // glTF normal maps are +Y up with V running down the image, so the bitangent is the
    // negated dP/dV. Kind 4 scales the map's tilt.
    normal = fmt::format(R"""(
      let pbr_ts = ({0} * 2.0 - 1.0) * select(1.0, ubuf.pbr_param.z, pbr_kind > 3.5 && pbr_kind < 4.5);
      let pbr_tn = vec3f(pbr_ts, sqrt(max(0.0, 1.0 - dot(pbr_ts, pbr_ts))));
      if (pbr_tlen > 1e-24) {{
        let pbr_s = inverseSqrt(pbr_tlen);
        pbr_n = normalize(pbr_t * (pbr_s * pbr_tn.x) - pbr_b * (pbr_s * pbr_tn.y) + pbr_ng * pbr_tn.z);
      }})""",
                         normalXy);
  }
  normal += liquid;
  // And what is seen of it: its own colour in the room's light where it is looked into,
  // the surroundings' reflection where it is seen at a grazing angle (Schlick, pbr_param.w
  // at the normal), and more opaque the more it reflects. pbr_param.z is the opacity seen
  // straight on, pbr_emissive the colour.
  if (!liquid.empty()) {
    liquid = R"""(
      if (pbr_kind > 4.5 && pbr_kind < 5.5) {
          let pbr_qf = ubuf.pbr_param.w + (1.0 - ubuf.pbr_param.w) * pow(1.0 - pbr_nv, 5.0);
          pbr_alpha = mix(ubuf.pbr_param.z, 1.0, pbr_qf);
          pbr_lo = (max(ubuf.pbr_emissive.rgb, vec3f(0.0)) * pbr_ambd * (ubuf.pbr_param.z * (1.0 - pbr_qf)) +
                    pbr_envspec * pbr_qf) / max(pbr_alpha, 1e-3);
          pbr_glow = vec3f(0.0);
      })""";
  }
  if (layered) {
    liquid += R"""(
      if (pbr_kind > 6.5 && pbr_kind < 7.5) {
          pbr_alpha = pbr_kalpha;
      })""";
    // Kind 8, glass (Remastered's 231F8383): map 4 is a mask (red how clear, green the
    // glow, alpha the opacity, each lifted by the vertex colour), and the room behind it
    // (map 7) is seen through it, bent by map 5's noise (pbr_param.x) and tinted
    // (pbr_emissive). pbr_param.z is how fast the clear part loses its opacity, y the
    // reflection's weight, pbr_layer_height the glow's colour and in w the reflection's
    // level. It is drawn premultiplied, so what shows through is added after the tone curve.
    std::string through;
    if (screen) {
      vtxOutAttrs += fmt::format("\n    @location({}) pbr_scr: vec4f,", vtxOutIdx++);
      vtxXfrAttrs += "\n    out.pbr_scr = out.pos;";
      through = fmt::format(R"""(
          let pbr_gn = ({0} - 0.5) * ubuf.pbr_param.x * vec2f(1.0, -1.0);
          let pbr_guv = in.pbr_scr.xy / in.pbr_scr.w * vec2f(0.5, -0.5) + 0.5 + pbr_gn;
          let pbr_gs = textureSampleLevel(tex7, tex7_samp, clamp(pbr_guv, vec2f(0.0), vec2f(1.0)), 0.0).rgb;
          pbr_pass = pow(max(pbr_gs, vec3f(0.0)), vec3f(2.2)) * max(ubuf.pbr_emissive.rgb, vec3f(0.0)) * pbr_gt;)""",
                            mapStage[5] == -1 ? "vec2f(0.5)"s : fmt::format("sampled{}.rg", mapStage[5]));
    }
    liquid += fmt::format(R"""(
      if (pbr_kind > 7.5 && pbr_kind < 8.5) {{
          let pbr_gm = sampled{0};
          let pbr_gt = clamp(pbr_gm.r * pbr_vraw.b + pbr_vraw.r, 0.0, 1.0);
          pbr_alpha = clamp(pbr_gm.a * pow(max(1.0 - pbr_gt, 1e-6), ubuf.pbr_param.z) * pbr_vraw.a, 0.0, 1.0);
          pbr_lo = clamp(pbr_gm.g + pbr_vraw.g, 0.0, 1.0) * max(ubuf.pbr_layer_height.xyz, vec3f(0.0)) +
                   pbr_envspec * (pbr_ab.x * pbr_alpha * ubuf.pbr_param.y + pbr_ab.y) * ubuf.pbr_layer_height.w;
          pbr_glow = vec3f(0.0);{1}
      }})""",
                          mapStage[4], through);
  }
  std::string attn;
  if (cc.attnFn == GX_AF_SPOT) {
    attn = R"""(
          let cosine = max(0.0, dot(ldir, light.dir));
          let cos_attn = dot(light.cos_att, vec3f(1.0, cosine, cosine * cosine));
          let dist_attn = dot(light.dist_att, vec3f(1.0, dist, dist2));
          let attn = max(0.0, cos_attn / dist_attn);)""";
  } else {
    attn = "\n          let attn = 1.0;";
  }
  const std::string amb = lit ? (cc.ambSrc == GX_SRC_REG ? "ubuf.cc0_amb.rgb"s : "vec3f(0.2)"s)
                              : (cc.matSrc == GX_SRC_REG ? "ubuf.cc0_mat.rgb"s : "vec3f(1.0)"s);
  std::string source = fmt::format(R"""(
    // PBR (GX_AURORA_SET_PBR)
    {{
      let pbr_pi = 3.14159265;{10}
      var pbr_base = {11};{13}
      let pbr_orm = {1}.rgb;
      let pbr_ao = pbr_orm.r;
      let pbr_rough = clamp(pbr_orm.g, 0.02, 1.0);
      let pbr_metal = clamp(pbr_orm.b, 0.0, 1.0);
      let pbr_emissive = pow(max({2}.rgb, vec3f(0.0)), vec3f(2.2)) * ubuf.pbr_emissive.rgb;
      var pbr_n = pbr_ng;{3}
      let pbr_v = normalize(-in.pbr_pos);
      let pbr_nv = max(dot(pbr_n, pbr_v), 1e-4);
      let pbr_f0 = mix(vec3f(0.04), pbr_base, pbr_metal);
      let pbr_diff = pbr_base * (1.0 - pbr_metal){8};
      let pbr_a2 = pow(pbr_rough, 4.0);
      let pbr_k = pbr_rough * pbr_rough * 0.5;
      // A normal-mapped reflection can point into the surface; lift it back to the horizon
      // of the geometric normal.
      let pbr_rn = reflect(-pbr_v, pbr_n);
      let pbr_refl = pbr_rn + pbr_ng * clamp(-dot(pbr_ng, pbr_rn), 0.0, 1.0);
      var pbr_lo = vec3f(0.0);
      var pbr_env = vec3f(0.0);
      var pbr_lsum = vec3f(0.0);
      // pbr-lights-begin
      for (var i = 0u; i < {4}u; i++) {{
          if (({15} & (1u << i)) == 0u) {{ continue; }}
          let light = ubuf.lights[i];
          var ldir = light.pos - in.pbr_pos;
          let dist2 = dot(ldir, ldir);
          let dist = sqrt(dist2);
          ldir = ldir / dist;{5}
          let nl = max(dot(pbr_n, ldir), 0.0);
          let h = normalize(ldir + pbr_v);
          let nh = max(dot(pbr_n, h), 0.0);
          let vh = max(dot(pbr_v, h), 0.0);
          let dd = nh * nh * (pbr_a2 - 1.0) + 1.0;
          let d = pbr_a2 / max(pbr_pi * dd * dd, 1e-6 * pbr_a2);
          let vis = 0.25 / (max(pbr_nv * (1.0 - pbr_k) + pbr_k, 1e-4) * max(nl * (1.0 - pbr_k) + pbr_k, 1e-4));
          let f = pbr_f0 + (1.0 - pbr_f0) * pow(1.0 - vh, 5.0);
          let spec = d * vis * f;
          // GX lights are unnormalised (colour * N.L is full brightness), so Lambert has no
          // 1/pi and the specular lobe is scaled by pi to match.
          let rad = pow(max(light.color.rgb, vec3f(0.0)), vec3f(2.2)) * attn{16};
          pbr_lo += (pbr_diff * pbr_ao + spec * pbr_pi) * rad * nl;
          // Stand-in environment: the surroundings as a soft hemisphere lit by this light,
          // seen along the reflection vector.
          let env_w = 0.5 + 0.5 * dot(pbr_refl, ldir);
          pbr_env += rad * (env_w * env_w);
          pbr_lsum += rad;
      }}
      // pbr-lights-end
      // Ambient: diffuse plus the split-sum environment BRDF (Karis' analytic fit) applied
      // to the reflection probe, a cube map whose mips are picked by roughness. The
      // probe's weight is 0 until the game has filled it; the ambient and the stand-in
      // environment above take its place then. Both occluded.
      let pbr_c0 = vec4f(-1.0, -0.0275, -0.572, 0.022);
      let pbr_c1 = vec4f(1.0, 0.0425, 1.04, -0.04);
      let pbr_r = pbr_rough * pbr_c0 + pbr_c1;
      let pbr_a004 = min(pbr_r.x * pbr_r.x, exp2(-9.28 * pbr_nv)) * pbr_r.x + pbr_r.y;
      let pbr_ab = vec2f(-1.04, 1.04) * pbr_a004 + pbr_r.zw;
      let pbr_amb = pow(max({6}, vec3f(0.0)), vec3f(2.2));
      let pbr_pd = ubuf.pbr_probe[0].xyz * pbr_refl.x + ubuf.pbr_probe[1].xyz * pbr_refl.y +
                   ubuf.pbr_probe[2].xyz * pbr_refl.z;
      // A room cube (GX_AURORA_SET_PBR_CUBE) is linear HDR: x is its exposure, y the mip
      // a roughness of 1 samples. The probe is the display-referred scene, x is 0 then.
      let pbr_hdr = ubuf.pbr_cube.x;
      let pbr_lod = select({7}.0, ubuf.pbr_cube.y, pbr_hdr > 0.0);
      let pbr_cubed = textureSampleLevel(pbr_cube, pbr_cube_samp, pbr_pd, pbr_rough * pbr_lod).rgb;
      let pbr_cubel = select(pow(max(pbr_cubed, vec3f(0.0)), vec3f(2.2)), pbr_cubed * pbr_hdr, pbr_hdr > 0.0);
      var pbr_envspec = mix(pbr_amb + pbr_env * 0.35, pbr_cubel, min(ubuf.pbr_probe[0].w, 1.0));
      // The room cube also shapes the ambient: its blurriest useful mip (z) along the
      // normal says how much of the room's light comes from that side, and w scales that
      // to 1 for the cube's average. The game's ambient keeps the level and the colour.
      var pbr_ambd = pbr_amb;
      if (pbr_hdr > 0.0 && ubuf.pbr_cube.w > 0.0) {{
          let pbr_nd = ubuf.pbr_probe[0].xyz * pbr_n.x + ubuf.pbr_probe[1].xyz * pbr_n.y +
                       ubuf.pbr_probe[2].xyz * pbr_n.z;
          let pbr_irr = textureSampleLevel(pbr_cube, pbr_cube_samp, pbr_nd, ubuf.pbr_cube.z).rgb;
          pbr_ambd = pbr_amb * clamp(dot(pbr_irr, vec3f(0.2126, 0.7152, 0.0722)) * pbr_hdr * ubuf.pbr_cube.w, 0.35, 2.5);
      }}
      // Baked ambient (GX_AURORA_SET_PBR_AMBIENT) replaces all of that: a lobe per colour
      // channel around the direction most of that channel's light comes from.
      if (ubuf.pbr_ambient[0].w > 0.0) {{
          let pbr_aq = clamp(vec3f(dot(pbr_n, ubuf.pbr_ambient[3].xyz), dot(pbr_n, ubuf.pbr_ambient[4].xyz),
                                   dot(pbr_n, ubuf.pbr_ambient[5].xyz)) * 0.5 + 0.5, vec3f(0.0), vec3f(1.0));
          // w is 1 when the game's ambient sets the level, 2 when the baked light is the level.
          pbr_ambd = select(dot(pbr_amb, vec3f(0.2126, 0.7152, 0.0722)), 1.0, ubuf.pbr_ambient[0].w > 1.5) * max(ubuf.pbr_ambient[0].rgb + ubuf.pbr_ambient[1].rgb * pow(pbr_aq, ubuf.pbr_ambient[2].rgb),
                         vec3f(0.0));
      }}
      // An ambient volume (GX_AURORA_SET_PBR_VOLUME) is the same lobes, read at this pixel
      // from the room's grid, a little off the surface so that a wall is lit by the air in
      // front of it.
      var pbr_vdiag = vec3f(0.0);
      if (ubuf.pbr_volume[3].w > 0.0) {{
          let pbr_vp = vec4f(in.pbr_pos + pbr_n * ubuf.pbr_volume[4].w, 1.0);
          let pbr_vuv = vec3f(dot(ubuf.pbr_volume[0], pbr_vp), dot(ubuf.pbr_volume[1], pbr_vp),
                              dot(ubuf.pbr_volume[2], pbr_vp));
          let pbr_vmean = textureSampleLevel(pbr_vol_mean, pbr_cube_samp, pbr_vuv, 0.0).rgb;
          let pbr_vlobe = textureSampleLevel(pbr_vol_lobe, pbr_cube_samp, pbr_vuv, 0.0).rgb;
          let pbr_vr = textureSampleLevel(pbr_vol_r, pbr_cube_samp, pbr_vuv, 0.0);
          let pbr_vg = textureSampleLevel(pbr_vol_g, pbr_cube_samp, pbr_vuv, 0.0);
          let pbr_vb = textureSampleLevel(pbr_vol_b, pbr_cube_samp, pbr_vuv, 0.0);
          let pbr_vn = vec3f(dot(ubuf.pbr_volume[3].xyz, pbr_n), dot(ubuf.pbr_volume[4].xyz, pbr_n),
                             dot(ubuf.pbr_volume[5].xyz, pbr_n));
          let pbr_vq = clamp(vec3f(dot(pbr_vn, pbr_vr.xyz * 2.0 - 1.0), dot(pbr_vn, pbr_vg.xyz * 2.0 - 1.0),
                                   dot(pbr_vn, pbr_vb.xyz * 2.0 - 1.0)) * 0.5 + 0.5, vec3f(0.0), vec3f(1.0));
          let pbr_vs = vec3f(pbr_vr.a, pbr_vg.a, pbr_vb.a);
          pbr_ambd = max(pbr_vmean - pbr_vlobe + 2.0 * pbr_vlobe * (1.0 + pbr_vs) * pow(pbr_vq, 1.0 + 2.0 * pbr_vs),
                         vec3f(0.0)) * ubuf.pbr_volume[3].w;
          // The same lobes along the reflection stand in for the environment: unlike a cube
          // for the room, they are dark where this spot is.
          let pbr_vrn = vec3f(dot(ubuf.pbr_volume[3].xyz, pbr_refl), dot(ubuf.pbr_volume[4].xyz, pbr_refl),
                              dot(ubuf.pbr_volume[5].xyz, pbr_refl));
          let pbr_vrq = clamp(vec3f(dot(pbr_vrn, pbr_vr.xyz * 2.0 - 1.0), dot(pbr_vrn, pbr_vg.xyz * 2.0 - 1.0),
                                    dot(pbr_vrn, pbr_vb.xyz * 2.0 - 1.0)) * 0.5 + 0.5, vec3f(0.0), vec3f(1.0));
          pbr_envspec = max(pbr_vmean - pbr_vlobe + 2.0 * pbr_vlobe * (1.0 + pbr_vs) * pow(pbr_vrq, 1.0 + 2.0 * pbr_vs),
                            vec3f(0.0)) * ubuf.pbr_volume[3].w;
          // Diagnostics (w of row 5): 1 the texture coordinates, 2 the light alone, 3 the
          // normal.
          pbr_vdiag = select(pbr_ambd, pbr_vuv, ubuf.pbr_volume[5].w < 1.5);
          if (ubuf.pbr_volume[5].w > 2.5) {{
              pbr_vdiag = pbr_n * 0.5 + 0.5;
          }}
      }}
      pbr_lo += (pbr_ambd * pbr_diff + pbr_envspec * (pbr_f0 * pbr_ab.x + pbr_ab.y)) * pbr_ao;
      // Backlight (GX_AURORA_SET_PBR_MATERIAL): a rim on the edges turned away from the
      // viewer, the surface's own colour times the backlight weight. It is scaled by the
      // light that reaches the surface from any side, so it stays a lighting term and goes
      // dark with the room.
      let pbr_rim = 1.0 - pbr_nv;
      pbr_lo += max(ubuf.pbr_backlight.rgb, vec3f(0.0)) * pbr_base * min(pbr_amb + pbr_lsum, vec3f(1.0)) *
                (pbr_rim * pbr_rim * pbr_ao);
      // The material's alpha and shading modes (w of the GX_AURORA_SET_PBR_MATERIAL rows).
      // Kind 3 (lava, embers): the vertex alpha is how much of the glow shows.
      var pbr_glow = pbr_emissive * select(1.0, pbr_vraw.a, pbr_kind > 2.5 && pbr_kind < 3.5) + pbr_kglow;
      var pbr_alpha = {12}{9};
      var pbr_pass = vec3f(0.0);
      if (ubuf.pbr_emissive.w > 0.0) {{
          // A height blend (snow and ice laid over rock): the base map's alpha lifts the
          // vertex alpha, and a smoothstep as wide as the threshold cuts the edge.
          let pbr_hx = clamp(({0}.a + 1.0){9} * 2.0 - 1.0, 0.0, 1.0);
          let pbr_hs = clamp((pbr_hx - 0.5 + ubuf.pbr_emissive.w) / (2.0 * ubuf.pbr_emissive.w), 0.0, 1.0);
          pbr_alpha = pbr_hs * pbr_hs * (3.0 - 2.0 * pbr_hs);
      }}
      // 1 = unlit, 2 = the base map's alpha masks the glow, 4 = tinted by the vertex colour
      // (pbr_vc); the sum of those.
      let pbr_mode = ubuf.pbr_backlight.w - select(0.0, 4.0, ubuf.pbr_backlight.w > 3.5);
      if (pbr_mode > 1.5) {{
          // The base map's alpha is how much of the glow shows, and no opacity: the
          // vertex alpha alone is.
          pbr_glow = pbr_emissive * {0}.a;
          pbr_alpha = 1.0{9};
      }}
      if ((pbr_mode > 0.5 && pbr_mode < 1.5) || pbr_mode > 2.5) {{
          // Unlit (screens, holograms): the surface's own colour and its glow.
          pbr_lo = pbr_diff * pbr_ao;
      }}{14}
      let pbr_out = max(pbr_lo + pbr_glow, vec3f(0.0));
      // Highlights roll off into white instead of clipping: unchanged up to 0.6, and the
      // rest of the range (a room cube's lamps are many times brighter than 1) approaches 1.
      var pbr_tm = min(pbr_out, vec3f(0.6)) + 0.4 * (1.0 - exp(-max(pbr_out - 0.6, vec3f(0.0)) / 0.4));
      if (ubuf.pbr_tone[1].x > 0.0) {{
          // Or a tone curve (GX_AURORA_SET_PBR_TONE): a cubic toe, a line and a shoulder
          // that approaches 1.
          let pbr_toe = ((ubuf.pbr_tone[0].x * pbr_out + ubuf.pbr_tone[0].y) * pbr_out + ubuf.pbr_tone[0].z) * pbr_out;
          let pbr_line = ubuf.pbr_tone[1].x * pbr_out + ubuf.pbr_tone[1].y;
          let pbr_st = max(ubuf.pbr_tone[2].y * pbr_out + ubuf.pbr_tone[2].z, vec3f(0.0));
          let pbr_sh = ubuf.pbr_tone[2].x * pbr_st / (1.0 + pbr_st) + ubuf.pbr_tone[2].w;
          pbr_tm = select(select(pbr_sh, pbr_line, pbr_out < vec3f(ubuf.pbr_tone[1].w)), pbr_toe,
                          pbr_out < vec3f(ubuf.pbr_tone[1].z));
          pbr_tm = clamp(pbr_tm, vec3f(0.0), vec3f(1.0));
      }}
      prev = vec4f(pow(clamp(pbr_tm + pbr_pass, vec3f(0.0), vec3f(1.0)), vec3f(1.0 / 2.2)), pbr_alpha);
      // A debug view (GXSetPBRDebugView): one input of the shading in place of the result.
      if (ubuf.pbr_layer.w > 0.5) {{
          let pbr_dv = ubuf.pbr_layer.w;
          var pbr_dc = pow(clamp(pbr_base, vec3f(0.0), vec3f(1.0)), vec3f(1.0 / 2.2));
          if (pbr_dv > 1.5) {{ pbr_dc = pbr_n * 0.5 + 0.5; }}
          if (pbr_dv > 2.5) {{ pbr_dc = vec3f(pbr_rough); }}
          if (pbr_dv > 3.5) {{ pbr_dc = vec3f(pbr_metal); }}
          if (pbr_dv > 4.5) {{ pbr_dc = vec3f(pbr_ao); }}
          if (pbr_dv > 5.5) {{ pbr_dc = pow(clamp(pbr_ambd, vec3f(0.0), vec3f(1.0)), vec3f(1.0 / 2.2)); }}
          if (pbr_dv > 6.5) {{ pbr_dc = pow(clamp(pbr_envspec, vec3f(0.0), vec3f(1.0)), vec3f(1.0 / 2.2)); }}
          if (pbr_dv > 7.5) {{ pbr_dc = pow(clamp(pbr_glow, vec3f(0.0), vec3f(1.0)), vec3f(1.0 / 2.2)); }}
          if (pbr_dv > 8.5) {{
              // The lit level in stops around middle grey: blue 4 under, green at, red 4 over.
              let pbr_dt = clamp(log2(max(dot(pbr_out, vec3f(0.2126, 0.7152, 0.0722)), 1e-6) / 0.18) / 8.0 + 0.5,
                                 0.0, 1.0);
              pbr_dc = clamp(vec3f(4.0 * pbr_dt - 2.0, 2.0 - abs(4.0 * pbr_dt - 2.0), 2.0 - 4.0 * pbr_dt),
                             vec3f(0.0), vec3f(1.0));
          }}
          if (pbr_dv > 9.5) {{
              // The special surface's kind: grey for none, then a colour each.
              let pbr_dk = i32(pbr_kind + 0.5);
              pbr_dc = vec3f(0.5 * f32(pbr_dk & 1) + 0.25 * f32(pbr_dk == 0),
                             0.5 * f32((pbr_dk >> 1) & 1) + 0.25 * f32(pbr_dk == 0),
                             0.5 * f32((pbr_dk >> 2) & 1) + 0.25 * f32(pbr_dk == 0)) * 1.6;
          }}
          prev = vec4f(clamp(pbr_dc, vec3f(0.0), vec3f(1.0)), prev.a);
      }}
      if (ubuf.pbr_volume[5].w > 0.5) {{
          prev = vec4f(clamp(pbr_vdiag, vec3f(0.0), vec3f(1.0)), prev.a);
      }}
      // A weight above 1 is a diagnostic: 2 makes the surface a perfect mirror of the
      // probe, and 3 a window onto it, which must line up with the scene around it.
      if (ubuf.pbr_probe[0].w > 1.5) {{
          var pbr_dd = pbr_pd;
          if (ubuf.pbr_probe[0].w > 2.5) {{
              pbr_dd = -(ubuf.pbr_probe[0].xyz * pbr_v.x + ubuf.pbr_probe[1].xyz * pbr_v.y +
                         ubuf.pbr_probe[2].xyz * pbr_v.z);
          }}
          let pbr_diag = textureSampleLevel(pbr_cube, pbr_cube_samp, pbr_dd, 0.0).rgb;
          prev = vec4f(select(pbr_diag, pow(clamp(pbr_diag * pbr_hdr, vec3f(0.0), vec3f(1.0)), vec3f(1.0 / 2.2)),
                              pbr_hdr > 0.0), prev.a);
      }}
    }})""",
                     base, orm, sampled(3, "vec4f(0.0)"), normal, GX::MaxLights, attn, amb,
                     gfx::probe::MipCount - 1, tint, tintAlpha, layer, baseRgb, layered ? "1.0" : "prev.a", kinds, liquid,
                     shadowed ? "(ubuf.lightState0 | ubuf.lightState1)" : "ubuf.lightState0",
                     shadowed ? " * select(vec3f(1.0), sampled0.rgb, (ubuf.lightState0 & (1u << i)) == 0u)" : "");
  if (!lit) {
    // The uniform block has no lights then.
    const size_t begin = source.find("// pbr-lights-begin");
    const size_t end = source.find("// pbr-lights-end");
    source.erase(begin, end - begin);
  }
  return source;
}

absl::flat_hash_set<gfx::ShaderRef> s_seenShaders;
} // namespace

namespace {
// Stereo replay under Vulkan multiview (gfx/stereo_multiview.hpp): the shader runs
// for both views of a two-layer eye pass. Its GX textures are 2D arrays sampled at
// layer min(view, layers - 1), so an EFB copy taken per eye shows each eye its own
// layer and every other texture its one layer. Its uniform, bound at the draw's
// stereo offset (`uniformSize` is the GX uniform's size and so the stride of what
// stage_stereo_uniforms pushes after it):
//  - MultiviewClip: the mono uniform, then each eye's clip matrix and the eyes'
//    render size. Everything but the clip position is computed once, in the game
//    camera's view space (lighting comes out the same there: the eye poses are
//    rigid), so only the position depends on the view and Adreno shares the rest
//    between the views.
//  - MultiviewFull: a pair of full eye uniforms, element `view & imm.eye_mask` (eye
//    mask 0 when both eyes share one), for a draw whose texture matrix differs per
//    eye. A uniform indexed by the view is slower on Adreno (it stays out of the
//    constant registers, and the views share no work), so it is the exception.
//  - EyeClipImmediate: MultiviewClip's uniform in a per-eye pass: no multiview extension
//    or array textures, the eye from the immediates (imm.eye_mask) instead of the view.
std::string to_multiview_source(std::string source, uint32_t uniformSize, MultiviewMode mode, bool mapBatch) {
  const bool byImmediate = mode == EyeClipImmediate;
  const bool clip = mode == MultiviewClip || byImmediate;
  bool complete = true;
  const auto replace_once = [&](std::string_view from, std::string_view to) {
    const size_t pos = source.find(from);
    if (pos == std::string::npos) {
      complete = false;
      return;
    }
    source.replace(pos, from.size(), to);
  };
  // GX texture samples go through helpers that add the layer (textures named tex0..tex7).
  for (size_t pos = 0; !byImmediate && (pos = source.find("textureSample", pos)) != std::string::npos;) {
    size_t end = pos + std::string_view{"textureSample"}.size();
    for (const std::string_view suffix : {"Bias"sv, "Grad"sv, "Level"sv}) {
      if (source.compare(end, suffix.size(), suffix) == 0) {
        end += suffix.size();
        break;
      }
    }
    const bool gxTexture = source.compare(end, 4, "(tex") == 0 && end + 4 < source.size() &&
                           source[end + 4] >= '0' && source[end + 4] <= '7' &&
                           (end + 5 >= source.size() || !(std::isalnum(static_cast<unsigned char>(source[end + 5])) ||
                                                          source[end + 5] == '_'));
    if (gxTexture) {
      source.replace(pos, std::string_view{"texture"}.size(), "mv"); // textureSample* -> mvSample*
      pos += 2;
    } else {
      pos = end;
    }
  }
  for (size_t pos = 0; !byImmediate && (pos = source.find("texture_2d<f32>", pos)) != std::string::npos;) {
    source.replace(pos, std::string_view{"texture_2d<f32>"}.size(), "texture_2d_array<f32>");
  }
  replace_once("    _pad: u32,", "    eye_mask: u32,");
  if (!byImmediate) {
    replace_once("fn vs_main(\n    @builtin(vertex_index) vidx: u32",
                 "fn vs_main(\n    @builtin(view_index) mv_view_in: u32,\n    @builtin(vertex_index) vidx: u32");
  }
  const std::string facing = mapBatch ? ", @builtin(front_facing) map_front: bool" : "";
  const std::string fragmentEntry = "fn fs_main(in: VertexOutput" + facing + ") -> @location(0) vec4f {";
  const std::string fragmentEntryView =
      "fn fs_main(in: VertexOutput" + facing + ", @builtin(view_index) mv_view_in: u32) -> @location(0) vec4f {\n"
      "    mv_view = mv_view_in;";
  if (clip) {
    replace_once("var<uniform> ubuf: Uniform;", "var<uniform> ubuf_mv: UniformMV;");
    constexpr std::string_view bodyStart = "    var out: VertexOutput;";
    constexpr std::string_view bodyEnd = "\n    return out;\n}";
    const size_t start = source.find(bodyStart);
    const size_t end = start != std::string::npos ? source.find(bodyEnd, start) : std::string::npos;
    if (end == std::string::npos) {
      complete = false;
    } else {
      const size_t bodyBegin = start + bodyStart.size();
      std::string body = source.substr(bodyBegin, end - bodyBegin);
      const auto replace_all = [&body](std::string_view from, std::string_view to) {
        for (size_t pos = 0; (pos = body.find(from, pos)) != std::string::npos; pos += to.size()) {
          body.replace(pos, from.size(), to);
        }
      };
      // The clip position per view: points and lines expand around theirs too.
      for (const std::string_view position : {"mv_pos"sv, "mv_pos_a"sv, "mv_pos_b"sv}) {
        replace_all(fmt::format("vec4f({}, 1.0) * ubuf.proj", position),
                    fmt::format("select(vec4f({0}, 1.0) * ubuf_mv.eye_clip0, vec4f({0}, 1.0) * ubuf_mv.eye_clip1, "
                                "mv_eye1)",
                                position));
      }
      replace_all("ubuf.render_viewport_size", "ubuf_mv.eye_render.xy");
      if (body.find("ubuf.proj") != std::string::npos) {
        complete = false;
      }
      source.replace(bodyBegin, end - bodyBegin,
                     (byImmediate ? "\n    let ubuf = &ubuf_mv.u;\n    let mv_eye1 = imm.eye_mask != 0u;"
                                  : "\n    mv_view = mv_view_in;\n    let ubuf = &ubuf_mv.u;\n    let mv_eye1 = "
                                    "mv_view_in != 0u;") +
                         body);
    }
    replace_once(fragmentEntry,
                 std::string{byImmediate ? fragmentEntry : fragmentEntryView} + "\n    let ubuf = &ubuf_mv.u;");
  } else {
    replace_once("var<uniform> ubuf: Uniform;", "var<uniform> ubuf_eyes: array<UniformEye, 2>;");
    replace_once("    var out: VertexOutput;", "    var out: VertexOutput;\n    mv_view = mv_view_in;\n"
                                               "    let ubuf = &ubuf_eyes[mv_view_in & imm.eye_mask].u;");
    // The fragment shader reads only what both eye copies share (the eyes have one
    // size), so the first; the PBR lighting's view-space lights differ per eye.
    const size_t fragmentStart = source.find("fn fs_main(");
    const bool fragmentReadsLights =
        fragmentStart != std::string::npos && source.find("ubuf.lights", fragmentStart) != std::string::npos;
    replace_once(fragmentEntry, std::string{fragmentEntryView} +
                                    (fragmentReadsLights ? "\n    let ubuf = &ubuf_eyes[mv_view_in & imm.eye_mask].u;"
                                                         : "\n    let ubuf = &ubuf_eyes[0].u;"));
  }
  if (!complete) {
    Log.error("Multiview shader rewrite missed an anchor; the generated shader changed shape");
  }
  static constexpr std::string_view kMultiviewHelpers = R"""(
var<private> mv_view: u32;

fn mv_layer(t: texture_2d_array<f32>) -> u32 {
    return min(mv_view, textureNumLayers(t) - 1u);
}

fn mvSample(t: texture_2d_array<f32>, s: sampler, uv: vec2f) -> vec4f {
    return textureSample(t, s, uv, mv_layer(t));
}

fn mvSampleBias(t: texture_2d_array<f32>, s: sampler, uv: vec2f, bias: f32) -> vec4f {
    return textureSampleBias(t, s, uv, mv_layer(t), bias);
}

fn mvSampleGrad(t: texture_2d_array<f32>, s: sampler, uv: vec2f, ddx: vec2f, ddy: vec2f) -> vec4f {
    return textureSampleGrad(t, s, uv, mv_layer(t), ddx, ddy);
}

fn mvSampleLevel(t: texture_2d_array<f32>, s: sampler, uv: vec2f, level: f32) -> vec4f {
    return textureSampleLevel(t, s, uv, mv_layer(t), level);
}
)""";
  return fmt::format(R"""({0}{1}
struct UniformEye {{
    @size({2}) u: Uniform,
}};

struct UniformMV {{
    @size({2}) u: Uniform,
    eye_clip0: mat4x4f,
    eye_clip1: mat4x4f,
    eye_render: vec4f,
}};
{3})""",
                     byImmediate ? ""sv : "enable chromium_experimental_multiview;\n"sv, source, uniformSize,
                     byImmediate ? ""sv : kMultiviewHelpers);
}
} // namespace

std::string build_shader_source(const ShaderConfig& config) noexcept {
  ZoneScoped;
  const auto hash = xxh3_hash(config);
  const auto info = build_shader_info(config);
  if (EnableDebugPrints && !s_seenShaders.contains(hash)) {
    s_seenShaders.insert(hash);

    Log.info("Shader config (hash {:x}):", hash);
    {
      for (int i = 0; i < config.tevStageCount; ++i) {
        const auto& stage = config.tevStages[i];
        Log.info("  tevStages[{}]:", i);
        Log.info("    color_a: {}", TevColorArgNames[stage.colorPass.a]);
        Log.info("    color_b: {}", TevColorArgNames[stage.colorPass.b]);
        Log.info("    color_c: {}", TevColorArgNames[stage.colorPass.c]);
        Log.info("    color_d: {}", TevColorArgNames[stage.colorPass.d]);
        Log.info("    alpha_a: {}", TevAlphaArgNames[stage.alphaPass.a]);
        Log.info("    alpha_b: {}", TevAlphaArgNames[stage.alphaPass.b]);
        Log.info("    alpha_c: {}", TevAlphaArgNames[stage.alphaPass.c]);
        Log.info("    alpha_d: {}", TevAlphaArgNames[stage.alphaPass.d]);
        Log.info("    color_op_clamp: {}", stage.colorOp.clamp);
        Log.info("    color_op_op: {}", stage.colorOp.op);
        Log.info("    color_op_bias: {}", stage.colorOp.bias);
        Log.info("    color_op_scale: {}", stage.colorOp.scale);
        Log.info("    color_op_reg_id: {}", stage.colorOp.outReg);
        Log.info("    alpha_op_clamp: {}", stage.alphaOp.clamp);
        Log.info("    alpha_op_op: {}", stage.alphaOp.op);
        Log.info("    alpha_op_bias: {}", stage.alphaOp.bias);
        Log.info("    alpha_op_scale: {}", stage.alphaOp.scale);
        Log.info("    alpha_op_reg_id: {}", stage.alphaOp.outReg);
        Log.info("    kc_sel: {}", stage.kcSel);
        Log.info("    ka_sel: {}", stage.kaSel);
        Log.info("    texCoordId: {}", stage.texCoordId);
        Log.info("    texMapId: {}", stage.texMapId);
        Log.info("    channelId: {}", stage.channelId);
        Log.info("    tevSwapRas: {}", stage.tevSwapRas);
        Log.info("    tevSwapTex: {}", stage.tevSwapTex);
        Log.info("    indTexStage: {}", stage.indTexStage);
        Log.info("    indTexFormat: {}", stage.indTexFormat);
        Log.info("    indTexBiasSel: {}", stage.indTexBiasSel);
        Log.info("    indTexAlphaSel: {}", stage.indTexAlphaSel);
        Log.info("    indTexMtxId: {}", stage.indTexMtxId);
        Log.info("    indTexWrapS: {}", stage.indTexWrapS);
        Log.info("    indTexWrapT: {}", stage.indTexWrapT);
        Log.info("    indTexUseOrigLOD: {}", stage.indTexUseOrigLOD);
        Log.info("    indTexAddPrev: {}", stage.indTexAddPrev);
      }
      Log.info("  numIndStages: {}", config.numIndStages);
      for (u32 i = 0; i < config.numIndStages; ++i) {
        const auto& stage = config.indStages[i];
        Log.info("  indStages[{}]: texCoordId {} texMapId {} scaleS {} scaleT {}", i, stage.texCoordId, stage.texMapId,
                 stage.scaleS, stage.scaleT);
      }
      for (int i = 0; i < config.colorChannels.size(); ++i) {
        const auto& chan = config.colorChannels[i];
        Log.info("  colorChannels[{}]: enabled {} mat {} amb {}", static_cast<GXChannelID>(i), chan.lightingEnabled,
                 chan.matSrc, chan.ambSrc);
      }
      for (int i = 0; i < config.tcgs.size(); ++i) {
        const auto& tcg = config.tcgs[i];
        if (tcg.src != GX_MAX_TEXGENSRC) {
          Log.info("  tcg[{}]: src {} mtx {} post {} type {} norm {}", i, tcg.src, tcg.mtx, tcg.postMtx, tcg.type,
                   tcg.normalize);
        }
      }
      Log.info("  alphaCompare: comp0 {} ref0 {} op {} comp1 {} ref1 {}", config.alphaCompare.comp0,
               config.alphaCompare.ref0, config.alphaCompare.op, config.alphaCompare.comp1, config.alphaCompare.ref1);
      Log.info("  fogType: {}", config.fogType);
      Log.info("  fogRangeEnabled: {}", config.fogRangeEnabled);
    }
  }

  std::string uniformPre;
  std::string uniBufAttrs;
  std::string texBindings;
  std::string vtxOutAttrs;
  std::string vtxInAttrs;
  std::string vtxXfrAttrsPre;
  std::string vtxXfrAttrs;
  size_t vtxOutIdx = 0;

  // Load points for line/point expansion
  std::string_view vidxAttr = "vidx"sv;
  if (config.lineMode != 0) {
    vtxInAttrs += ",\n    @builtin(instance_index) iidx: u32";
    uniBufAttrs +=
        "\n    line_width: f32,"
        "\n    line_aspect_y: f32,"
        "\n    line_tex_offset: f32,"
        "\n    line_texcoord_mask: u32,";
    if (config.lineMode == 3) {
      // GX_POINTS: each instance = one vertex, expand to quad
      vtxXfrAttrsPre += fmt::format(
          "\n    let in_vidx = iidx;"
          "\n    let in_pos = {};"
          "\n    let in_pnmtxidx = {};"
          "\n    let mv_pos = vec4f(in_pos, 1.0) * ubuf.postex_mtx[in_pnmtxidx];",
          attr_load(config, GX_VA_POS, "in_vidx"sv), attr_load(config, GX_VA_PNMTXIDX, "in_vidx"sv));
    } else {
      // GX_LINES / GX_LINESTRIP: each instance = two vertices, expand to quad
      vtxXfrAttrsPre += fmt::format(
          "\n    let use_b = vidx >= 2u;"
          "\n    let vidx_a = iidx * {}u;"
          "\n    let vidx_b = vidx_a + 1u;"
          "\n    let in_vidx = select(vidx_a, vidx_b, use_b);"
          "\n    let pos_a = {};"
          "\n    let pos_b = {};"
          "\n    let in_pos = select(pos_a, pos_b, use_b);"
          "\n    let pnmtxidx_a = {};"
          "\n    let pnmtxidx_b = {};"
          "\n    let in_pnmtxidx = select(pnmtxidx_a, pnmtxidx_b, use_b);"
          "\n    let mv_pos_a = vec4f(pos_a, 1.0) * ubuf.postex_mtx[pnmtxidx_a];"
          "\n    let mv_pos_b = vec4f(pos_b, 1.0) * ubuf.postex_mtx[pnmtxidx_b];"
          "\n    let mv_pos = select(mv_pos_a, mv_pos_b, use_b);",
          config.lineMode == 1 ? 2 : 1, attr_load(config, GX_VA_POS, "vidx_a"sv),
          attr_load(config, GX_VA_POS, "vidx_b"sv), attr_load(config, GX_VA_PNMTXIDX, "vidx_a"sv),
          attr_load(config, GX_VA_PNMTXIDX, "vidx_b"sv));
    }
    vidxAttr = "in_vidx"sv;
  } else if (config.attrs[GX_VA_PNMTXIDX].attrType == GX_NONE) {
    vtxXfrAttrsPre += fmt::format("\n    let in_pnmtxidx = {}u;", config.currentPnMtx);
  }

  // Load vertex attributes
  for (GXAttr attr = GX_VA_PNMTXIDX; attr <= GX_VA_TEX7; attr = static_cast<GXAttr>(attr + 1)) {
    const auto attrType = config.attrs[attr].attrType;
    if (attrType == GX_NONE) {
      continue;
    }
    // in_pnmtxidx and in_pos written above for line mode
    if ((attr != GX_VA_PNMTXIDX && attr != GX_VA_POS) || config.lineMode == 0) {
      vtxXfrAttrsPre += fmt::format("\n    let {} = {};", vtx_attr(config, attr), attr_load(config, attr, vidxAttr));
    }
  }
  bool needsBinrm = false;
  bool needsTangent = false;
  for (int i = 0; i < info.sampledTexCoords.size(); ++i) {
    if (!info.sampledTexCoords.test(i)) {
      continue;
    }
    const bool emboss = is_emboss_texgen(config.tcgs[i].type);
    needsBinrm = needsBinrm || config.tcgs[i].src == GX_TG_BINRM || emboss;
    needsTangent = needsTangent || config.tcgs[i].src == GX_TG_TANGENT || emboss;
  }
  if (needsBinrm) {
    vtxXfrAttrsPre += fmt::format("\n    let {} = {};", nbt_slice_local(NbtSlice::B),
                                  attr_load_nbt_slice(config, NbtSlice::B, vidxAttr));
  }
  if (needsTangent) {
    vtxXfrAttrsPre += fmt::format("\n    let {} = {};", nbt_slice_local(NbtSlice::T),
                                  attr_load_nbt_slice(config, NbtSlice::T, vidxAttr));
  }

  if (config.mapBatch) {
    // The ordered map stream interleaves fills and line quads. Expand endpoints
    // in this eye's clip space, just as the existing GX_LINES shader does.
    vtxOutAttrs += fmt::format("\n    @location({}) @interpolate(flat) map_fill: u32,", vtxOutIdx++);
    vtxXfrAttrsPre +=
        "\n    let mv_pos_a = vec4f(in_pos, 1.0) * ubuf.postex_mtx[in_pnmtxidx];"
        "\n    let mv_pos_b = vec4f(in_nrm, 1.0) * ubuf.postex_mtx[in_pnmtxidx];"
        "\n    let use_b = in_tex0_uv.y > 0.5;"
        "\n    let mv_pos = select(mv_pos_a, mv_pos_b, use_b);"
        "\n    let clip_a = vec4f(mv_pos_a, 1.0) * ubuf.proj;"
        "\n    let clip_b = vec4f(mv_pos_b, 1.0) * ubuf.proj;"
        "\n    let viewport_scale = ubuf.render_viewport_size / max(ubuf.logical_viewport_size, vec2f(1.0));"
        "\n    let delta_px = (clip_b.xy / clip_b.w - clip_a.xy / clip_a.w) / 2.0 * ubuf.render_viewport_size;"
        "\n    let dir_px = select(vec2f(1.0, 0.0), normalize(delta_px), dot(delta_px, delta_px) > 1e-10);"
        "\n    let perp_px = vec2f(-dir_px.y, dir_px.x);"
        "\n    let offset_px = perp_px * (in_tex0_uv.x * min(viewport_scale.x, viewport_scale.y) / 2.0);"
        "\n    let offset_ndc = (offset_px * 2.0) / ubuf.render_viewport_size;"
        "\n    let clip_base = select(clip_a, clip_b, use_b);"
        "\n    out.pos = vec4f(clip_base.xy + offset_ndc * clip_base.w, clip_base.zw);"
        "\n    out.map_fill = select(0u, 1u, in_tex0_uv.y < 0.0);";
  } else if (config.lineMode == 0) {
    vtxXfrAttrsPre += fmt::format(
        "\n    let mv_pos = vec4f({}, 1.0) * ubuf.postex_mtx[in_pnmtxidx];"
        "\n    out.pos = vec4f(mv_pos, 1.0) * ubuf.proj;",
        vtx_attr(config, GX_VA_POS));
  } else if (config.lineMode == 3) {
    // GX_POINTS: expand single vertex to axis-aligned screen-space square
    vtxXfrAttrsPre +=
        "\n    let clip = vec4f(mv_pos, 1.0) * ubuf.proj;"
        "\n    let viewport_scale = ubuf.render_viewport_size / max(ubuf.logical_viewport_size, vec2f(1.0));"
        "\n    let point_size = ubuf.line_width * min(viewport_scale.x, viewport_scale.y);"
        "\n    let x_sign = select(-1.0, 1.0, (vidx & 1u) != 0u);"
        "\n    let y_sign = select(-1.0, 1.0, vidx >= 2u);"
        "\n    let offset_px = vec2f(x_sign, y_sign) * (point_size / 2.0);"
        "\n    let offset_ndc = (offset_px * 2.0) / ubuf.render_viewport_size;"
        "\n    out.pos = vec4f(clip.xy + offset_ndc * clip.w, clip.zw);";
  } else {
    // GX_LINES / GX_LINESTRIP: expand line segment perpendicular to direction
    vtxXfrAttrsPre +=
        "\n    let clip_a = vec4f(mv_pos_a, 1.0) * ubuf.proj;"
        "\n    let clip_b = vec4f(mv_pos_b, 1.0) * ubuf.proj;"
        "\n    let ndc_a = clip_a.xy / clip_a.w;"
        "\n    let ndc_b = clip_b.xy / clip_b.w;"
        "\n    let viewport_scale = ubuf.render_viewport_size / max(ubuf.logical_viewport_size, vec2f(1.0));"
        "\n    let delta_px = (ndc_b - ndc_a) / 2.0 * ubuf.render_viewport_size;"
        "\n    let dir_px = select(vec2f(1.0, 0.0), normalize(delta_px), dot(delta_px, delta_px) > 1e-10);"
        "\n    let perp_px = vec2f(-dir_px.y, dir_px.x);"
        "\n    let line_width = ubuf.line_width * min(viewport_scale.x, viewport_scale.y);"
        "\n    let offset_px = perp_px * (line_width / 2.0) * select(-1.0, 1.0, (vidx & 1u) != 0u);"
        "\n    let offset_ndc = (offset_px * 2.0) / ubuf.render_viewport_size;"
        "\n    let clip_base = select(clip_a, clip_b, use_b);"
        "\n    out.pos = vec4f(clip_base.xy + offset_ndc * clip_base.w, clip_base.zw);";
  }
  vtxXfrAttrsPre += fmt::format(
      "\n    let nrm_tmp = vec4f({}, 0.0) * ubuf.nrm_mtx[in_pnmtxidx];"
      "\n    let mv_nrm = select(nrm_tmp, normalize(nrm_tmp), dot(nrm_tmp, nrm_tmp) > 1e-10);",
      vtx_attr(config, GX_VA_NRM));
  if constexpr (EnableNormalVisualization) {
    vtxOutAttrs += fmt::format("\n    @location({}) nrm: vec3f,", vtxOutIdx++);
    vtxXfrAttrsPre += "\n    out.nrm = mv_nrm;";
  }

  uniBufAttrs += "\n    proj: mat4x4f,";
  uniBufAttrs += fmt::format("\n    postex_mtx: array<mat3x4f, {}>,", MaxPnMtx + MaxTexMtx);
  uniBufAttrs += fmt::format("\n    nrm_mtx: array<mat3x4f, {}>,", MaxPnMtx);
  std::string fragmentFnPre;
  std::string fragmentFn;
  if (config.mapBatch) {
    // GX lines ignore culling; only the fill primitives inherit the GX mode.
    if (config.mapCull == GX_CULL_FRONT) {
      fragmentFnPre += "\n    if (in.map_fill != 0u && map_front) { discard; }";
    } else if (config.mapCull == GX_CULL_BACK) {
      fragmentFnPre += "\n    if (in.map_fill != 0u && !map_front) { discard; }";
    } else if (config.mapCull == GX_CULL_ALL) {
      fragmentFnPre += "\n    if (in.map_fill != 0u) { discard; }";
    }
  }

  static std::array regName{"prev"sv, "tevreg0"sv, "tevreg1"sv, "tevreg2"sv};
  std::array<bool, MaxTevRegs> colorNormalized{};
  std::array<bool, MaxTevRegs> alphaNormalized{};
  for (u32 idx = 0; idx < config.tevStageCount; ++idx) {
    const auto& stage = config.tevStages[idx];
    {
      const auto color_arg = [&](GXTevColorArg arg) {
        auto value = color_arg_reg(arg, idx, config, stage);
        if (tev_color_arg_is_normalized(arg, colorNormalized, alphaNormalized)) {
          return fmt::format("vec3f({})", value);
        }
        return fmt::format("tev_overflow_vec3f({})", value);
      };
      std::string_view outReg = regName[stage.colorOp.outReg];
      std::string op = tev_color_op(stage.colorOp.op, tev_bias(stage.colorOp.bias), tev_scale(stage.colorOp.scale),
                                    stage.colorOp.clamp, color_arg(stage.colorPass.a), color_arg(stage.colorPass.b),
                                    color_arg(stage.colorPass.c), color_arg_reg(stage.colorPass.d, idx, config, stage));
      fragmentFn += fmt::format("\n    // TEV stage {2}\n    {0} = vec4f({1}, {0}.a);", outReg, op, idx);
      colorNormalized[stage.colorOp.outReg] = stage.colorOp.clamp;
    }
    {
      const auto alpha_arg = [&](GXTevAlphaArg arg) {
        auto value = alpha_arg_reg(arg, idx, config, stage);
        if (tev_alpha_arg_is_normalized(arg, alphaNormalized)) {
          return value;
        }
        return fmt::format("tev_overflow_f32({})", value);
      };
      std::string_view outReg = regName[stage.alphaOp.outReg];
      std::string op = tev_alpha_op(stage.alphaOp.op, tev_bias(stage.alphaOp.bias), tev_scale(stage.alphaOp.scale),
                                    stage.alphaOp.clamp, alpha_arg(stage.alphaPass.a), alpha_arg(stage.alphaPass.b),
                                    alpha_arg(stage.alphaPass.c), alpha_arg_reg(stage.alphaPass.d, idx, config, stage));
      fragmentFn += fmt::format("\n    {0}.a = {1};", outReg, op);
      alphaNormalized[stage.alphaOp.outReg] = stage.alphaOp.clamp;
    }
  }

  const auto& lastStage = config.tevStages[config.tevStageCount - 1];
  const bool prevColorNormalized = colorNormalized[lastStage.colorOp.outReg];
  const bool prevAlphaNormalized = alphaNormalized[lastStage.alphaOp.outReg];
  if (lastStage.colorOp.outReg != 0) {
    fragmentFn += fmt::format("\n    prev = vec4f({0}.rgb, prev.a);", regName[lastStage.colorOp.outReg]);
  }
  if (lastStage.alphaOp.outReg != 0) {
    fragmentFn += fmt::format("\n    prev.a = {0}.a;", regName[lastStage.alphaOp.outReg]);
  }

  if (info.loadsTevReg.test(0)) {
    uniBufAttrs += "\n    tevprev: vec4f,";
    fragmentFnPre += "\n    var prev = ubuf.tevprev;";
  } else {
    fragmentFnPre += "\n    var prev: vec4f;";
  }
  for (int i = 1 /* Skip TEVPREV */; i < info.loadsTevReg.size(); ++i) {
    if (info.loadsTevReg.test(i)) {
      uniBufAttrs += fmt::format("\n    tevreg{}: vec4f,", i - 1);
      fragmentFnPre += fmt::format("\n    var tevreg{0} = ubuf.tevreg{0};", i - 1);
    } else if (info.writesTevReg.test(i)) {
      fragmentFnPre += fmt::format("\n    var tevreg{0}: vec4f;", i - 1);
    }
  }

  if (info.lightingEnabled) {
    uniBufAttrs += fmt::format(FMT_STRING(R"""(
    lights: array<Light, {}>,
    lightState0: u32,
    lightState1: u32,
    lightState0a: u32,
    lightState1a: u32,)"""),
                               GX::MaxLights);
    uniformPre +=
        "\n"
        "struct Light {\n"
        "    pos: vec3f,\n"
        "    dir: vec3f,\n"
        "    color: vec4f,\n"
        "    cos_att: vec3f,\n"
        "    dist_att: vec3f,\n"
        "};";
    if (UsePerPixelLighting) {
      vtxOutAttrs += fmt::format("\n    @location({}) mv_pos: vec3f,", vtxOutIdx++);
      vtxOutAttrs += fmt::format("\n    @location({}) mv_nrm: vec3f,", vtxOutIdx++);
      vtxXfrAttrs += fmt::format(FMT_STRING(R"""(
    out.mv_pos = mv_pos;
    out.mv_nrm = mv_nrm;)"""));
    }
  }

  for (int i = 0; i < info.sampledColorChannels.size(); ++i) {
    if (!info.sampledColorChannels.test(i)) {
      continue;
    }

    const auto& cc = config.colorChannels[i];
    const auto& cca = config.colorChannels[i + GX_ALPHA0];
    if (cc.lightingEnabled && cc.ambSrc == GX_SRC_REG) {
      uniBufAttrs += fmt::format("\n    cc{0}_amb: vec4f,", i);
    }
    if (cc.matSrc == GX_SRC_REG) {
      uniBufAttrs += fmt::format("\n    cc{0}_mat: vec4f,", i);
    }
    if (cca.lightingEnabled && cca.ambSrc == GX_SRC_REG) {
      uniBufAttrs += fmt::format("\n    cc{0}a_amb: vec4f,", i);
    }
    if (cca.matSrc == GX_SRC_REG) {
      uniBufAttrs += fmt::format("\n    cc{0}a_mat: vec4f,", i);
    }

    // Output vertex color if necessary
    if (UsePerPixelLighting) {
      if ((cc.lightingEnabled && cc.ambSrc == GX_SRC_VTX) || cc.matSrc == GX_SRC_VTX ||
          (cca.lightingEnabled && cca.ambSrc == GX_SRC_VTX) || cca.matSrc == GX_SRC_VTX) {
        vtxOutAttrs += fmt::format("\n    @location({}) clr{}: vec4f,", vtxOutIdx++, i);
        vtxXfrAttrs += fmt::format("\n    out.clr{} = {};", i, vtx_attr(config, static_cast<GXAttr>(GX_VA_CLR0 + i)));
      }
    }

    if (UsePerPixelLighting) {
      fragmentFnPre += fmt::format("\n    var rast{}: vec4f;", i);
      fragmentFnPre += lighting_func(config, cc, i, false);
      fragmentFnPre += lighting_func(config, cca, i, true);
    } else {
      vtxOutAttrs += fmt::format("\n    @location({}) cc{}: vec4f,", vtxOutIdx++, i);
      vtxXfrAttrs += lighting_func(config, cc, i, false);
      vtxXfrAttrs += lighting_func(config, cca, i, true);
      fragmentFnPre += fmt::format("\n    var rast{0} = in.cc{0};", i);
    }
  }
  for (int i = 0; i < info.sampledKColors.size(); ++i) {
    if (info.sampledKColors.test(i)) {
      uniBufAttrs += fmt::format("\n    kcolor{}: vec4f,", i);
    }
  }
  for (int i = 0; i < info.sampledTexCoords.size(); ++i) {
    if (!info.sampledTexCoords.test(i)) {
      continue;
    }
    const auto& tcg = config.tcgs[i];
    if (tcg.type == GX_TG_MTX3x4) {
      vtxOutAttrs += fmt::format("\n    @location({}) tex{}_uvw: vec3f,", vtxOutIdx++, i);
    } else {
      vtxOutAttrs += fmt::format("\n    @location({}) tex{}_uv: vec2f,", vtxOutIdx++, i);
    }
    if (is_emboss_texgen(tcg.type)) {
      // Emboss bump: offset the source texcoord by the light projected onto tangent/binormal
      const u32 lightIdx = tcg.type - GX_TG_BUMP0;
      vtxXfrAttrs += fmt::format(
          "\n    let bump_ldir{0} = normalize(ubuf.lights[{1}].pos - mv_pos);"
          "\n    let bump_tan{0} = vec4f(in_tangent, 0.0) * ubuf.nrm_mtx[in_pnmtxidx];"
          "\n    let bump_bin{0} = vec4f(in_binrm, 0.0) * ubuf.nrm_mtx[in_pnmtxidx];"
          "\n    out.tex{0}_uv = tc{2}_proj.xy + vec2f(dot(bump_ldir{0}, bump_tan{0}), dot(bump_ldir{0}, "
          "bump_bin{0}));",
          i, lightIdx, tcg.embossSrc);
      fragmentFnPre += fmt::format("\n    var tex{0}_uv = in.tex{0}_uv.xy;", i);
      continue;
    }
    if (tcg.src >= GX_TG_TEX0 && tcg.src <= GX_TG_TEX7) {
      vtxXfrAttrs += fmt::format("\n    var tc{} = vec4f({}, 1.0, 1.0);", i,
                                 vtx_attr(config, GXAttr(GX_VA_TEX0 + (tcg.src - GX_TG_TEX0))));
    } else if (tcg.src == GX_TG_POS) {
      vtxXfrAttrs += fmt::format("\n    var tc{} = vec4f({}, 1.0);", i, vtx_attr(config, GX_VA_POS));
    } else if (tcg.src == GX_TG_NRM) {
      vtxXfrAttrs += fmt::format("\n    var tc{} = vec4f({}, 1.0);", i, vtx_attr(config, GX_VA_NRM));
    } else if (tcg.src == GX_TG_COLOR0) {
      vtxXfrAttrs += fmt::format("\n    var tc{} = {};", i, vtx_attr(config, GX_VA_CLR0));
    } else if (tcg.src == GX_TG_COLOR1) {
      vtxXfrAttrs += fmt::format("\n    var tc{} = {};", i, vtx_attr(config, GX_VA_CLR1));
    } else if (tcg.src == GX_TG_BINRM) {
      vtxXfrAttrs += fmt::format("\n    var tc{} = vec4f({}, 1.0);", i, nbt_slice_local(NbtSlice::B));
    } else if (tcg.src == GX_TG_TANGENT) {
      vtxXfrAttrs += fmt::format("\n    var tc{} = vec4f({}, 1.0);", i, nbt_slice_local(NbtSlice::T));
    } else if (tcg.src == GX_MAX_TEXGENSRC) {
      // The game leaves a texcoord generator's source register undefined when
      // it disables that generator, and only writes the ones it enables. This
      // value means "no source": the stage contributes nothing, so give it a
      // constant texcoord instead of aborting on the game's own idle state.
      // Logging already skips this value (see the dump above).
      vtxXfrAttrs += fmt::format("\n    var tc{} = vec4f(0.0, 0.0, 1.0, 1.0);", i);
    } else
      UNLIKELY FATAL("unhandled tcg src {}", underlying(tcg.src));
    if (tcg.type == GX_TG_MTX2x4 || tcg.type == GX_TG_MTX3x4) {
      if (info.indexAttr.test(GX_VA_TEX0MTXIDX + i)) {
        vtxXfrAttrs += fmt::format("\n    var tc{0}_tmp = tc{0} * ubuf.postex_mtx[in_texmtxidx{0} / 3u];", i);
      } else if (tcg.mtx == GX_IDENTITY) {
        vtxXfrAttrs += fmt::format("\n    var tc{0}_tmp = tc{0}.xyz;", i);
      } else {
        u32 texMtxIdx = (tcg.mtx) / 3;
        vtxXfrAttrs += fmt::format("\n    var tc{0}_tmp = tc{0} * ubuf.postex_mtx[{1}];", i, texMtxIdx);
      }
      if (tcg.type == GX_TG_MTX2x4) {
        vtxXfrAttrs += fmt::format("\n    tc{0}_tmp.z = 1.0f;", i);
      }
    } else if (tcg.type == GX_TG_SRTG) {
      vtxXfrAttrs += fmt::format("\n    var tc{0}_tmp = vec3f(tc{0}.xy, 1.0f);", i);
    }
    if (tcg.normalize) {
      vtxXfrAttrs += fmt::format("\n    tc{0}_tmp = normalize(tc{0}_tmp);", i);
    }
    if (tcg.postMtx == GX_PTIDENTITY) {
      vtxXfrAttrs += fmt::format("\n    var tc{0}_proj = tc{0}_tmp;", i);
    } else {
      u32 postMtxIdx = (tcg.postMtx - GX_PTTEXMTX0) / 3;
      vtxXfrAttrs +=
          fmt::format("\n    var tc{0}_proj = vec4f(tc{0}_tmp.xyz, 1.0) * ubuf.postmtx[{1}];", i, postMtxIdx);
    }
    // Apply line/point tex offset
    if (config.lineMode == 3) {
      // GX_POINTS: offset S for right columns, T for bottom rows
      vtxXfrAttrs += fmt::format(
          "\n    if ((ubuf.line_texcoord_mask & (1u << {0})) != 0u) {{"
          "\n        if ((vidx & 1u) != 0u) {{ tc{0}_proj.x += ubuf.line_tex_offset; }}"
          "\n        if (vidx >= 2u) {{ tc{0}_proj.y += ubuf.line_tex_offset; }}"
          "\n    }}",
          i);
    } else if (config.lineMode != 0) {
      // GX_LINES / GX_LINESTRIP: offset one axis for perpendicular side
      vtxXfrAttrs += fmt::format(
          "\n    if ((ubuf.line_texcoord_mask & (1u << {0})) != 0u && (vidx & 1u) != 0u) {{"
          "\n        tc{0}_proj.y += ubuf.line_tex_offset;"
          "\n    }}",
          i);
    }
    if (tcg.type == GX_TG_MTX3x4) {
      vtxXfrAttrs += fmt::format("\n    out.tex{0}_uvw = tc{0}_proj.xyz;", i);
      fragmentFnPre += fmt::format("\n    var tex{0}_uv = in.tex{0}_uvw.xy / in.tex{0}_uvw.z;", i);
    } else {
      vtxXfrAttrs += fmt::format("\n    out.tex{0}_uv = tc{0}_proj.xy;", i);
      fragmentFnPre += fmt::format("\n    var tex{0}_uv = in.tex{0}_uv.xy;", i);
    }
  }
  // Multiple TEV stages may reference the same indirect stage,
  // so we sample each indirect texture only once.
  const auto ind_scale = [](const GXIndTexScale s) -> std::string_view {
    switch (s) {
    case GX_ITS_1:
      return "1.0"sv;
    case GX_ITS_2:
      return "(1.0 / 2.0)"sv;
    case GX_ITS_4:
      return "(1.0 / 4.0)"sv;
    case GX_ITS_8:
      return "(1.0 / 8.0)"sv;
    case GX_ITS_16:
      return "(1.0 / 16.0)"sv;
    case GX_ITS_32:
      return "(1.0 / 32.0)"sv;
    case GX_ITS_64:
      return "(1.0 / 64.0)"sv;
    case GX_ITS_128:
      return "(1.0 / 128.0)"sv;
    case GX_ITS_256:
      return "(1.0 / 256.0)"sv;
    default:
      FATAL("unhandled indirect scale {}", underlying(s));
    }
  };
  for (int i = 0; i < info.usedIndStages.size(); ++i) {
    if (!info.usedIndStages.test(i)) {
      continue;
    }
    const auto& indStage = config.indStages[i];
    const u32 texCoordId = underlying(indStage.texCoordId);
    const u32 texMapId = underlying(indStage.texMapId);
    // GX applies the SU texture-coordinate scale before the indirect stage scale.
    // The shader carries normalized UVs, so convert that texel-space result back
    // into normalized coordinates for the indirect texture sample.
    const auto scaleExpr =
        fmt::format("tex{0}_uv * ubuf.texcoord_scale[{0}].xy * vec2f({1}, {2}) / ubuf.tex{3}_size_bias.xy", texCoordId,
                    ind_scale(indStage.scaleS), ind_scale(indStage.scaleT), texMapId);
    fragmentFnPre += fmt::format(
        "\n    // Indirect stage {0}"
        "\n    var t_IndTexCoord{0} = 255.0 * textureSampleBias(tex{1}, tex{1}_samp, {2}, "
        "ubuf.tex{1}_size_bias.z).abg;",
        i, texMapId, scaleExpr);
  }
  if (info.usedIndStages.any()) {
    fragmentFnPre += "\n    var t_TexCoord = vec2f(0.0);";
  }
  for (int i = 0; i < config.tevStageCount; ++i) {
    const auto& stage = config.tevStages[i];
    const bool needsIndirectCoord = stage.indTexMtxId != GX_ITM_OFF;
    const bool hasIndirectStage = stage.indTexStage < config.numIndStages;
    const bool needsTevTexCoord =
        needsIndirectCoord || stage.indTexWrapS != GX_ITW_OFF || stage.indTexWrapT != GX_ITW_OFF || stage.indTexAddPrev;
    const bool needsTextureSample = uses_texture_sample(stage);
    if (!needsTevTexCoord && !needsTextureSample) {
      continue;
    }
    const bool hasBaseTexCoord = stage.texCoordId != GX_TEXCOORD_NULL;
    const bool hasBaseTexture = stage.texMapId != GX_TEXMAP_NULL;
    const bool hasBaseCoord = hasBaseTexCoord && hasBaseTexture;
    std::string uvIn;
    if (needsTevTexCoord) {
      fragmentFnPre += fmt::format("\n    // TEV stage {} indirect", i);

      // Apply indirect texture matrix (produces a texel-space offset)
      std::string indirectOffsetTexel;
      if (needsIndirectCoord && hasIndirectStage) {
        std::string_view fmtShift;
        switch (stage.indTexFormat) {
        case GX_ITF_8:
          break;
        case GX_ITF_5:
          fmtShift = " / 8.0"sv;
          break;
        case GX_ITF_4:
          fmtShift = " / 16.0"sv;
          break;
        case GX_ITF_3:
          fmtShift = " / 32.0"sv;
          break;
        default:
          FATAL("unhandled indirect format {}", underlying(stage.indTexFormat));
        }
        if (fmtShift.empty()) {
          fragmentFnPre += fmt::format("\n    var ind{0}_coord = t_IndTexCoord{1};", i, underlying(stage.indTexStage));
        } else {
          fragmentFnPre += fmt::format("\n    var ind{0}_coord = floor(t_IndTexCoord{1}{2});", i,
                                       underlying(stage.indTexStage), fmtShift);
        }

        if (stage.indTexBiasSel != GX_ITB_NONE) {
          auto bias = stage.indTexFormat == GX_ITF_8 ? "-128.0"sv : "1.0"sv;
          auto biasS = "0.0"sv, biasT = "0.0"sv, biasU = "0.0"sv;
          if (stage.indTexBiasSel == GX_ITB_S || stage.indTexBiasSel == GX_ITB_ST || stage.indTexBiasSel == GX_ITB_SU ||
              stage.indTexBiasSel == GX_ITB_STU) {
            biasS = "1.0"sv;
          }
          if (stage.indTexBiasSel == GX_ITB_T || stage.indTexBiasSel == GX_ITB_ST || stage.indTexBiasSel == GX_ITB_TU ||
              stage.indTexBiasSel == GX_ITB_STU) {
            biasT = "1.0"sv;
          }
          if (stage.indTexBiasSel == GX_ITB_U || stage.indTexBiasSel == GX_ITB_SU || stage.indTexBiasSel == GX_ITB_TU ||
              stage.indTexBiasSel == GX_ITB_STU) {
            biasU = "1.0"sv;
          }
          fragmentFnPre += fmt::format("\n    ind{0}_coord = ind{0}_coord + vec3f({1}, {2}, {3}) * {4};", i, biasS,
                                       biasT, biasU, bias);
        }

        if (stage.indTexMtxId >= GX_ITM_0 && stage.indTexMtxId <= GX_ITM_2) {
          // Static 2x3 matrix: dot(mat_row, vec3(S,T,U)) * scale
          u32 mtxIdx = stage.indTexMtxId - GX_ITM_0;
          fragmentFnPre += fmt::format(
              "\n    let ind{0}_c0 = ubuf.ind_mtx[{1}][0];"
              "\n    let ind{0}_c1 = ubuf.ind_mtx[{1}][1];",
              i, mtxIdx);
          indirectOffsetTexel = fmt::format(
              "vec2f("
              "dot(vec3f(ind{0}_c0.xz, ind{0}_c1.x), ind{0}_coord), "
              "dot(vec3f(ind{0}_c0.yw, ind{0}_c1.y), ind{0}_coord)"
              ") * ind{0}_c1.z",
              i);
        } else if (stage.indTexMtxId >= GX_ITM_S0 && stage.indTexMtxId <= GX_ITM_S2 && hasBaseCoord) {
          // Dynamic S: result = scaled texcoord * ind_coord.x * scale / 256
          u32 mtxIdx = stage.indTexMtxId - GX_ITM_S0;
          u32 regTexCoord = underlying(stage.texCoordId);
          indirectOffsetTexel = fmt::format(
              "tex{1}_uv * ubuf.texcoord_scale[{1}].xy * ind{0}_coord.x"
              " * ubuf.ind_mtx[{2}][1][2] / 256.0",
              i, regTexCoord, mtxIdx);
        } else if (stage.indTexMtxId >= GX_ITM_T0 && stage.indTexMtxId <= GX_ITM_T2 && hasBaseCoord) {
          // Dynamic T: result = scaled texcoord * ind_coord.y * scale / 256
          u32 mtxIdx = stage.indTexMtxId - GX_ITM_T0;
          u32 regTexCoord = underlying(stage.texCoordId);
          indirectOffsetTexel = fmt::format(
              "tex{1}_uv * ubuf.texcoord_scale[{1}].xy * ind{0}_coord.y"
              " * ubuf.ind_mtx[{2}][1][2] / 256.0",
              i, regTexCoord, mtxIdx);
        }
      }

      // Don't convert to/from texel space if we can avoid it
      const bool useSimpleCoords = stage.indTexMtxId == GX_ITM_OFF && !stage.indTexAddPrev;

      // Wrap base coord and combine with the indirect translation.
      auto wrap_comp = [](GXIndTexWrap wrap, std::string&& coord) -> std::string {
        switch (wrap) {
        case GX_ITW_OFF:
          return std::move(coord);
        case GX_ITW_256:
          return fmt::format("({} % 256.0)", coord);
        case GX_ITW_128:
          return fmt::format("({} % 128.0)", coord);
        case GX_ITW_64:
          return fmt::format("({} % 64.0)", coord);
        case GX_ITW_32:
          return fmt::format("({} % 32.0)", coord);
        case GX_ITW_16:
          return fmt::format("({} % 16.0)", coord);
        case GX_ITW_0:
          return "0.0";
        default:
          FATAL("unhandled indirect wrap {}", underlying(wrap));
        }
      };
      std::string baseCoordExpr;
      if (hasBaseCoord) {
        u32 texCoordId = underlying(stage.texCoordId);
        if (useSimpleCoords) {
          baseCoordExpr = fmt::format("tex{}_uv", texCoordId);
        } else {
          fragmentFnPre +=
              fmt::format("\n    var ind{0}_texel = tex{1}_uv * ubuf.texcoord_scale[{1}].xy;", i, texCoordId);
          baseCoordExpr = fmt::format("ind{}_texel", i);
        }
      }
      std::string wrappedExpr = baseCoordExpr;
      if (!baseCoordExpr.empty() && (stage.indTexWrapS != GX_ITW_OFF || stage.indTexWrapT != GX_ITW_OFF)) {
        wrappedExpr = fmt::format("vec2f({}, {})", wrap_comp(stage.indTexWrapS, fmt::format("{}.x", baseCoordExpr)),
                                  wrap_comp(stage.indTexWrapT, fmt::format("{}.y", baseCoordExpr)));
      }

      std::string finalCoord;
      if (!wrappedExpr.empty() && !indirectOffsetTexel.empty()) {
        finalCoord = fmt::format("{} + ({})", wrappedExpr, indirectOffsetTexel);
      } else if (!wrappedExpr.empty()) {
        finalCoord = wrappedExpr;
      } else {
        finalCoord = indirectOffsetTexel;
      }

      if (info.usedIndStages.any() && !finalCoord.empty()) {
        if (stage.indTexAddPrev) {
          fragmentFnPre += fmt::format("\n    t_TexCoord += {};", finalCoord);
        } else {
          fragmentFnPre += fmt::format("\n    t_TexCoord = {};", finalCoord);
        }

        if (needsTextureSample && hasBaseTexture) {
          u32 texMapId = underlying(stage.texMapId);
          if (useSimpleCoords) {
            fragmentFnPre += fmt::format("\n    var ind{0}_uv = t_TexCoord;", i);
          } else {
            fragmentFnPre += fmt::format("\n    var ind{0}_uv = t_TexCoord / ubuf.tex{1}_size_bias.xy;", i, texMapId);
          }
          uvIn = fmt::format("ind{0}_uv", i);
        }
      }
    }
    if (!needsTextureSample) {
      continue;
    }

    CHECK(stage.texMapId != GX_TEXMAP_NULL, "unmapped texture for stage {}", i);
    CHECK(stage.texCoordId != GX_TEXCOORD_NULL, "unmapped texcoord for stage {}", i);
    if (uvIn.empty()) {
      // No indirect texturing
      uvIn = fmt::format("tex{0}_uv", underlying(stage.texCoordId));
    }
    fragmentFnPre +=
        fmt::format("\n    var sampled{0} = textureSampleBias(tex{1}, tex{1}_samp, {2}, ubuf.tex{1}_size_bias.z);", i,
                    underlying(stage.texMapId), uvIn);
    if (config.sdf != 0) {
      // GX_AURORA_SET_SDF: the sample is a distance, made coverage a screen pixel wide.
      fragmentFnPre += fmt::format("\n    {{"
                                   "\n        let sdf_d = sampled{0}.r;"
                                   "\n        let sdf_w = max(fwidth(sdf_d), 0.0001);"
                                   "\n        sampled{0} = vec4f(vec3f(clamp((sdf_d - 0.5) / sdf_w + 0.5, 0.0, 1.0)),"
                                   "\n            clamp((sdf_d - {1:.6f}) / sdf_w + 0.5, 0.0, 1.0));"
                                   "\n    }}",
                                   i, float(config.sdf) / 255.f);
    }
  }
  if (config.pbr) {
    uniBufAttrs += "\n    pbr_probe: mat3x4f,";
    uniBufAttrs += "\n    pbr_emissive: vec4f,";
    uniBufAttrs += "\n    pbr_backlight: vec4f,";
    uniBufAttrs += "\n    pbr_layer: vec4f,";
    uniBufAttrs += "\n    pbr_layer_height: vec4f,";
    uniBufAttrs += "\n    pbr_param: vec4f,";
    uniBufAttrs += "\n    pbr_up: vec4f,";
    uniBufAttrs += "\n    pbr_cube: vec4f,";
    uniBufAttrs += "\n    pbr_ambient: array<vec4f, 6>,";
    uniBufAttrs += "\n    pbr_volume: array<vec4f, 6>,";
    uniBufAttrs += "\n    pbr_tone: array<vec4f, 3>,";
    const auto pbr = pbr_func(config, info, vtxOutAttrs, vtxXfrAttrs, vtxOutIdx);
    if (!pbr.empty()) {
      fragmentFn += pbr;
      texBindings += fmt::format("\n@group(2) @binding({})\n"
                                 "var pbr_cube: texture_cube<f32>;\n"
                                 "@group(2) @binding({})\n"
                                 "var pbr_cube_samp: sampler;",
                                 MaxTextures * 2, MaxTextures * 2 + 1);
      static constexpr std::array<const char*, gfx::probe::VolumeTextures> volumeNames{"mean", "lobe", "r", "g", "b"};
      for (u32 i = 0; i < gfx::probe::VolumeTextures; ++i) {
        texBindings += fmt::format("\n@group(2) @binding({})\nvar pbr_vol_{}: texture_3d<f32>;",
                                   MaxTextures * 2 + 2 + i, volumeNames[i]);
      }
    }
  }
  if (info.usesPTTexMtx.any()) {
    uniBufAttrs += fmt::format("\n    postmtx: array<mat3x4f, {}>,", MaxPTTexMtx);
  }
  if (info.usesFog) {
    uniformPre +=
        "\n"
        "struct Fog {\n"
        "    color: vec4f,\n"
        "    a: f32,\n"
        "    b: f32,\n"
        "    c: f32,\n"
        "    range_center: f32,\n"
        "    range_k: array<vec4f, 3>,\n"
        "}";
    uniBufAttrs += "\n    fog: Fog,";

    const std::string_view fogDepth = UseReversedZ ? "(1.0 - in.pos.z)" : "in.pos.z";
    if ((config.fogType & 0x08) != 0) {
      fragmentFn += fmt::format("\n    // Orthographic fog\n    var fogBase = ubuf.fog.a * {};", fogDepth);
    } else {
      fragmentFn +=
          fmt::format("\n    // Perspective fog\n    var fogBase = ubuf.fog.a / (ubuf.fog.b - {});", fogDepth);
    }
    if (config.fogRangeEnabled) {
      fragmentFn += "\n        fogBase *= bitcast<f32>(abuf[imm.fog_range_base + u32(in.pos.x)]);";
    }
    fragmentFn += "\n    var fogF = clamp(fogBase - ubuf.fog.c, 0.0, 1.0);";
    switch (config.fogType) {
      DEFAULT_FATAL("invalid fog type {}", config.fogType);
    case GX_FOG_PERSP_LIN:
    case GX_FOG_ORTHO_LIN:
      fragmentFn += "\n    var fogZ = fogF;";
      break;
    case GX_FOG_PERSP_EXP:
    case GX_FOG_ORTHO_EXP:
      fragmentFn += "\n    var fogZ = 1.0 - exp2(-8.0 * fogF);";
      break;
    case GX_FOG_PERSP_EXP2:
    case GX_FOG_ORTHO_EXP2:
      fragmentFn += "\n    var fogZ = 1.0 - exp2(-8.0 * fogF * fogF);";
      break;
    case GX_FOG_PERSP_REVEXP:
    case GX_FOG_ORTHO_REVEXP:
      fragmentFn += "\n    var fogZ = exp2(-8.0 * (1.0 - fogF));";
      break;
    case GX_FOG_PERSP_REVEXP2:
    case GX_FOG_ORTHO_REVEXP2:
      fragmentFn +=
          "\n    fogF = 1.0 - fogF;"
          "\n    var fogZ = exp2(-8.0 * fogF * fogF);";
      break;
    }
    fragmentFn += "\n    prev = vec4f(mix(prev.rgb, ubuf.fog.color.rgb, clamp(fogZ, 0.0, 1.0)), prev.a);";
  }
  uniBufAttrs += fmt::format("\n    texcoord_scale: array<vec4f, {}>,", MaxTexCoord);
  if (info.usedIndTexMtxs.any()) {
    uniBufAttrs += fmt::format("\n    ind_mtx: array<mat2x4f, {}>,", MaxIndTexMtxs);
  }
  for (int i = 0; i < info.sampledTextures.size(); ++i) {
    if (!info.sampledTextures.test(i)) {
      continue;
    }
    uniBufAttrs += fmt::format("\n    tex{}_size_bias: vec4f,", i);
    texBindings += fmt::format(
        "\n@group(2) @binding({1})\n"
        "var tex{0}: texture_2d<f32>;\n"
        "@group(2) @binding({2})\n"
        "var tex{0}_samp: sampler;",
        i, i * 2, i * 2 + 1);
  }
  if (!prevColorNormalized && !prevAlphaNormalized) {
    fragmentFn += "\n    prev = tev_overflow_vec4f(prev);";
  } else if (!prevColorNormalized) {
    fragmentFn += "\n    prev = vec4f(tev_overflow_vec3f(prev.rgb), prev.a);";
  } else if (!prevAlphaNormalized) {
    fragmentFn += "\n    prev.a = tev_overflow_f32(prev.a);";
  }
  if (config.alphaCompare) {
    const auto comp0 = alpha_compare(config.alphaCompare.comp0, config.alphaCompare.ref0);
    const auto comp1 = alpha_compare(config.alphaCompare.comp1, config.alphaCompare.ref1);
    AlphaCompareExpr pass;
    switch (config.alphaCompare.op) {
      DEFAULT_FATAL("invalid alpha compare op {}", underlying(config.alphaCompare.op));
    case GX_AOP_AND:
      pass = alpha_compare_and(comp0, comp1);
      break;
    case GX_AOP_OR:
      pass = alpha_compare_or(comp0, comp1);
      break;
    case GX_AOP_XOR:
      pass = alpha_compare_xor(comp0, comp1);
      break;
    case GX_AOP_XNOR:
      pass = alpha_compare_xnor(comp0, comp1);
      break;
    }
    const auto discard = alpha_compare_not(pass);
    if (discard.constant == 1) {
      fragmentFn += "\n    // Alpha compare\n    discard;";
    } else if (discard.constant != 0) {
      fragmentFn +=
          "\n    // Alpha compare"
          "\n    let alphaCompare = u32(round(clamp(prev.a, 0.0, 1.0) * 255.0));";
      fragmentFn += fmt::format("\n    if ({}) {{ discard; }}", discard.expr);
    }
  }
  if constexpr (EnableNormalVisualization) {
    fragmentFn += "\n    prev = vec4f(in.nrm, prev.a);";
  }

  const auto shaderSource = fmt::format(R"""(
fn bswap32(v: u32, le: bool) -> u32 {{
  if (le) {{
    return v;
  }}
  return ((v & 0x000000FFu) << 24u) |
         ((v & 0x0000FF00u) << 8u) |
         ((v & 0x00FF0000u) >> 8u) |
         ((v & 0xFF000000u) >> 24u);
}}

fn bswap16(v: u32, le: bool) -> u32 {{
  return select(((v & 0xFFu) << 8u) | (v >> 8u), v, le);
}}

fn load_word(p: ptr<storage, array<u32>>, word_idx: u32) -> u32 {{
  // This guard is not expected to handle routine out-of-bounds accesses.
  // It appears to discourage some Adreno drivers/optimizers from storage buffer
  // optimizations that can cause visual artifacts, including vertex explosions
  // in Dusklight.
  if (word_idx < arrayLength(p)) {{
    return p[word_idx];
  }}
  return 0u;
}}

fn load_u8(p: ptr<storage, array<u32>>, byte_off: u32) -> u32 {{
  let word = load_word(p, byte_off / 4u);
  let shift = (byte_off & 3u) * 8u;
  return (word >> shift) & 0xFFu;
}}

fn load_u32_raw(p: ptr<storage, array<u32>>, byte_off: u32) -> u32 {{
  let word_idx = byte_off >> 2u;
  let sub = byte_off & 3u;
  let lo = load_word(p, word_idx);
  if (sub == 0u) {{
    return lo;
  }}
  let hi = load_word(p, word_idx + 1u);
  let shift = sub * 8u;
  return (lo >> shift) | (hi << (32u - shift));
}}

fn load_u16(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> u32 {{
  let word_idx = byte_off >> 2u;
  let sub = byte_off & 3u;
  let word = load_word(p, word_idx);
  if (sub <= 2u) {{
    return bswap16(extractBits(word, sub * 8u, 16u), le);
  }}
  let next = load_word(p, word_idx + 1u);
  let raw = extractBits(word, 24u, 8u) | (extractBits(next, 0u, 8u) << 8u);
  return bswap16(raw, le);
}}

fn load_u24(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> u32 {{
  let raw = load_u32_raw(p, byte_off) & 0x00FFFFFFu;
  if (le) {{
    return raw;
  }}
  return ((raw & 0x0000FFu) << 16u) |
         (raw & 0x00FF00u) |
         ((raw & 0xFF0000u) >> 16u);
}}

fn load_u32(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> u32 {{
  return bswap32(load_u32_raw(p, byte_off), le);
}}

fn load_f32(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> f32 {{
  return bitcast<f32>(load_u32(p, byte_off, le));
}}

fn raw_fetch_u8_1(p: ptr<storage, array<u32>>, byte_off: u32) -> u32 {{
  return load_u8(p, byte_off);
}}

fn raw_fetch_u8_2(p: ptr<storage, array<u32>>, byte_off: u32) -> vec2u {{
  let word_idx = byte_off >> 2u;
  let sub = byte_off & 3u;
  let word = load_word(p, word_idx);
  if (sub <= 2u) {{
    let shift = sub * 8u;
    return vec2u(
      extractBits(word, shift + 0u, 8u),
      extractBits(word, shift + 8u, 8u),
    );
  }}
  let next = load_word(p, word_idx + 1u);
  return vec2u(
    extractBits(word, 24u, 8u),
    extractBits(next, 0u, 8u),
  );
}}

fn raw_fetch_u8_3(p: ptr<storage, array<u32>>, byte_off: u32) -> vec3u {{
  let raw = load_u32_raw(p, byte_off);
  return vec3u(
    extractBits(raw, 0u, 8u),
    extractBits(raw, 8u, 8u),
    extractBits(raw, 16u, 8u),
  );
}}

fn raw_fetch_u8_4(p: ptr<storage, array<u32>>, byte_off: u32) -> vec4u {{
  let raw = load_u32_raw(p, byte_off);
  return vec4u(
    extractBits(raw, 0u, 8u),
    extractBits(raw, 8u, 8u),
    extractBits(raw, 16u, 8u),
    extractBits(raw, 24u, 8u),
  );
}}

fn raw_fetch_u16_1(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> u32 {{
  return load_u16(p, byte_off, le);
}}

fn raw_fetch_u16_2(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec2u {{
  return vec2u(
    load_u16(p, byte_off + 0u, le),
    load_u16(p, byte_off + 2u, le),
  );
}}

fn raw_fetch_u16_3(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec3u {{
  return vec3u(
    load_u16(p, byte_off + 0u, le),
    load_u16(p, byte_off + 2u, le),
    load_u16(p, byte_off + 4u, le),
  );
}}

fn raw_fetch_u16_4(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec4u {{
  return vec4u(
    load_u16(p, byte_off + 0u, le),
    load_u16(p, byte_off + 2u, le),
    load_u16(p, byte_off + 4u, le),
    load_u16(p, byte_off + 6u, le),
  );
}}

fn raw_fetch_f32_1(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> f32 {{
  return load_f32(p, byte_off, le);
}}

fn raw_fetch_f32_2(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec2f {{
  return vec2f(
    load_f32(p, byte_off + 0u, le),
    load_f32(p, byte_off + 4u, le),
  );
}}

fn raw_fetch_f32_3(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec3f {{
  return vec3f(
    load_f32(p, byte_off + 0u, le),
    load_f32(p, byte_off + 4u, le),
    load_f32(p, byte_off + 8u, le),
  );
}}

fn raw_fetch_f32_4(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec4f {{
  return vec4f(
    load_f32(p, byte_off + 0u, le),
    load_f32(p, byte_off + 4u, le),
    load_f32(p, byte_off + 8u, le),
    load_f32(p, byte_off + 12u, le),
  );
}}

fn fetch_u8_1(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> f32 {{
  let v = raw_fetch_u8_1(p, byte_off);
  return f32(v) / f32(1u << frac);
}}

fn fetch_s8_1(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> f32 {{
  let v = (bitcast<i32>(raw_fetch_u8_1(p, byte_off)) << 24) >> 24;
  return f32(v) / f32(1u << frac);
}}

fn fetch_u8_2(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> vec2f {{
  let v = raw_fetch_u8_2(p, byte_off);
  return vec2f(v) / f32(1u << frac);
}}

fn fetch_s8_2(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> vec2f {{
  let v = (bitcast<vec2i>(raw_fetch_u8_2(p, byte_off)) << vec2u(24u)) >> vec2u(24u);
  return vec2f(v) / f32(1u << frac);
}}

fn fetch_u8_3(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> vec3f {{
  let v = raw_fetch_u8_3(p, byte_off);
  return vec3f(v) / f32(1u << frac);
}}

fn fetch_s8_3(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> vec3f {{
  let v = (bitcast<vec3i>(raw_fetch_u8_3(p, byte_off)) << vec3u(24u)) >> vec3u(24u);
  return vec3f(v) / f32(1u << frac);
}}

fn fetch_u8_4(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> vec4f {{
  let v = raw_fetch_u8_4(p, byte_off);
  return vec4f(v) / f32(1u << frac);
}}

fn fetch_s8_4(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> vec4f {{
  let v = (bitcast<vec4i>(raw_fetch_u8_4(p, byte_off)) << vec4u(24u)) >> vec4u(24u);
  return vec4f(v) / f32(1u << frac);
}}

fn fetch_u16_1(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> f32 {{
  let v = raw_fetch_u16_1(p, byte_off, le);
  return f32(v) / f32(1u << frac);
}}

fn fetch_s16_1(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> f32 {{
  let v = bitcast<i32>(raw_fetch_u16_1(p, byte_off, le) << 16u) >> 16;
  return f32(v) / f32(1u << frac);
}}

fn fetch_u16_2(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> vec2f {{
  let v = raw_fetch_u16_2(p, byte_off, le);
  return vec2f(v) / f32(1u << frac);
}}

fn fetch_s16_2(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> vec2f {{
  let v = (bitcast<vec2i>(raw_fetch_u16_2(p, byte_off, le)) << vec2u(16u)) >> vec2u(16u);
  return vec2f(v) / f32(1u << frac);
}}

fn fetch_u16_3(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> vec3f {{
  let v = raw_fetch_u16_3(p, byte_off, le);
  return vec3f(v) / f32(1u << frac);
}}

fn fetch_s16_3(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> vec3f {{
  let v = (bitcast<vec3i>(raw_fetch_u16_3(p, byte_off, le)) << vec3u(16u)) >> vec3u(16u);
  return vec3f(v) / f32(1u << frac);
}}

fn fetch_u16_4(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> vec4f {{
  let v = raw_fetch_u16_4(p, byte_off, le);
  return vec4f(v) / f32(1u << frac);
}}

fn fetch_s16_4(p: ptr<storage, array<u32>>, byte_off: u32, frac: u32, le: bool) -> vec4f {{
  let v = (bitcast<vec4i>(raw_fetch_u16_4(p, byte_off, le)) << vec4u(16u)) >> vec4u(16u);
  return vec4f(v) / f32(1u << frac);
}}

fn fetch_f32_1(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> f32 {{
  return raw_fetch_f32_1(p, byte_off, le);
}}

fn fetch_f32_2(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec2f {{
  return raw_fetch_f32_2(p, byte_off, le);
}}

fn fetch_f32_3(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec3f {{
  return raw_fetch_f32_3(p, byte_off, le);
}}

fn fetch_f32_4(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec4f {{
  return raw_fetch_f32_4(p, byte_off, le);
}}

fn fetch_rgb565(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec4f {{
  let v = load_u16(p, byte_off, le);
  return vec4f(
    f32((v >> 11u) & 0x1Fu) / f32(0x1Fu),
    f32((v >>  5u) & 0x3Fu) / f32(0x3Fu),
    f32((v >>  0u) & 0x1Fu) / f32(0x1Fu),
    1.0,
  );
}}

fn fetch_rgb8(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec4f {{
  let v = raw_fetch_u8_3(p, byte_off);
  return vec4f(f32(v.x), f32(v.y), f32(v.z), 255.0) / 255.0;
}}

fn fetch_rgbx8(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec4f {{
  let v = raw_fetch_u8_4(p, byte_off);
  return vec4f(f32(v.x), f32(v.y), f32(v.z), 255.0) / 255.0;
}}

fn fetch_rgba4(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec4f {{
  let v = load_u16(p, byte_off, le);
  return vec4f(
    f32((v >> 12u) & 0x0Fu) / f32(0x0Fu),
    f32((v >>  8u) & 0x0Fu) / f32(0x0Fu),
    f32((v >>  4u) & 0x0Fu) / f32(0x0Fu),
    f32((v >>  0u) & 0x0Fu) / f32(0x0Fu),
  );
}}

fn fetch_rgba6(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec4f {{
  let v = load_u24(p, byte_off, le);
  return vec4f(
    f32((v >> 18u) & 0x3Fu) / f32(0x3Fu),
    f32((v >> 12u) & 0x3Fu) / f32(0x3Fu),
    f32((v >>  6u) & 0x3Fu) / f32(0x3Fu),
    f32((v >>  0u) & 0x3Fu) / f32(0x3Fu),
  );
}}

fn fetch_rgba8(p: ptr<storage, array<u32>>, byte_off: u32, le: bool) -> vec4f {{
  let v = raw_fetch_u8_4(p, byte_off);
  return vec4f(v) / 255.0;
}}

fn tev_overflow_f32(in: f32) -> f32 {{
  let byte_space = in * 255.0;
  return (byte_space - floor(byte_space / 256.0) * 256.0) / 255.0;
}}

fn tev_overflow_vec3f(in: vec3f) -> vec3f {{
  let byte_space = in * 255.0;
  return (byte_space - floor(byte_space / 256.0) * 256.0) / 255.0;
}}

fn tev_overflow_vec4f(in: vec4f) -> vec4f {{
  let byte_space = in * 255.0;
  return (byte_space - floor(byte_space / 256.0) * 256.0) / 255.0;
}}

{8}

struct Immediate {{
    vtx_start: u32,
    current_pnmtx: u32,
    fog_range_base: u32,
    _pad: u32,
    array_start0: vec4u,
    array_start1: vec4u,
    array_start2: vec4u,
}};
var<immediate> imm: Immediate;

struct Uniform {{
    render_viewport_size: vec2f,
    logical_viewport_size: vec2f,{0}
}};
@group(0) @binding(0)
var<storage, read> vbuf: array<u32>;
@group(0) @binding(1)
var<storage, read> abuf: array<u32>;
@group(1) @binding(0)
var<uniform> ubuf: Uniform;{1}

struct VertexOutput {{
    // GX multipass draws need identical depth across lighting/TEV shader variants.
    @builtin(position) @invariant pos: vec4f,{2}
}};

@vertex
fn vs_main(
    @builtin(vertex_index) vidx: u32{3}
) -> VertexOutput {{
    var out: VertexOutput;{7}{4}
    return out;
}}

@fragment
fn fs_main(in: VertexOutput{9}) -> @location(0) vec4f {{{6}{5}
    return prev;
}}
)""",
                                        uniBufAttrs, texBindings, vtxOutAttrs, vtxInAttrs, vtxXfrAttrs, fragmentFn,
                                        fragmentFnPre, vtxXfrAttrsPre, uniformPre,
                                        config.mapBatch ? ", @builtin(front_facing) map_front: bool" : "");
  if (EnableDebugPrints) {
    Log.info("Generated shader (hash {:x}): {}", hash, shaderSource);
  }

  if (config.multiview != MultiviewNone) {
    return to_multiview_source(shaderSource, info.uniformSize, static_cast<MultiviewMode>(config.multiview), config.mapBatch);
  }
  return shaderSource;
}

wgpu::ShaderModule build_shader(const ShaderConfig& config) noexcept {
  ZoneScoped;
  const auto shaderSource = build_shader_source(config);
  const auto hash = xxh3_hash(config);
  wgpu::ShaderSourceWGSL wgslDescriptor{};
  wgslDescriptor.code = shaderSource.c_str();
  const auto label = fmt::format("GX Shader {:x}", hash);
  const auto shaderDescriptor = wgpu::ShaderModuleDescriptor{
      .nextInChain = &wgslDescriptor,
      .label = label.c_str(),
  };
  return webgpu::g_device.CreateShaderModule(&shaderDescriptor);
}
} // namespace aurora::gx
