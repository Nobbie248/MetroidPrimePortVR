#ifndef _CGUICAMERA
#define _CGUICAMERA

#include "GuiSys/CGuiWidget.hpp"
#include "GuiSys/CGuiWidgetDrawParms.hpp"

class CGuiCamera : public CGuiWidget {
public:
  enum EProjection {
    kProjection_Perspective,
    kProjection_Orthographic,
  };

  union UCameraParms {
    struct {
      float fov;
      float aspect;
      float znear;
      float zfar;
    } perspective;
    struct {
      float left;
      float right;
      float top;
      float bottom;
      float znear;
      float zfar;
    } orthographic;
  };

  static CGuiWidget* Create(CGuiFrame* frame, CInputStream& in, CSimplePool* sp);
  CGuiCamera(const CGuiWidgetParms& parms, float fov, float aspect, float znear, float zfar);
  CGuiCamera(const CGuiWidgetParms& parms, float left, float right, float top, float bottom,
             float znear, float zfar);

  void Draw(const CGuiWidgetDrawParms& parms) const override;
  UCameraParms GetParms() const { return mCameraParms; }
  void SetParms(UCameraParms parms) { mCameraParms = parms; }
  // GUI cameras opt in so their projection tracks the widescreen render aspect
  // instead of stretching. Set for the in-game HUD, the pause/map screens and
  // the front end; anything left unset keeps its authored aspect.
  void SetAspectMatch(bool match) { xb9_aspectMatch = match; }
  // Widescreen HUD spread for this frame's widgets: 1.0 when inactive.
  float GetAspectSpread() const { return mSpread; }
  float GetAspectSpreadCenterX() const { return mSpreadCenterX; }
  // Perspective widgets slide in their plane and turn outward in place.
  bool GetAspectSpreadAboutEye() const { return mSpreadAboutEye; }
  const CTransform4f& GetAspectSpreadView() const { return mSpreadView; }
  CTransform4f GetAspectSpreadTransform(const CVector3f& worldAnchor) const;
  // HUD scale: frames that opt in shrink about the view centre
  // (see PortDebug::HudScale).
  void SetHudScaled(bool scaled) { mHudScaled = scaled; }
  // 1.0 when inactive; computed by the last Draw.
  float GetHudScale() const { return mHudScale; }
  // PortVr: the tangents of the half angles the perspective view the last Draw
  // set up spans across and up (after the aspect match); zero for an
  // orthographic camera or before the first Draw. The headset lays the HUD's
  // 2D draws on a plane spanning the same angles (CInGameGuiManager::Draw).
  float GetDrawTanHalfWidth() const { return mDrawTanHalfWidth; }
  float GetDrawTanHalfHeight() const { return mDrawTanHalfHeight; }
  // The HUD scale alone, as a world-space transform; scaleWeight fades it
  // (0 = identity).
  CTransform4f GetHudScaleTransform(float scaleWeight = 1.f) const;
  // The widescreen spread followed by the HUD scale for a widget anchored at
  // worldAnchor.
  CTransform4f GetHudTransform(const CVector3f& worldAnchor, float scaleWeight = 1.f) const;

  FourCC GetWidgetTypeID() const override { return 'CAMR'; }

  CVector3f ConvertToScreenSpace(const CVector3f& point) const;

public:
  EProjection xb8_projection;
  bool xb9_aspectMatch = false;
  UCameraParms mCameraParms;
  // Port: horizontal spread applied to this frame's top-level widgets so the
  // HUD reaches the wide viewport edges without distorting element shapes.
  // Computed in the const Draw() and read immediately by CGuiFrame::Draw.
  mutable float mSpread = 1.f;
  mutable float mSpreadCenterX = 0.f;
  mutable bool mSpreadAboutEye = false;
  // Camera-to-world transform used by the last Draw.
  mutable CTransform4f mSpreadView = CTransform4f::Identity();
  bool mHudScaled = false;
  mutable float mHudScale = 1.f;
  // PortVr: see GetDrawTanHalfWidth.
  mutable float mDrawTanHalfWidth = 0.f;
  mutable float mDrawTanHalfHeight = 0.f;
  // The view centre drawn by the last Draw (orthographic cameras may be offset).
  mutable float mCenterX = 0.f;
  mutable float mCenterZ = 0.f;
};

#endif // _CGUICAMERA
