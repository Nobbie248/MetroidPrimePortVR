#pragma once

#include "Kyoto/SObjectTag.hpp"
#include "MetroidPrime/TGameTypes.hpp"

class CStateManager;
class CPlayerGun;
class CTransform4f;

// Linked only with MP_ENABLE_SMOKE_DRIVER. Never grabs the user's real pointer.
bool PortSmokeMouseEnabled();
bool PortSmokeScriptedInput();
void PortSmokeAreaReload(CStateManager& mgr);
void PortSmokeWorldTeleport(CStateManager& mgr);
void PortSmokeElevatorLoaded(TUniqueId uid, CAssetId worldId, CAssetId areaId);
void PortSmokeElevator(CStateManager& mgr);
void PortSmokeVisor(CStateManager& mgr);
void PortSmokeSave(CStateManager& mgr);
void PortSmokeSaveScreenUI(int saveCtx, int oldUiType, int uiType, int driverState);
void PortSmokeScript(unsigned frame);
bool PortSmokeContinueEnabled();
bool PortSmokePressPending();
void PortSmokePress(unsigned buttons, unsigned hold);
void PortSmokeShotAfter(unsigned frames, const char* why);
unsigned PortSmokeCurrentFrame();
void PortSmokeCurrentFrameSet(unsigned frame);
void PortSmokeStick(CStateManager& mgr);
void PortSmokeWalk(CStateManager& mgr);
void PortSmokeDash(CStateManager& mgr);
void PortSmokeWater(CStateManager& mgr);
unsigned PortSmokeMouseButtons(unsigned realButtons);
void PortSmokeMouseBeforeUpdate(CStateManager& mgr);
void PortSmokeMouseAfterUpdate(CStateManager& mgr);
void PortSmokeMouseShot(bool charged, bool secondary);
void PortSmokeMouseGunView(const CStateManager& mgr, const CPlayerGun& gun,
                          const CTransform4f& worldView);
