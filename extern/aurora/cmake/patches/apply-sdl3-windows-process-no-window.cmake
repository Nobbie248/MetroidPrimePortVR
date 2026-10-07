if (NOT DEFINED SDL_SOURCE_DIR)
  message(FATAL_ERROR "SDL_SOURCE_DIR is required")
endif ()

# A GUI program that starts a console program gets a new console window for it
# unless CREATE_NO_WINDOW is passed. SDL only passes it for background
# processes, which also lose their exit code, so this adds a property for it.
set(_process "${SDL_SOURCE_DIR}/src/process/windows/SDL_windowsprocess.c")
if (NOT EXISTS "${_process}")
  message(FATAL_ERROR "SDL Windows process source is missing: ${_process}")
endif ()

file(READ "${_process}" _source)
set(_old "    creation_flags = CREATE_UNICODE_ENVIRONMENT;\n")
set(_new "${_old}    if (SDL_GetBooleanProperty(props, \"SDL.process.create.windows.no_window\", false)) {\n        creation_flags |= CREATE_NO_WINDOW;\n    }\n")

string(FIND "${_source}" "SDL.process.create.windows.no_window" _already_patched)
if (NOT _already_patched EQUAL -1)
  message(STATUS "SDL3 Windows process no-window patch already applied")
  return()
endif ()

string(FIND "${_source}" "${_old}" _patch_site)
if (_patch_site EQUAL -1)
  message(FATAL_ERROR "Failed to apply SDL3 Windows process no-window patch: expected creation flags not found")
endif ()

string(REPLACE "${_old}" "${_new}" _patched_source "${_source}")
file(WRITE "${_process}" "${_patched_source}")
message(STATUS "Applied SDL3 Windows process no-window patch")
