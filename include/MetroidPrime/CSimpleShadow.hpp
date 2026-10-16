#ifndef _CSIMPLESHADOW
#define _CSIMPLESHADOW

#include "types.h"

#include "Kyoto/Math/CTransform4f.hpp"
#include "Kyoto/TToken.hpp"
#ifdef TARGET_PC
#include "port_tick_pair.h"
#endif

class CAABox;
class CStateManager;
class CTexture;

class CSimpleShadow {
public:
  CSimpleShadow(float scale, float userAlpha, float maxObjHeight, float displacement);

  void Calculate(const CAABox& bounds, const CTransform4f& xf, const CStateManager& mgr);
  void Render(const CTexture* tex) const;
  const CTransform4f& GetTransform() const;
  void SetUserAlpha(float);
  float GetMaxObjectHeight() const;

  void SetAlwaysCalculateRadius(bool);
  CAABox GetBounds() const;
  CAABox GetMaxShadowBox(const CAABox& bounds) const;
  bool Valid() const;
#ifdef TARGET_PC
  // Remastered's projected shadow (port_actor_shadow.h): the actor's world bounds, one anchor ray
  // per update, a receiver box. False when no anchor has hit a floor.
  void PortCalculate(const CAABox& worldBounds, const CStateManager& mgr);
  bool PortValid() const { return xPortValid; }
  // Sampled mode (anchor rays; fade 0) or, for AI actors, explicit height mode (one downward cast
  // from the centre, fade 0.625; the user alpha is the whole opacity).
  void PortSetMode(bool explicitHeight, float fadeStart, float userAlpha) {
    xPortExplicit = explicitHeight;
    xPortFadeStart = fadeStart;
    x38_userAlpha = userAlpha;
  }
  // An anchor ray hit a platform: dynamic receivers (filter 0x40) are redrawn too.
  bool PortDynamic() const { return xPortDynamicBits != 0; }
  float PortOpacity() const { return xPortOpacity; }
  float PortFadeStart() const { return xPortFadeStart; }
  // The caster map's box (the actor's bounds) and the receiver box, as centre and half extent.
  const float* PortMapBox() const { return xPortMap; }
  const float* PortReceiverBox() const { return xPortReceiver; }
#endif

private:
  CTransform4f x0_xf;
  float x30_scale;
  float x34_radius;
  float x38_userAlpha;
  float x3c_heightAlpha;
  float x40_maxObjHeight;
  float x44_displacement;
  bool x48_24_collision : 1;
  bool x48_25_alwaysCalculateRadius : 1;
  bool x48_26_radiusCalculated : 1;
#ifdef TARGET_PC
  // The blob placed at the last two ticks, so it follows its presented actor.
  mutable PortTickPair< CTransform4f > xPortXf{CTransform4f::Identity()};
  bool xPortValid = false;
  unsigned xPortUpdate = 0;
  float xPortHit[5] = {};     // floor height under each anchor
  bool xPortHasHit[5] = {};
  float xPortBounds[6] = {};  // the bounds the shadow was last built for
  float xPortOpacity = 0.f;
  float xPortFadeStart = 0.f;
  bool xPortExplicit = false;
  unsigned xPortDynamicBits = 0;
  float xPortMap[6] = {};      // centre xyz, half xyz
  float xPortReceiver[6] = {}; // centre xyz, half xyz
#endif
};
CHECK_SIZEOF(CSimpleShadow, 0x4c)

#endif // _CSIMPLESHADOW
