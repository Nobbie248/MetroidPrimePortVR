#ifndef METROID_PRIME_PORT_PORT_LOG_FILE_H
#define METROID_PRIME_PORT_PORT_LOG_FILE_H
#include <string>

// The "write the log to a file" setting (F1 > Debug > Log, MP_LOG_FILE). Everything
// the process prints to stdout and stderr - the port's messages, Aurora's, SDL's,
// and the FATAL line Aurora prints right before it aborts - is also written to
// <user folder>/metroid_prime_port.log, so a crash on a machine without a
// terminal leaves a log to send. The previous run's log is kept as
// metroid_prime_port.old.log, so restarting after a crash does not lose it.
//
// On Linux a small forked process copies a pipe to both the terminal and the file:
// it outlives an abort and drains everything already written. On Windows stdout
// and stderr go to the file only. On Android the logcat writers (PortLog, Aurora's
// callback, SDL) also call Write, each line straight to the file, and stdout and
// stderr are copied into logcat and the file; the log goes to the app's external
// folder (Android/data/org.metroidprime.port/files) unless the data was moved to
// shared storage.
namespace PortLogFile {

// Starts the file log (once per run); true when it is running.
bool Start();
bool Active();
// Android: appends "<tag>: <text>" as one line while the log runs. Elsewhere a
// no-op, since stdout and stderr already reach the file.
void Write(const char* tag, const char* text);
// The log's path, empty without a user folder.
std::string Path();

} // namespace PortLogFile

#endif // METROID_PRIME_PORT_PORT_LOG_FILE_H
