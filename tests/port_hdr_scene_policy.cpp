#include "MetroidPrime/CStateManager.hpp"

int main() {
  if (!CStateManager::PortIsSceneHdrVisorSupported(true, false, false, false)) // Combat visor
    return 1;
  if (!CStateManager::PortIsSceneHdrVisorSupported(false, true, false, false)) // Scan visor
    return 2;
  if (CStateManager::PortIsSceneHdrVisorSupported(false, false, true, false)) // Thermal visor
    return 3;
  if (CStateManager::PortIsSceneHdrVisorSupported(false, false, false, true)) // X-ray visor
    return 4;

  if (!CStateManager::PortShouldUseSceneHdr(true, true, true, true, false)) // enabled with Remastered tone
    return 5;
  if (CStateManager::PortShouldUseSceneHdr(false, true, true, true, false)) // MP_HDR_SCENE unset / 0
    return 6;
  if (CStateManager::PortShouldUseSceneHdr(true, false, false, true, false)) // room env mod disabled
    return 7;
  if (CStateManager::PortShouldUseSceneHdr(true, true, false, true, false)) // tone absent
    return 8;
  if (CStateManager::PortShouldUseSceneHdr(true, true, true, true, true)) // any visor transition
    return 9;
}
