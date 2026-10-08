#ifndef _CGUIFRAME
#define _CGUIFRAME

#include "GuiSys/CGuiWidgetIdDB.hpp"
#include "rstl/string.hpp"
#ifdef TARGET_PC
#include <utility>
#include <vector>
#endif

class CFinalInput;
class CGuiSys;
class CGuiWidget;
class CGuiCamera;
class CGuiLight;
class CGuiHeadWidget;
class CGuiWidgetDrawParms;
class CSimplePool;
class CGuiFrame {
public:
  CGuiFrame(uint id, CGuiSys& sys, int a, int b, int c, CSimplePool* sp);
  ~CGuiFrame();
  static CGuiFrame* CreateFrame(uint id, CGuiSys& sys, CInputStream& in, CSimplePool* sp);
#if VERSION >= VERSION_GM8P_00 && VERSION != VERSION_GM8E_02
  int LoadWidgetsInGame(CInputStream& in, CSimplePool* sp, uint version);
#else
  int LoadWidgetsInGame(CInputStream& in, CSimplePool* sp);
#endif
  void Initialize();
  void Touch() const;
  void SortDrawOrder();
  CGuiLight* GetFrameLight(int idx);
  void Update(float dt);
  void Draw(const CGuiWidgetDrawParms& parms) const;
  void ProcessUserInput(const CFinalInput& input);
  CGuiWidget* FindWidget(const short id) const;
  CGuiWidget* FindWidget(const char* name) const;
  CGuiWidget* FindWidget(const rstl::string& name) const;
  bool GetIsFinishedLoading() const;

  CGuiCamera* GetFrameCamera() const { return x14_camera; }
  void SetFrameCamera(CGuiCamera* camera);
  void AddLight(CGuiLight* light);
  void RemoveLight(CGuiLight* light);
  void SetHeadWidget(CGuiHeadWidget* widget);
  void RemoveWidgetFromDrawList(CGuiWidget* widget);

  short AddWidgetToIDDB(const rstl::string& name) { return x18_db.AddWidget(name); }
  CGuiWidgetIdDB& WidgetIdDB() { return x18_db; }

  CGuiSys& GetGuiSys() const { return x8_guiSys; }
  // Port: the FRME this frame was read from.
  uint GetId() const { return x0_id; }

  void EnableLights(uint mask) const;
  void DisableLights() const;

#ifdef TARGET_PC
  // Port: the HUD aspect spread moves each widget by its own anchor. Widgets of one compact
  // cluster (a visor/beam selector) name a shared anchor so they move as one unit.
  void SetSpreadAnchor(const CGuiWidget* member, const CGuiWidget* anchor);
  // Port: the same for every widget parented (at any depth) under `root`, with `root` as anchor.
  void SetSpreadAnchorTree(const CGuiWidget* root);
  // Port: under the vertical spread, stretch `widget` with the frame decoration instead of moving
  // it rigidly (the missile and threat bars, which run along the side struts).
  void SetSpreadStretch(const CGuiWidget* widget);
  // Port: the same for `root` and every widget parented (at any depth) under it.
  void SetSpreadStretchTree(const CGuiWidget* root);
  // Port: under the vertical spread, slide the models under `root` (at any depth) straight up or
  // down without pitching them, all those above the view centre by one offset and all those below
  // by another, so pieces of one assembly (the helmet shell, its glass and lights) stay together.
  void SetSpreadSlideTree(const CGuiWidget* root);
  // Port: whether a loaded model of this frame has a Remastered interference material
  // (kStateFlag_PortHudInterference), i.e. the frame fades in through DYIN, not retail's static.
  bool PortHasHudInterference() const;
#endif

private:
  uint x0_id;
  uint x4_;
  CGuiSys& x8_guiSys;
  CGuiHeadWidget* xc_headWidget;
  CGuiWidget* x10_rootWidget;
  CGuiCamera* x14_camera;
  CGuiWidgetIdDB x18_db;
  rstl::vector< CGuiWidget* > x2c_widgets;
  rstl::vector< CGuiLight* > x3c_lights;
  int x4c_a;
  int x50_b;
  int x54_c;
  mutable bool x58_24_loaded : 1;
#ifdef TARGET_PC
  std::vector< std::pair< const CGuiWidget*, const CGuiWidget* > > mSpreadAnchors;
  std::vector< const CGuiWidget* > mSpreadStretch;
  std::vector< const CGuiWidget* > mSpreadSlide;
#endif
};
CHECK_SIZEOF(CGuiFrame, 0x5c);

#endif // _CGUIFRAME
