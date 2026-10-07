#ifndef METROID_PRIME_PORT_PORT_WATCHDOG_H
#define METROID_PRIME_PORT_PORT_WATCHDOG_H

// Makes a silent hang visible in the log (metroid_prime_port.log). The game loop calls
// Heartbeat once per frame; a thread of its own watches it and, when no frame has run
// for 5 s (then 15, 30 and 60 s), writes "watchdog: no frame for ..." with the phase
// the main and render threads last reported (aurora/phase.hpp), the main thread's
// stack (Linux and Android) and, on Android, the app's recent logcat lines. A line
// says when frames resume. Time while the app is backgrounded does not count.
//
// MP_WATCHDOG=0 turns it off. MP_WATCHDOG_TEST_STALL=<frame> sleeps the main thread
// for 8 s at that frame, to check the report. MP_WATCHDOG_TEST_THROW=<frame> throws
// out of the main loop at that frame, to check the uncaught-exception report.
namespace PortWatchdog {

// Once per main-loop iteration, on the main thread. The first call starts the watchdog,
// so nothing is watched before the loop (the disc dialog, loading the data).
void Heartbeat(unsigned frame);

// Android: appends the app's own recent logcat lines to the log as "logcat:" lines,
// leaving out what already reaches the log through our own tags, so the drivers'
// messages stand out. warningsOnly keeps warnings and above. Elsewhere a no-op.
void LogcatDump(const char* why, bool warningsOnly);

} // namespace PortWatchdog

#endif // METROID_PRIME_PORT_PORT_WATCHDOG_H
