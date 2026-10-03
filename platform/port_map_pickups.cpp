#include "port_map_pickups.h"

#include "port_ap_metroidprime.h"
#include "port_apclient.h"
#include "port_debug.h"
#include "port_skip_cutscenes.h"

namespace PortMapPickups {
namespace {

const Dot kDots[] = {
#include "port_map_pickups.inc"
};

} // namespace

bool Forced() { return PortSkipCutscenes::Forced(); }

bool Active() { return PortDebug::MapPickups() || Forced(); }

const Dot* Dots(size_t& count) {
  count = sizeof(kDots) / sizeof(kDots[0]);
  return kDots;
}

bool Colors(const unsigned char*& colors) {
  static const size_t kCount = sizeof(kDots) / sizeof(kDots[0]);
  static unsigned char sColors[kCount];
  // Which AP location each dot is: the one whose pickup sets the same relay.
  static int sLocation[kCount];
  static bool sMapped = false;
  if (!sMapped) {
    size_t count = 0;
    const PortAp::MetroidPrime::Location* locations = PortAp::MetroidPrime::Locations(count);
    for (size_t i = 0; i < kCount; ++i) {
      sLocation[i] = -1;
      for (size_t j = 0; j < count && sLocation[i] < 0; ++j) {
        if (locations[j].world == kDots[i].world && locations[j].relay == kDots[i].relay)
          sLocation[i] = static_cast< int >(j);
      }
    }
    sMapped = true;
  }
  static PortAp::LogicState sState;
  if (!PortDebug::MapLogicColors() || !PortAp::Logic(sState))
    return false;
  for (size_t i = 0; i < kCount; ++i) {
    const int location = sLocation[i];
    if (location < 0) {
      sColors[i] = kC_White;
    } else if (sState.checked[location]) {
      sColors[i] = kC_Grey;
    } else {
      switch (sState.levels[location]) {
      case PortApLogic::Level::Normal: sColors[i] = kC_Green; break;
      case PortApLogic::Level::SequenceBreak: sColors[i] = kC_Yellow; break;
      case PortApLogic::Level::Inspect: sColors[i] = kC_Blue; break;
      case PortApLogic::Level::None: sColors[i] = kC_Red; break;
      }
    }
  }
  colors = sColors;
  return true;
}

} // namespace PortMapPickups
