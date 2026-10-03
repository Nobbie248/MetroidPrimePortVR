#pragma once

class CStateManager;

// The debug command console (platform/port_console.cpp, MP_CONSOLE=<port>).
// In every build; it does nothing unless MP_CONSOLE is set.
bool PortConsoleEnabled();
bool PortConsoleFrame(unsigned frame);
void PortConsoleTick(CStateManager& mgr);
