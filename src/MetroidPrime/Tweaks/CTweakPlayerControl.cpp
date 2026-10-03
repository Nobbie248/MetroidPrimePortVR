#include "MetroidPrime/Tweaks/CTweakPlayerControl.hpp"

#include "Kyoto/Streams/CInputStream.hpp"

#ifdef TARGET_PC
#include "port_debug.h"
#endif

CTweakPlayerControl::~CTweakPlayerControl() {}

rstl::reserved_vector< ControlMapper::EFunctionList, 67 > LoadMappings(CInputStream& in) {
  rstl::reserved_vector< ControlMapper::EFunctionList, 67 > result;
  for (int i = 0; i < result.capacity(); ++i) {
    result.push_back(static_cast< ControlMapper::EFunctionList >(in.ReadLong()));
  }
  return result;
}

CTweakPlayerControl::CTweakPlayerControl(CInputStream& in) : m_mappings(LoadMappings(in)) {}

ControlMapper::EFunctionList
CTweakPlayerControl::GetMapping(ControlMapper::ECommands command) const {
  if (command < ControlMapper::kC_Forward || command > ControlMapper::kC_UNKNOWN - 1)
    return m_mappings[0];

#ifdef TARGET_PC
  // Remastered's Dual Sticks layout has the Scan and X-Ray visors on each
  // other's D-pad directions.
  if (PortDebug::SwapScanXray()) {
    if (command == ControlMapper::kC_EnviroVisor) {
      return m_mappings[ControlMapper::kC_XrayVisor];
    }
    if (command == ControlMapper::kC_XrayVisor) {
      return m_mappings[ControlMapper::kC_EnviroVisor];
    }
  }
#endif
  return m_mappings[command];
}
