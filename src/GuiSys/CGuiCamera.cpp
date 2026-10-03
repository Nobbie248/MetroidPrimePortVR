#include "GuiSys/CGuiCamera.hpp"
#include "GuiSys/CGuiFrame.hpp"
#include "GuiSys/CGuiWidget.hpp"
#include "GuiSys/CGuiWidgetDrawParms.hpp"
#include "Kyoto/Alloc/CMemory.hpp"
#include "Kyoto/Math/CVector3f.hpp"
#include "Kyoto/Math/CRelAngle.hpp"
#include <cmath>
#include <Kyoto/Graphics/CGraphics.hpp>

#include <Kyoto/Streams/CInputStream.hpp>

#include "port_debug.h"

CGuiWidget* CGuiCamera::Create(CGuiFrame* frame, CInputStream& in, CSimplePool* sp) {
  CGuiWidgetParms parms = ReadWidgetHeader(frame, in);
  EProjection proj = static_cast< EProjection >(in.ReadLong());
  CGuiCamera* camera = nullptr;
  if (proj == kProjection_Perspective) {
    const float fov = in.ReadFloat();
    const float aspect = in.ReadFloat();
    const float znear = in.ReadFloat();
    const float zfar = in.ReadFloat();
    camera = rs_new CGuiCamera(parms, fov, aspect, znear, zfar);
  } else if (proj == kProjection_Orthographic) {
    const float left = in.ReadFloat();
    const float right = in.ReadFloat();
    const float top = in.ReadFloat();
    const float bottom = in.ReadFloat();
    const float znear = in.ReadFloat();
    const float zfar = in.ReadFloat();
    camera = rs_new CGuiCamera(parms, left, right, top, bottom, znear, zfar);
  }

  frame->SetFrameCamera(camera);
  camera->ParseBaseInfo(frame, in, parms);
  return camera;
}

CGuiCamera::CGuiCamera(const CGuiWidgetParms& parms, float fov, float aspect, float znear,
                       float zfar)
: CGuiWidget(parms) {
  xb8_projection = kProjection_Perspective;
  CVector3f(1.f, 0.f, 0.f).Normalize();
  mCameraParms.perspective.fov = fov;
  mCameraParms.perspective.aspect = aspect;
  mCameraParms.perspective.znear = znear;
  mCameraParms.perspective.zfar = zfar;
}
CGuiCamera::CGuiCamera(const CGuiWidgetParms& parms, float left, float right, float top,
                       float bottom, float znear, float zfar)
: CGuiWidget(parms) {
  xb8_projection = kProjection_Orthographic;
  mCameraParms.orthographic.left = left;
  mCameraParms.orthographic.right = right;
  mCameraParms.orthographic.top = top;
  mCameraParms.orthographic.bottom = bottom;
  mCameraParms.orthographic.znear = znear;
  mCameraParms.orthographic.zfar = zfar;
}

void CGuiCamera::Draw(const CGuiWidgetDrawParms& parms) const {
  // The FRME cameras are authored for the original 4:3 render target. When the
  // port widens the framebuffer for widescreen, using the stored aspect would
  // stretch the whole GUI (HUD, menus) across the wider viewport. Match the
  // current render aspect instead so UI keeps its proportions; 4:3 is left
  // exactly as authored.
  float renderAspect = 0.f;
  // A mod frame authored for a wider screen (Remastered's are 16:9) is fitted
  // to 4:3 as well: its width is kept by widening the vertical view instead of
  // being squeezed into the narrower viewport.
  bool fitWidth = false;
  if (xb9_aspectMatch) {
    const float vw = static_cast< float >(CGraphics::GetViewportWidth());
    const float vh = static_cast< float >(CGraphics::GetViewportHeight());
    if (vw > 0.f && vh > 0.f) {
      renderAspect = vw / vh;
    }
    if (PortDebug::AspectMode() == PortDebug::kAspect_4_3) {
      const float authored =
          xb8_projection == kProjection_Perspective
              ? mCameraParms.perspective.aspect
              : (mCameraParms.orthographic.right - mCameraParms.orthographic.left) /
                    (mCameraParms.orthographic.top - mCameraParms.orthographic.bottom);
      if (renderAspect > 0.f && authored > renderAspect * 1.1f) {
        fitWidth = !PortDebug::HudWide();
      } else {
        renderAspect = 0.f;
      }
    }
  }

  mSpread = 1.f;
  mSpreadCenterX = 0.f;
  mSpreadAboutEye = false;
  mHudScale = mHudScaled ? static_cast< float >(PortDebug::HudScale()) / 100.f : 1.f;

  if (xb8_projection == kProjection_Perspective) {
    const float authored = mCameraParms.perspective.aspect;
    const float aspect = renderAspect > 0.f ? renderAspect : authored;
    // Widening a fixed FOV keeps the projection uniform; the HUD elements stay
    // correctly shaped but are pulled toward the centre, so spread their
    // positions to reach the true corners.
    if (renderAspect > 0.f && authored > 0.f && PortDebug::HudWide()) {
      mSpread = renderAspect / authored;
      mSpreadAboutEye = true;
    }
    float fov = mCameraParms.perspective.fov;
    if (fitWidth) {
      constexpr float kDegToRad = 3.14159265f / 180.f;
      fov = 2.f * std::atan(std::tan(0.5f * fov * kDegToRad) * authored / renderAspect) / kDegToRad;
    }
    mCenterX = 0.f;
    mCenterZ = 0.f;
    CGraphics::SetPerspective(fov, aspect, mCameraParms.perspective.znear,
                              mCameraParms.perspective.zfar);
  } else {
    float left = mCameraParms.orthographic.left;
    float right = mCameraParms.orthographic.right;
    float top = mCameraParms.orthographic.top;
    float bottom = mCameraParms.orthographic.bottom;
    if (fitWidth) {
      const float middle = 0.5f * (top + bottom);
      const float halfHeight = 0.5f * (right - left) / renderAspect;
      top = middle + halfHeight;
      bottom = middle - halfHeight;
    } else if (renderAspect > 0.f) {
      const float center = 0.5f * (left + right);
      const float halfWidth = 0.5f * (top - bottom) * renderAspect;
      left = center - halfWidth;
      right = center + halfWidth;
      const float authoredWidth = mCameraParms.orthographic.right - mCameraParms.orthographic.left;
      if (authoredWidth > 0.f && top > bottom && PortDebug::HudWide()) {
        mSpread = renderAspect / (authoredWidth / (top - bottom));
        mSpreadCenterX = center;
      }
    }
    mCenterX = 0.5f * (left + right);
    mCenterZ = 0.5f * (top + bottom);
    CGraphics::SetOrtho(left, right, top, bottom, mCameraParms.orthographic.znear,
                        mCameraParms.orthographic.zfar);
  }

  mSpreadView =
      CTransform4f::Translate(parms.GetCameraOffset()) * GetWorldTransform();
  CGraphics::SetViewPointMatrix(mSpreadView);
  CGuiWidget::Draw(parms);
}

CTransform4f CGuiCamera::GetAspectSpreadTransform(const CVector3f& worldAnchor) const {
  if (mSpread == 1.f) {
    return CTransform4f::Identity();
  }
  const CTransform4f invView = mSpreadView.GetInverse();
  const CVector3f eyePos = invView * worldAnchor;
  if (!mSpreadAboutEye) {
    return CTransform4f::Translate(
        mSpreadView.Rotate(CVector3f((mSpread - 1.f) * (eyePos.GetX() - mSpreadCenterX), 0.f, 0.f)));
  }
  // Camera +Y is forward; +Z is screen-up. Keep the anchor's depth unchanged.
  if (eyePos.GetY() <= 0.f) {
    return CTransform4f::Identity();
  }
  const float yaw = std::atan2(eyePos.GetX(), eyePos.GetY());
  const float delta = std::atan2(mSpread * eyePos.GetX(), eyePos.GetY()) - yaw;
  const CVector3f offset((mSpread - 1.f) * eyePos.GetX(), 0.f, 0.f);
  return mSpreadView * CTransform4f::Translate(eyePos + offset) *
         CTransform4f::RotateZ(CRelAngle(-delta)) * CTransform4f::Translate(-eyePos) * invView;
}

CTransform4f CGuiCamera::GetHudScaleTransform(float scaleWeight) const {
  const float scale = 1.f + (mHudScale - 1.f) * scaleWeight;
  if (scale == 1.f) {
    return CTransform4f::Identity();
  }
  // Scale view X and Z about the view centre, keeping depth: a screen-space
  // scale for perspective and orthographic cameras alike. The whole frame
  // shrinks together, so widgets stay registered with the visor frame mesh.
  const CVector3f center(mCenterX, 0.f, mCenterZ);
  return mSpreadView * CTransform4f::Translate(center) * CTransform4f::Scale(scale, 1.f, scale) *
         CTransform4f::Translate(-center) * mSpreadView.GetInverse();
}

CTransform4f CGuiCamera::GetHudTransform(const CVector3f& worldAnchor, float scaleWeight) const {
  return GetHudScaleTransform(scaleWeight) * GetAspectSpreadTransform(worldAnchor);
}

CVector3f CGuiCamera::ConvertToScreenSpace(const CVector3f& point) const {
  CVector3f rotated = RotateTranslateW2O(point);

  if (rotated.IsNonZero()) {
    CMatrix4f xf = CGraphics::CalculatePerspectiveMatrix(
        mCameraParms.perspective.fov, mCameraParms.perspective.aspect,
        mCameraParms.perspective.znear, mCameraParms.perspective.zfar);

    return xf.MultiplyOneOverW(rotated);
  }

  return CVector3f(-1.f, -1.f, 1.f);
}
