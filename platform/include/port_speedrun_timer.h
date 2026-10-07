#pragma once

#include <cstddef>

// The in-game time shown in the bottom-right corner (PortDebug::SpeedrunTimer).
namespace PortSpeedrunTimer {
// h:mm:ss.cc from an hour on, m:ss.cc before.
void FormatTime(double seconds, char* out, size_t size);
// Draws it with the game's font; call between BeginScene and EndScene.
void Draw();
} // namespace PortSpeedrunTimer
