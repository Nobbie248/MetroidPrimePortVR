#ifndef _CPAUSESCREENBLUR
#define _CPAUSESCREENBLUR

#include "MetroidPrime/Cameras/CCameraBlurPass.hpp"
#include "MetroidPrime/CInGameGuiManagerCommon.hpp"

#include "Kyoto/TToken.hpp"

class CTexture;
class CStateManager;

class CPauseScreenBlur {
public:
  enum EState { kS_InGame, kS_MapScreen, kS_SaveGame, kS_HUDMessage, kS_Pause };

  CPauseScreenBlur();
  virtual ~CPauseScreenBlur();

  void OnNewInGameGuiState(EInGameGuiState state, const CStateManager& stateMgr);
  bool IsGameDraw() const { return x50_25_gameDraw; }
  void Update(float dt, const CStateManager& stateMgr, bool);
  void Draw(const CStateManager& stateMgr);
  float GetBlurAmt() const;
  bool IsNotTransitioning() const { return x10_prevState == x14_nextState; }
#ifdef TARGET_PC
  // PortVr: a message box, the save prompt, the pause menu, logbook or map is
  // shown in the headset's immersive view (PortVr::VrMessageBoxInView,
  // VrGameMenuInView), set by CInGameGuiManager::PortVrRefreshPausedScreens
  // every tick and frame. The world keeps being drawn under the blur and
  // the blur's framebuffer copy is retaken every frame, so each eye blurs its
  // own live view, instead of the one frozen copy the TV keeps showing.
  void PortVrSetLiveWorld(bool live);
  bool PortVrLiveWorld() const { return mPortVrLiveWorld; }
  // The visor and the HUD go under the blur: while the world is live, and on
  // the frame that takes the one copy kept once the live view ends mid-box.
  bool PortVrHudUnderBlur() const { return mPortVrLiveWorld || mPortVrCopyPending; }
#endif

private:
  TLockedToken< CTexture > x4_mapLightQuarter;
  EState x10_prevState;
  EState x14_nextState;
  float x18_blurAmt;   
  CCameraBlurPass x1c_camBlur;
  bool x50_24_blurring : 1;
  bool x50_25_gameDraw : 1;
#ifdef TARGET_PC
  bool mPortVrLiveWorld;
  bool mPortVrCopyPending;
#endif

  void OnBlurComplete(bool);
  void SetState(EState state);
  float GetBlurAmtInline() const { return fabs(x18_blurAmt); }
};

#endif // _CPAUSESCREENBLUR
