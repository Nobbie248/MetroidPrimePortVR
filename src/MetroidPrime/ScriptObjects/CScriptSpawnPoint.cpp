#include "MetroidPrime/ScriptObjects/CScriptSpawnPoint.hpp"

#include "MetroidPrime/CGameArea.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/CWorld.hpp"
#include "MetroidPrime/Player/CPlayer.hpp"

#ifdef TARGET_PC
#include "dolphin/ar.h"
#include "port_apclient.h"
#endif

CScriptSpawnPoint::CScriptSpawnPoint(
    TUniqueId uid, const rstl::string& name, const CEntityInfo& info, const CTransform4f& xf,
    const rstl::reserved_vector< int, int(CPlayerState::kIT_Max) >& itemCounts,
    const bool defaultSpawn, const bool active, const bool morphed)
: CEntity(uid, info, active, name)
, x34_xf(xf)
, x64_itemCounts(itemCounts)
, x10c_24_firstSpawn(defaultSpawn)
, x10c_25_morphed(morphed) {}

CScriptSpawnPoint::~CScriptSpawnPoint() {}

const CTransform4f& CScriptSpawnPoint::GetTransform() const { return x34_xf; }

int CScriptSpawnPoint::GetPowerup(const CPlayerState::EItemType& type) const {
#ifdef TARGET_PC
  // Port: an Archipelago seed hands out the starting items itself (randomprime
  // rewrites the start room's spawn points the same way).
  if (PortAp::SeedGivesStartItems()) {
    return type == CPlayerState::kIT_CombatVisor || type == CPlayerState::kIT_PowerSuit ? 1 : 0;
  }
#endif
  if (CPlayerState::kIT_Max <= type || type < 0) {
    return x64_itemCounts.front();
  }
  return x64_itemCounts[type];
}

void CScriptSpawnPoint::AcceptScriptMsg(EScriptObjectMessage msg, TUniqueId objId,
                                        CStateManager& stateMgr) {
  CEntity::AcceptScriptMsg(msg, objId, stateMgr);

  switch (msg) {
  case kSM_Reset:
    for (int i = 0; i < CPlayerState::kIT_Max; ++i) {
      const CPlayerState::EItemType e = static_cast< CPlayerState::EItemType >(i);
      stateMgr.PlayerState()->SetPowerUp(e, GetPowerup(e));
      stateMgr.PlayerState()->SetPickup(e, GetPowerup(e));
    }
#ifdef TARGET_PC
    // Port: Archipelago items live in the inventory this just replaced.
    PortAp::OnInventoryReset();
#endif
  case kSM_SetToZero:
    if (GetActive()) {
      CPlayer* player = stateMgr.Player();
      TAreaId thisAreaId = GetCurrentAreaId();
      TAreaId nextAreaId = stateMgr.GetNextAreaId();

      if (nextAreaId != thisAreaId) {
        bool propagateAgain = false;

        CGameArea* area = stateMgr.World()->Area(thisAreaId);
        if (area->GetOcclusionState() == CGameArea::kOS_Occluded) {
          while (!area->TryTakingOutOfARAM()) {
#ifdef TARGET_PC
            // Port: the ARAM DMA completes by interrupt on the console. Aurora
            // queues the completion until ARQPoll runs, so spinning without
            // it never ends when the occluded area's data is still in ARAM.
            ARQPoll();
#endif
          }
          CWorld::PropogateAreaChain(CGameArea::kOS_Visible, area, stateMgr.World());
          propagateAgain = true;
        }

        stateMgr.SetCurrentAreaId(thisAreaId);
        stateMgr.SetActorAreaId(*player, thisAreaId);
        player->Teleport(x34_xf, stateMgr, true);
        player->SetSpawnedMorphBallState(
            x10c_25_morphed ? CPlayer::kMS_Morphed : CPlayer::kMS_Unmorphed, stateMgr);

        if (propagateAgain) {
          CWorld::PropogateAreaChain(CGameArea::kOS_Occluded, stateMgr.World()->Area(nextAreaId),
                                     stateMgr.World());
        }

      } else {
        player->Teleport(x34_xf, stateMgr, true);
        player->SetSpawnedMorphBallState(
            x10c_25_morphed ? CPlayer::kMS_Morphed : CPlayer::kMS_Unmorphed, stateMgr);
      }
      CEntity::SendScriptMsgs(kSS_Zero, stateMgr, kSM_None);
    }
  default:
    break;
  }
}

ENTITY_ACCEPT_IMPL(CScriptSpawnPoint)
