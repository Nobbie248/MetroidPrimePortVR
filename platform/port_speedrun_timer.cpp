// The on-screen in-game time, drawn with the game's own font (FONT_Deface14B)
// after the frame's IOWins, so it sits over the HUD and the pause screens.
#include "port_speedrun_timer.h"

#include "Kyoto/CResFactory.hpp"
#include "Kyoto/CSimplePool.hpp"
#include "Kyoto/Graphics/CGraphics.hpp"
#include "Kyoto/TToken.hpp"
#include "Kyoto/Text/CDrawStringOptions.hpp"
#include "Kyoto/Text/CRasterFont.hpp"
#include "Kyoto/Text/CTextColor.hpp"
#include "Kyoto/Text/CTextRenderBuffer.hpp"
#include "MetaRender/CCubeRenderer.hpp"
#include "MetroidPrime/Player/CGameState.hpp"
#include "port_debug.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace PortSpeedrunTimer {
namespace {

// Text size relative to the font's native 14 px, on the 448-line frame buffer.
const float kScale = 1.25f;
const float kMargin = 10.f;

TCachedToken< CRasterFont >* sFont = nullptr;
CTextRenderBuffer* sBuffer = nullptr;
int sBufferWidth = 0;
char sShown[32] = {};

int Advance(const CRasterFont& font, wchar_t c) {
  const CGlyph* glyph = font.GetGlyph(c);
  return glyph != nullptr ? glyph->GetA() + glyph->GetB() + glyph->GetC() : 0;
}

// Lays the text out with every digit in a cell as wide as the widest one, so
// right-justified time doesn't shift sideways as the digits change.
void Build(CRasterFont& font, const char* text) {
  int digit = 0;
  for (wchar_t c = L'0'; c <= L'9'; ++c) {
    const int advance = Advance(font, c);
    if (advance > digit) {
      digit = advance;
    }
  }
  CDrawStringOptions options;
  options.SetTextDirection(kTD_Horizontal);
  options.SetPaletteEntry(0, CTextColor(255, 255, 255, 255).GetRGBA());
  options.SetPaletteEntry(1, CTextColor(0, 0, 0, 204).GetRGBA());
  options.SetPaletteEntry(2, CTextColor(255, 255, 255, 255).GetRGBA());

  delete sBuffer;
  sBuffer = rs_new CTextRenderBuffer(CTextRenderBuffer::kM_AllocTally);
  for (int pass = 0; pass < 2; ++pass) {
    if (pass == 1) {
      sBuffer->SetMode(CTextRenderBuffer::kM_BufferFill);
    }
    sBuffer->AddFontChange(*sFont);
    int x = 0;
    for (const char* p = text; *p != '\0'; ++p) {
      const wchar_t c = static_cast< wchar_t >(*p);
      const int advance = Advance(font, c);
      const int cell = (c >= L'0' && c <= L'9') ? digit : advance;
      int xOut;
      int yOut;
      // DrawString also queues the palette; the rest of the line reuses it.
      if (p == text) {
        font.DrawString(options, x + (cell - advance) / 2, font.GetBaseLine(), xOut, yOut,
                        sBuffer, &c, 1);
      } else {
        font.SinglePassDrawString(options, x + (cell - advance) / 2, font.GetBaseLine(), xOut,
                                  yOut, sBuffer, &c, 1);
      }
      x += cell;
    }
    sBufferWidth = x;
  }
}

} // namespace

void FormatTime(double seconds, char* out, size_t size) {
  const long long cs = static_cast< long long >(std::floor(seconds * 100.0));
  if (cs >= 360000) {
    std::snprintf(out, size, "%lld:%02lld:%02lld.%02lld", cs / 360000, cs / 6000 % 60,
                  cs / 100 % 60, cs % 100);
  } else {
    std::snprintf(out, size, "%lld:%02lld.%02lld", cs / 6000, cs / 100 % 60, cs % 100);
  }
}

void Draw() {
  if (!PortDebug::SpeedrunTimer() || PortDebug::StateManager() == nullptr ||
      gpGameState == nullptr || gpSimplePool == nullptr || gpRender == nullptr) {
    return;
  }
  if (sFont == nullptr) {
    const SObjectTag* tag = gpResourceFactory->GetResourceIdByName("FONT_Deface14B");
    if (tag == nullptr) {
      return;
    }
    sFont = rs_new TCachedToken< CRasterFont >(gpSimplePool->GetObj(*tag));
    sFont->Lock();
  }
  if (!sFont->TryCache()) {
    return;
  }
  CRasterFont& font = *sFont->GetObject();
  if (!font.IsFinishedLoading()) {
    return;
  }

  char text[32];
  FormatTime(gpGameState->GetTotalPlayTime(), text, sizeof(text));
  if (sBuffer == nullptr || std::strcmp(text, sShown) != 0) {
    std::memcpy(sShown, text, sizeof(sShown));
    Build(font, text);
  }

  const float width = static_cast< float >(CGraphics::GetViewportWidth());
  const float height = static_cast< float >(font.GetMonoHeight());
  gpRender->SetViewportOrtho(false, -4096.f, 4096.f);
  // The line's top-left corner; the text is laid out y-down, the ortho view is z-up.
  gpRender->SetModelMatrix(
      CTransform4f::Translate(width - kMargin - sBufferWidth * kScale, 0.f,
                              kMargin + height * kScale) *
      CTransform4f::Scale(CVector3f(kScale, 1.f, -kScale)));
  CGraphics::SetCullMode(kCM_None);
  gpRender->SetDepthReadWrite(false, false);
  sBuffer->Render(CColor::White(), 0.f);
}

} // namespace PortSpeedrunTimer
