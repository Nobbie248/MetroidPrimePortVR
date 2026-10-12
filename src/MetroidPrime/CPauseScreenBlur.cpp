#include "MetroidPrime/CPauseScreenBlur.hpp"

#include "MetroidPrime/Cameras/CCameraFilterPass.hpp"
#include "MetroidPrime/SFX/UI.h"
#include "MetroidPrime/Tweaks/CTweakGui.hpp"
#include "MetroidPrime/Tweaks/CTweakGuiColors.hpp"

#include "Kyoto/Audio/CSfxManager.hpp"
#include "Kyoto/Graphics/CGraphics.hpp"
#ifdef TARGET_PC
#include "MetaRender/CCubeRenderer.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "MetroidPrime/Cameras/CGameCamera.hpp"
#include "aurora/gfx.h"
#include "dolphin/gx/GXAurora.h"

#include <cmath>
#endif

#include "float.h"
#include "rstl/math.hpp"

CPauseScreenBlur::CPauseScreenBlur()
: x4_mapLightQuarter(gpSimplePool->GetObj("TXTR_MapLightQuarter"))
, x10_prevState(kS_InGame)
, x14_nextState(kS_InGame)
, x18_blurAmt(0.f)
, x50_24_blurring(false)
, x50_25_gameDraw(true)
#ifdef TARGET_PC
, mPortVrLiveWorld(false)
, mPortVrCopyPending(false)
#endif
{
}

CPauseScreenBlur::~CPauseScreenBlur() {}

#ifdef TARGET_PC
namespace {
// PortVr: the pause blur's CRT scan lines in the headset: dark bands at fixed
// elevations on a sphere around the camera, routed AURORA_STEREO_ROUTE_SKY so
// each eye sees them with the head's rotation only. They stay put in the world
// (at infinity, without disparity) while the head looks around, where the
// TV's lines (CCameraFilterPass::DrawScanLines) are glued to the picture.
// The pitch is about what those 120 lines get across a headset's view, with
// the TV's even split of dark band and gap. Each band's edges are rings of
// flat sides, whose middles sag below the ring's elevation by about
// sin(e) cos(e) (pi / sides)^2 / 2: with 48 sides at most 0.06 degree, well
// under a band (12 sides bent the lines into chevrons). The ring starts half
// a side round, so a side's middle, not a corner, is straight ahead.
const float kPortVrScanPitchDegrees = 0.8f;
const float kPortVrScanReachDegrees = 88.f; // above and below the horizon
const int kPortVrScanRingSides = 48;
const float kPortVrScanRadius = 10.f; // game units; at infinity any radius looks the same

void PortVrDrawWorldScanLines(const CStateManager& mgr, const CColor& color) {
  const CGraphics::CProjectionState projection = CGraphics::GetProjectionState();
  const CTransform4f viewPoint = CGraphics::GetViewMatrix();
  const CTransform4f model = CGraphics::GetModelMatrix();
  // The mono picture (the desktop mirror) sees them through the game
  // camera's field of view; each eye puts its own frustum in its place.
  const CViewport& viewport = CGraphics::GetViewport();
  const float aspect = viewport.mHeight > 0
                           ? static_cast< float >(viewport.mWidth) / static_cast< float >(viewport.mHeight)
                           : 1.f;
  CGraphics::SetPerspective(mgr.GetCameraManager()->GetCurrentCamera(mgr).GetFov(), aspect, 0.1f,
                            2.f * kPortVrScanRadius);
  CGraphics::SetViewPointMatrix(CTransform4f::Identity());
  gpRender->SetModelMatrix(CTransform4f::Identity());
  gpRender->SetBlendMode_ColorMultiply();
  gpRender->SetDepthReadWrite(false, false);
  CGraphics::SetCullMode(kCM_None);
  AuroraSetStereoDrawRoute(AURORA_STEREO_ROUTE_SKY);
  const float degrees = 3.14159265f / 180.f;
  const float sideRadians = 2.f * 3.14159265f / kPortVrScanRingSides;
  const int bands = static_cast< int >(2.f * kPortVrScanReachDegrees / kPortVrScanPitchDegrees);
  // One strip per band: its low and high edges, every vertex the radius away.
  for (int band = 0; band < bands; ++band) {
    const float low = (-kPortVrScanReachDegrees + kPortVrScanPitchDegrees * band) * degrees;
    const float high = low + 0.5f * kPortVrScanPitchDegrees * degrees;
    const float r0 = kPortVrScanRadius * std::cos(low);
    const float z0 = kPortVrScanRadius * std::sin(low);
    const float r1 = kPortVrScanRadius * std::cos(high);
    const float z1 = kPortVrScanRadius * std::sin(high);
    gpRender->BeginTriangleStrip(2 * (kPortVrScanRingSides + 1));
    gpRender->PrimColor(color);
    for (int side = 0; side <= kPortVrScanRingSides; ++side) {
      const float a = sideRadians * (static_cast< float >(side) + 0.5f);
      const float c = std::cos(a);
      const float s = std::sin(a);
      gpRender->PrimVertex(CVector3f(r0 * c, r0 * s, z0));
      gpRender->PrimVertex(CVector3f(r1 * c, r1 * s, z1));
    }
    gpRender->EndPrimitive();
  }
  AuroraSetStereoDrawRoute(AURORA_STEREO_ROUTE_WORLD);
  CGraphics::SetCullMode(kCM_Front);
  gpRender->SetBlendMode_AlphaBlended();
  gpRender->SetModelMatrix(model);
  CGraphics::SetViewPointMatrix(viewPoint);
  CGraphics::SetProjectionState(projection);
}
} // namespace
#endif

void CPauseScreenBlur::OnNewInGameGuiState(EInGameGuiState state, const CStateManager& stateMgr) {
  bool valid = false;
  if (state >= 0 && state <= 1) {
    valid = true;
  }
  if (valid) {
    SetState(kS_InGame);
    return;
  }

  switch (state) {
  case kIGGS_MapScreen:
    SetState(kS_MapScreen);
    break;
  case kIGGS_PauseSaveGame:
    SetState(kS_SaveGame);
    break;
  case kIGGS_PauseHUDMessage:
    SetState(kS_HUDMessage);
    break;
  case kIGGS_PauseGame:
  case kIGGS_PauseLogBook:
    SetState(kS_Pause);
    break;
  }
}

void CPauseScreenBlur::Update(float dt, const CStateManager& stateMgr, bool b) {
  if (x10_prevState == x14_nextState)
    return;

  if (x18_blurAmt < 0.f)
    x18_blurAmt = rstl::min_val(0.f, 2.f * dt + x18_blurAmt);
  else if (x18_blurAmt > 0.f)
    x18_blurAmt = rstl::min_val(1.f, 2.f * dt + x18_blurAmt);

  if (x18_blurAmt == 0.f || x18_blurAmt == 1.f)
    OnBlurComplete(b);

  if (x18_blurAmt == 0.f && b) {
    x1c_camBlur.DisableBlur(0.f);
  } else {
    x1c_camBlur.SetBlur(CCameraBlurPass::kBT_HiBlur,
                        gpTweakGui->GetPauseBlurFactor() * GetBlurAmtInline(), 0.f, true);
    x50_24_blurring = true;
  }
}

void CPauseScreenBlur::Draw(const CStateManager& mgr) {
#ifdef TARGET_PC
  if (mPortVrLiveWorld) {
    // PortVr: the world under the blur is live, so this frame's picture is
    // copied again (each eye's own: aurora takes a stereo copy per eye) rather
    // than the first copy kept. FreePersistentFbTexture only drops the
    // 'already copied' flag; the copy keeps the persistent, opaque look.
    x1c_camBlur.FreePersistentFbTexture();
  }
#endif
  x1c_camBlur.Draw();
  CGraphics::DisableAllLights();
  CGraphics::SetAmbientColor(CColor(0xffffffff));
  const float t = fabs(x18_blurAmt);
  if (x1c_camBlur.GetCurrType() != CCameraBlurPass::kBT_NoBlur) {
    const CColor& white = CColor::White();
    const CColor tint = CColor::Lerp(white, gpTweakGuiColors->GetPauseBlurFilterColor(), t);
#ifdef TARGET_PC
    if (mPortVrLiveWorld) {
      // PortVr: in the headset the quarters' vignette (TXTR_MapLightQuarter:
      // white at the centre, a quarter as bright at the corners) would darken
      // the edges of the lens view. The tint alone, as the picture's centre
      // gets it.
      CCameraFilterPass::DrawFilter(CCameraFilterPass::kFT_Multiply,
                                    CCameraFilterPass::kFS_Fullscreen, tint, nullptr, t);
    } else
#endif
    {
      CCameraFilterPass::DrawFilter(CCameraFilterPass::kFT_Multiply,
                                    CCameraFilterPass::kFS_FullscreenQuarters, tint,
                                    *x4_mapLightQuarter, t);
    }
    const CColor lines = CColor::Lerp(CColor::White(), CColor(0xc0c0c0ff), t);
#ifdef TARGET_PC
    if (mPortVrLiveWorld) {
      // PortVr: in the headset the scan lines stay put in the world.
      PortVrDrawWorldScanLines(mgr, lines);
    } else
#endif
    {
      CCameraFilterPass::DrawFilter(CCameraFilterPass::kFT_Multiply,
                                    CCameraFilterPass::kFS_ScanLinesEven, lines, nullptr, t);
    }
  }

  if (x50_24_blurring && x1c_camBlur.GetNoPersistentCopy()) {
    x50_24_blurring = false;
    bool stopGameDraw = true;
#ifdef TARGET_PC
    // PortVr: the live world keeps being drawn (PortVrSetLiveWorld). A copy
    // the live view's end asked for has now been taken.
    stopGameDraw = !mPortVrLiveWorld;
    mPortVrCopyPending = false;
#endif
    if (stopGameDraw) {
      x50_25_gameDraw = false;
    }
  }
}

#ifdef TARGET_PC
void CPauseScreenBlur::PortVrSetLiveWorld(bool live) {
  // Any paused screen's state: a message box, the save prompt, the pause menu
  // and logbook, the map. For the last three CInGameGuiManager::EnsureStates
  // then skips the area texture dump, which needs the world stopped.
  const bool messageState = x10_prevState != kS_InGame || x14_nextState != kS_InGame;
  const bool wasLive = mPortVrLiveWorld;
  mPortVrLiveWorld = live && messageState;
  if (mPortVrLiveWorld) {
    // The world may have stopped while the box was on the virtual screen (the
    // setting turned on, or the headset came back, with the box up): draw it
    // again from this frame.
    x50_25_gameDraw = true;
    mPortVrCopyPending = false;
  } else if (wasLive && messageState && x50_25_gameDraw) {
    // Leaving the live view with the box still up (the setting turned off, the
    // headset asleep): take one copy after this frame's world, then stop the
    // world as the TV does (Draw's latch). The copy last taken cannot stand in:
    // the world's own copies (refracting particles, glass, the space warp) go
    // to the same spare buffer at other sizes, and a copy found at another size
    // is not used. The visor and the HUD are drawn for that copy, as the TV's
    // copy holds them (PortVrHudUnderBlur).
    x1c_camBlur.FreePersistentFbTexture();
    x50_24_blurring = true;
    mPortVrCopyPending = true;
  }
}
#endif

void CPauseScreenBlur::OnBlurComplete(bool b) {
  if (x14_nextState == kS_InGame && !b)
    return;
  x10_prevState = x14_nextState;
  if (x10_prevState == kS_InGame) {
    x50_25_gameDraw = true;
#ifdef TARGET_PC
    mPortVrCopyPending = false;
#endif
  }
}

void CPauseScreenBlur::SetState(EState state) {
  switch (x10_prevState) {
  case kS_InGame:
    if (state != kS_InGame) {
      CSfxManager::SetChannel(CSfxManager::kSC_PauseScreen);
      if (state == kS_HUDMessage)
        CSfxManager::SfxStart(SFXui_x_override_00, 0x7f, 0x40, false, CSfxManager::kMedPriority);
      else if (state == kS_MapScreen)
        CSfxManager::SfxStart(SFXui_x_pause_00, 0x7f, 0x40, false, CSfxManager::kMedPriority);
      x18_blurAmt = FLT_EPSILON;
    }
    break;
  case kS_MapScreen:
  case kS_SaveGame:
  case kS_HUDMessage:
  case kS_Pause:
    break;
  }

  switch (state) {
  case kS_InGame: {
    bool valid = true;
    if (x10_prevState == kS_InGame && x14_nextState == kS_InGame) {
      valid = false;
    }
    if (valid) {
      CSfxManager::SetChannel(CSfxManager::kSC_Game);

      if (x10_prevState == kS_HUDMessage)
        CSfxManager::SfxStart(SFXui_x_override_01, 0x7f, 0x40, false, CSfxManager::kMedPriority);
      else if (x10_prevState == kS_MapScreen)
        CSfxManager::SfxStart(SFXui_x_pause_01, 0x7f, 0x40, false, CSfxManager::kMedPriority);

      x18_blurAmt = -1.f;
    }
    break;
  }
  case kS_MapScreen:
  case kS_SaveGame:
  case kS_HUDMessage:
  case kS_Pause:
    break;
  }

  x14_nextState = state;
}

float CPauseScreenBlur::GetBlurAmt() const { return GetBlurAmtInline(); }
