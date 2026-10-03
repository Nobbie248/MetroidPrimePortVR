#include "port_log_file.h"

#include "port_paths.h"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <system_error>

#if !defined(_WIN32)
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif
#if defined(__ANDROID__)
#include <android/log.h>
#include <thread>

#include <SDL3/SDL_log.h>
#include <SDL3/SDL_system.h>
#elif !defined(_WIN32)
#include <csignal>
#endif

namespace PortLogFile {
namespace {
bool sActive = false;

#if !defined(_WIN32)
void WriteAll(int fd, const char* data, ssize_t size) {
  while (size > 0) {
    const ssize_t n = write(fd, data, static_cast< size_t >(size));
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return;
    }
    data += n;
    size -= n;
  }
}
#endif

#if !defined(_WIN32) && !defined(__ANDROID__)
// The copying process: pipe -> terminal + file until every writer has gone. Only
// async-signal-safe calls, since the game may already have threads when it forks.
[[noreturn]] void Copy(int in, int terminal, int file, int maxFd) {
  for (int fd = 3; fd < maxFd; ++fd) {
    if (fd != in && fd != terminal && fd != file) {
      close(fd);
    }
  }
  // Ctrl+C in the terminal reaches this process too; it must keep draining until
  // the game has printed its last line.
  signal(SIGINT, SIG_IGN);
  signal(SIGQUIT, SIG_IGN);
  signal(SIGPIPE, SIG_IGN);
  char buffer[8192];
  for (;;) {
    const ssize_t n = read(in, buffer, sizeof(buffer));
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      break;
    }
    WriteAll(terminal, buffer, n);
    WriteAll(file, buffer, n);
  }
  _exit(0);
}
#endif

#if defined(__ANDROID__)
// The open log file. Every line is one write() of its own, so whatever was logged
// before a crash or a kill by Android is already in the file.
int sFile = -1;
SDL_LogOutputFunction sSdlDefault = nullptr;
void* sSdlDefaultData = nullptr;

void SdlLog(void* userdata, int category, SDL_LogPriority priority, const char* message) {
  if (sSdlDefault != nullptr) {
    sSdlDefault(sSdlDefaultData, category, priority, message);
  }
  Write("SDL", message);
}

// Nothing reads a process's stdout and stderr on Android, so printf output (the
// game's own reports among it) went nowhere. While the log runs both go through
// a pipe into logcat (tag "stdout") and the file.
void CopyStdio(int in) {
  char line[2048];
  size_t used = 0;
  char buffer[4096];
  for (;;) {
    const ssize_t n = read(in, buffer, sizeof(buffer));
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      return;
    }
    for (ssize_t i = 0; i < n; ++i) {
      const char c = buffer[i];
      if (c != '\n' && used + 1 < sizeof(line)) {
        line[used++] = c;
        continue;
      }
      if (c != '\n') {
        line[used++] = c;
      }
      line[used] = '\0';
      __android_log_write(ANDROID_LOG_INFO, "stdout", line);
      Write("stdout", line);
      used = 0;
    }
  }
}
#endif
} // namespace

std::string Path() {
#if defined(__ANDROID__)
  // The app's private folder is out of reach without root, so unless the data
  // was moved to shared storage the log goes to the app's external folder,
  // Android/data/org.metroidprime.port/files, which a USB file transfer shows.
  if (!PortPaths::IsPortable()) {
    const char* external = SDL_GetAndroidExternalStoragePath();
    if (external == nullptr || external[0] == '\0') {
      return {};
    }
    std::string folder = external;
    if (folder.back() != '/') {
      folder += '/';
    }
    return folder + "metroid_prime_port.log";
  }
#endif
  const std::string& folder = PortPaths::UserFolder();
  return folder.empty() ? std::string() : folder + "metroid_prime_port.log";
}

bool Active() { return sActive; }

void Write(const char* tag, const char* text) {
#if defined(__ANDROID__)
  if (sFile < 0 || text == nullptr) {
    return;
  }
  char line[2304];
  size_t size = static_cast< size_t >(std::snprintf(line, sizeof(line), "%s: %s", tag, text));
  if (size >= sizeof(line)) {
    size = sizeof(line) - 1;
  }
  if (size > 0 && line[size - 1] != '\n') {
    if (size + 1 >= sizeof(line)) {
      --size;
    }
    line[size++] = '\n';
  }
  WriteAll(sFile, line, static_cast< ssize_t >(size));
#else
  (void)tag;
  (void)text;
#endif
}

bool Start() {
  if (sActive) {
    return true;
  }
  const std::string path = Path();
  if (path.empty()) {
    return false;
  }
  std::error_code ec;
  const std::filesystem::path file(path);
  if (std::filesystem::exists(file, ec)) {
    std::filesystem::path old = file;
    old.replace_extension(".old.log");
    std::filesystem::rename(file, old, ec);
  }
  std::fflush(stdout);
  std::fflush(stderr);
  char started[64] = "";
  const std::time_t now = std::time(nullptr);
  if (const std::tm* local = std::localtime(&now)) {
    std::strftime(started, sizeof(started), "%Y-%m-%d %H:%M:%S", local);
  }
#if defined(_WIN32)
  // A GUI program may start without a console, so reopen the streams themselves
  // rather than their descriptors.
  if (_wfreopen(file.c_str(), L"w", stdout) == nullptr) {
    return false;
  }
  std::fprintf(stdout, "metroid_prime_port log, started %s\n", started);
  std::fflush(stdout);
  if (_wfreopen(file.c_str(), L"a", stderr) == nullptr) {
    return false;
  }
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::setvbuf(stderr, nullptr, _IONBF, 0);
#elif defined(__ANDROID__)
  const int out = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644);
  if (out < 0) {
    return false;
  }
  char header[128];
  const int headerSize = std::snprintf(header, sizeof(header), "metroid_prime_port log, started %s\n", started);
  WriteAll(out, header, headerSize);
  sFile = out;
  SDL_GetLogOutputFunction(&sSdlDefault, &sSdlDefaultData);
  SDL_SetLogOutputFunction(SdlLog, nullptr);
  int fds[2];
  if (pipe(fds) == 0) {
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    close(fds[1]);
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::thread(CopyStdio, fds[0]).detach();
  }
#else
  const int out = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (out < 0) {
    return false;
  }
  char header[128];
  const int headerSize = std::snprintf(header, sizeof(header), "metroid_prime_port log, started %s\n", started);
  WriteAll(out, header, headerSize);
  int fds[2];
  const int terminal = dup(STDERR_FILENO);
  if (terminal < 0 || pipe(fds) != 0) {
    close(out);
    if (terminal >= 0) {
      close(terminal);
    }
    return false;
  }
  long maxFd = sysconf(_SC_OPEN_MAX);
  maxFd = maxFd < 256 ? 256 : maxFd > 65536 ? 65536 : maxFd;
  const pid_t child = fork();
  if (child == 0) {
    close(fds[1]);
    Copy(fds[0], terminal, out, static_cast< int >(maxFd));
  }
  close(fds[0]);
  close(out);
  close(terminal);
  if (child < 0) {
    close(fds[1]);
    return false;
  }
  dup2(fds[1], STDOUT_FILENO);
  dup2(fds[1], STDERR_FILENO);
  close(fds[1]);
  // stdout to a pipe is fully buffered, and abort() flushes nothing: one line at a
  // time keeps the last messages before a crash.
  std::setvbuf(stdout, nullptr, _IOLBF, 0);
#endif
  sActive = true;
  std::fprintf(stderr, "port: writing the log to %s\n", path.c_str());
  return true;
}

} // namespace PortLogFile
