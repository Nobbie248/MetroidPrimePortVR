#ifndef METROID_PRIME_PORT_CRASH_HANDLER_H
#define METROID_PRIME_PORT_CRASH_HANDLER_H

// On Windows, installs an unhandled-exception filter that logs the exception,
// a symbolised stack of the faulting thread (from the build's PDB when it is
// beside the executable) and writes a minidump next to the executable, before
// letting Windows Error Reporting proceed. Elsewhere it does nothing.
void PortInstallCrashHandler();

#endif // METROID_PRIME_PORT_CRASH_HANDLER_H
