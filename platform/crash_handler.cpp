#include "crash_handler.h"

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <dbghelp.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "port_log.h"

namespace {

const char* AccessKind(ULONG_PTR kind) {
  switch (kind) {
  case 0:
    return "read of";
  case 1:
    return "write to";
  case 8:
    return "execute at";
  default:
    return "access to";
  }
}

// Walks the faulting thread's stack with dbghelp. Line numbers come from the
// PDB when it sits next to the executable (RelWithDebInfo builds).
void LogStack(HANDLE process, const CONTEXT& faultingContext) {
  SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_FAIL_CRITICAL_ERRORS);
  if (!SymInitialize(process, nullptr, TRUE)) {
    PortLog::Write("  (no symbols: SymInitialize failed, error %lu)\n", GetLastError());
    return;
  }
  CONTEXT context = faultingContext;
  STACKFRAME64 frame{};
  frame.AddrPC.Offset = context.Rip;
  frame.AddrPC.Mode = AddrModeFlat;
  frame.AddrFrame.Offset = context.Rbp;
  frame.AddrFrame.Mode = AddrModeFlat;
  frame.AddrStack.Offset = context.Rsp;
  frame.AddrStack.Mode = AddrModeFlat;
  for (int i = 0; i < 48; ++i) {
    if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, &context, nullptr,
                     SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) {
      break;
    }
    if (frame.AddrPC.Offset == 0) {
      break;
    }
    alignas(SYMBOL_INFO) char symbolBuffer[sizeof(SYMBOL_INFO) + 512] = {};
    SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolBuffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 511;
    DWORD64 displacement = 0;
    const char* name = SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol) ? symbol->Name : "?";
    IMAGEHLP_LINE64 line{};
    line.SizeOfStruct = sizeof(line);
    DWORD lineDisplacement = 0;
    const bool hasLine = SymGetLineFromAddr64(process, frame.AddrPC.Offset, &lineDisplacement, &line) != FALSE;
    char module[MAX_PATH] = "?";
    const DWORD64 base = SymGetModuleBase64(process, frame.AddrPC.Offset);
    if (base != 0) {
      GetModuleFileNameA(reinterpret_cast<HMODULE>(base), module, sizeof(module));
      const char* slash = std::strrchr(module, '\\');
      if (slash != nullptr) {
        std::memmove(module, slash + 1, std::strlen(slash + 1) + 1);
      }
    }
    if (hasLine) {
      PortLog::Write("  #%02d %s!%s+0x%llx (%s:%lu)\n", i, module, name, static_cast<unsigned long long>(displacement),
                     line.FileName, static_cast<unsigned long>(line.LineNumber));
    } else {
      PortLog::Write("  #%02d %s!%s+0x%llx\n", i, module, name, static_cast<unsigned long long>(displacement));
    }
  }
  SymCleanup(process);
}

void WriteMinidump(HANDLE process, EXCEPTION_POINTERS* info) {
  char path[MAX_PATH] = {};
  if (GetModuleFileNameA(nullptr, path, sizeof(path)) == 0) {
    return;
  }
  char* slash = std::strrchr(path, '\\');
  if (slash == nullptr) {
    return;
  }
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_s(&local, &now);
  char name[64];
  std::strftime(name, sizeof(name), "crash_%Y%m%d_%H%M%S.dmp", &local);
  const size_t prefix = static_cast<size_t>(slash + 1 - path);
  if (prefix + std::strlen(name) + 1 > sizeof(path)) {
    return;
  }
  std::strcpy(slash + 1, name);
  HANDLE file = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    PortLog::Write("  (no minidump: cannot create %s, error %lu)\n", path, GetLastError());
    return;
  }
  MINIDUMP_EXCEPTION_INFORMATION exception{};
  exception.ThreadId = GetCurrentThreadId();
  exception.ExceptionPointers = info;
  exception.ClientPointers = FALSE;
  // MP_CRASH_FULL_DUMP=1 captures the whole address space (large, but every
  // heap object is readable afterwards).
  const char* full = std::getenv("MP_CRASH_FULL_DUMP");
  const auto type = full != nullptr && *full != '\0' && *full != '0'
                        ? static_cast<MINIDUMP_TYPE>(MiniDumpWithFullMemory | MiniDumpWithThreadInfo |
                                                     MiniDumpWithUnloadedModules)
                        : static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithDataSegs |
                                                     MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
  const BOOL written = MiniDumpWriteDump(process, GetCurrentProcessId(), file, type, &exception, nullptr, nullptr);
  CloseHandle(file);
  if (written) {
    PortLog::Write("  minidump written to %s\n", path);
  } else {
    PortLog::Write("  (minidump failed, error %lu)\n", GetLastError());
  }
}

LONG WINAPI OnUnhandledException(EXCEPTION_POINTERS* info) {
  static LONG entered = 0;
  if (InterlockedExchange(&entered, 1) != 0 || info == nullptr || info->ExceptionRecord == nullptr) {
    return EXCEPTION_CONTINUE_SEARCH;
  }
  const EXCEPTION_RECORD& record = *info->ExceptionRecord;
  PortLog::Write("metroid_prime_port: fatal exception 0x%08lx at %p on thread %lu\n",
                 static_cast<unsigned long>(record.ExceptionCode), record.ExceptionAddress, GetCurrentThreadId());
  if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2) {
    PortLog::Write("  %s address %p\n", AccessKind(record.ExceptionInformation[0]),
                   reinterpret_cast<void*>(record.ExceptionInformation[1]));
  }
  HANDLE process = GetCurrentProcess();
  if (info->ContextRecord != nullptr) {
    LogStack(process, *info->ContextRecord);
  }
  WriteMinidump(process, info);
  std::fflush(stderr);
  return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

void PortInstallCrashHandler() { SetUnhandledExceptionFilter(OnUnhandledException); }

#else

void PortInstallCrashHandler() {}

#endif
