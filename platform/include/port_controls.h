#pragma once

// Input rebinding: the port's keyboard defaults and the overlay's Controls tab.
// The binding storage, matching and persistence live in Aurora (dolphin/pad.h).

namespace PortControls {

// Binds the port's default keyboard layout (WASD move, IJKL look, X/Z/C/V face
// buttons) to the port and turns its keyboard on. Doesn't save.
void ApplyDefaultKeyBindings(unsigned port);
// True while the Controls tab is waiting for, or settling after, an input to
// bind; controller navigation of the overlay should ignore the pad then.
bool Capturing();
// True while a bound beam shift key or pad input is held (Controls tab;
// PortDebug::ShiftBinding stores them).
bool ShiftHeld();
// The PAD bits whose alt controller button (PortDebug::PadAltButton) is held.
unsigned HeldAltPadButtons();
// Draws the Controls tab of the debug overlay.
void DrawTab();

} // namespace PortControls
