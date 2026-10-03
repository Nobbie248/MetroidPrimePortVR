// Port entry point. Aurora owns the real process entry (aurora_main) and the
// window/GPU/input/audio backend; this file initializes it, mounts the user's
// disc, and hands control to the game.
//
// Disc path resolution: first non-flag argument, then $MP_DISC.

#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <aurora/gfx.h>
#include <aurora/event.h>
#include <aurora/main.h>
#include <dolphin/gx.h>
#include <dolphin/vi.h>
#include <dolphin/dvd.h>

#include "port_debug.h"
#include "port_apclient.h"
#include "port_randomizer.h"
#include "port_textures.h"
#include "port_prompts.h"
#include "port_build_info.h"
#include "crash_handler.h"
#include "port_log.h"
#include "port_mods.h"
#include "port_room_geo.h"
#include "port_importers.h"
#include "port_remastered_import.h"

#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_timer.h>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

extern "C" int metroid_main(int argc, char** argv);
extern "C" void AIPortShutdown(void);

// PortVr: the OpenXR host layer (platform/vr).
#include "vr/openxr_integration.h"
#include "vr/vr_settings.h"

namespace {
#if defined(__ANDROID__)
// Aurora logs to stderr, which Android discards. Send it to logcat instead so
// the Vulkan/audio/disc diagnostics are actually reachable on a device.
void AndroidLogCallback(AuroraLogLevel level, const char* module, const char* message,
                        unsigned int len) {
    int priority = ANDROID_LOG_INFO;
    switch (level) {
    case LOG_DEBUG:
        priority = ANDROID_LOG_DEBUG;
        break;
    case LOG_WARNING:
        priority = ANDROID_LOG_WARN;
        break;
    case LOG_ERROR:
        priority = ANDROID_LOG_ERROR;
        break;
    case LOG_FATAL:
        priority = ANDROID_LOG_FATAL;
        break;
    case LOG_INFO:
    default:
        break;
    }
    __android_log_print(priority, "aurora", "[%s] %.*s", module != nullptr ? module : "",
                        static_cast< int >(len), message);
}
#endif

std::string LowerExtension(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    for (char& c : ext) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return ext;
}

bool IsDiscImage(const std::filesystem::path& path) {
    static const char* const kExtensions[] = {".iso", ".gcm", ".rvz", ".wbfs", ".ciso", ".nkit"};
    const std::string ext = LowerExtension(path);
    for (const char* candidate : kExtensions) {
        if (ext == candidate) {
            return true;
        }
    }
    return false;
}

// Looks for a disc image next to the executable (and in its immediate
// subdirectories) so a copied build is self-contained.
std::string FindDiscNextToExecutable() {
    const char* base = SDL_GetBasePath();
    if (base == nullptr) {
        return {};
    }
    namespace fs = std::filesystem;
    // The iterator and the per-entry checks keep separate error codes: an entry
    // whose status cannot be read (a symlink into a directory without access)
    // fails its own check and is skipped, where a shared code would end the
    // whole search before a good image further on.
    // Path conversions can still throw (a name the narrow encoding cannot hold),
    // and this runs before anything that could report it, so nothing escapes.
    try {
        std::error_code ec;
        const fs::path baseDir(base);
        std::vector<fs::path> dirs{baseDir};
        for (fs::directory_iterator it(baseDir, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code entryEc;
            if (it->is_directory(entryEc)) {
                dirs.push_back(it->path());
            }
        }
        for (const fs::path& dir : dirs) {
            for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
                std::error_code entryEc;
                if (it->is_regular_file(entryEc) && IsDiscImage(it->path())) {
                    return it->path().string();
                }
            }
        }
    } catch (const std::exception& e) {
        PortLog::Write("metroid_prime_port: searching for a disc image failed: %s\n", e.what());
    }
    return {};
}

const char* ResolveDiscPath(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (argv[i][0] != '-' && argv[i][0] != '\0') {
            return argv[i];
        }
    }
    if (const char* env = std::getenv("MP_DISC"); env != nullptr && env[0] != '\0') {
        return env;
    }
    if (const char* saved = PortDebug::DiscPath(); saved != nullptr) {
#if defined(__ANDROID__)
        if (std::strncmp(saved, "content://", 10) == 0) {
            // A URI from before the copy existed. Prefer the local copy, which
            // needs no permission grant; fall back to the URI if it is not there
            // yet, since the grant may still be live.
            char* pref = SDL_GetPrefPath(nullptr, "Metroid Prime");
            if (pref != nullptr) {
                const std::string local = (std::filesystem::path(pref) / "disc.iso").string();
                SDL_free(pref);
                std::error_code ec;
                static const std::string sLocal =
                    std::filesystem::exists(local, ec) ? local : std::string();
                if (!sLocal.empty()) {
                    return sLocal.c_str();
                }
            }
            return saved;
        }
#endif
        // The error_code overload: the throwing one would end the program on a
        // remembered path that is merely unreadable, instead of moving on.
        std::error_code ec;
        if (std::filesystem::exists(saved, ec)) {
            return saved;
        }
    }
    static const std::string sFound = FindDiscNextToExecutable();
    return sFound.empty() ? nullptr : sFound.c_str();
}

// aurora_dvd_open reports failure for three different reasons - the file would
// not open, the disc parser rejected it, or the data partition was missing -
// and says which of them nowhere. Splitting them here turns an unexplained
// exit into a specific, actionable line.
void ReportDiscOpenFailure(const char* path) {
    PortLog::Write( "metroid_prime_port: failed to open disc image: %s\n", path);
    SDL_ClearError();
    SDL_IOStream* probe = SDL_IOFromFile(path, "rb");
    if (probe == nullptr) {
        PortLog::Write( "  the file itself could not be opened: %s\n", SDL_GetError());
        return;
    }
    const Sint64 size = SDL_GetIOSize(probe);
    Uint8 header[8] = {};
    const size_t got = SDL_ReadIO(probe, header, sizeof(header));
    SDL_CloseIO(probe);
    if (got != sizeof(header)) {
        PortLog::Write( "  the file opened but is only %lld bytes: too short to be a disc\n",
                        static_cast<long long>(size));
        return;
    }
    PortLog::Write( "  the file opened and is %lld bytes, so the disc parser rejected it\n",
                    static_cast<long long>(size));
    PortLog::Write( "  first bytes: %02x %02x %02x %02x %02x %02x %02x %02x\n", header[0], header[1],
                    header[2], header[3], header[4], header[5], header[6], header[7]);
}

#if defined(__ANDROID__)
// Android's picker hands back a content:// URI, not a path. Opening one is
// possible (SDL routes SDL_IOFromFile through ContentResolver) but it depends
// on a permission grant that the system can revoke at any time - and on some
// builds the open fails even on the launch that picked the file. Copying the
// image into app storage once makes the remembered setting a plain file, which
// is readable with no grant at all and survives anything short of an uninstall.
//
// Returns the local copy's path, or an empty string if the copy failed.
std::string CopyDiscFromContentUri(const std::string& uri) {
    char* pref = SDL_GetPrefPath(nullptr, "Metroid Prime");
    if (pref == nullptr) {
        PortLog::Write( "metroid_prime_port: no pref path to copy the disc into\n");
        return {};
    }
    const std::filesystem::path target = std::filesystem::path(pref) / "disc.iso";
    // Copied under another name and renamed when complete: a copy killed part
    // way (the app closed during a multi-minute copy) must not leave a
    // truncated disc.iso, which ResolveDiscPath would prefer on every launch.
    const std::filesystem::path partial = std::filesystem::path(pref) / "disc.iso.part";
    SDL_free(pref);

    SDL_IOStream* in = SDL_IOFromFile(uri.c_str(), "rb");
    if (in == nullptr) {
        PortLog::Write( "metroid_prime_port: could not read the picked image: %s: %s\n", uri.c_str(),
                        SDL_GetError());
        return {};
    }
    const Sint64 total = SDL_GetIOSize(in);
    SDL_IOStream* out = SDL_IOFromFile(partial.string().c_str(), "wb");
    if (out == nullptr) {
        PortLog::Write( "metroid_prime_port: could not create %s: %s\n", partial.string().c_str(),
                        SDL_GetError());
        SDL_CloseIO(in);
        return {};
    }

    char buffer[1 << 16];
    Sint64 done = 0;
    int lastPercent = -1;
    bool ok = true;
    for (;;) {
        const size_t got = SDL_ReadIO(in, buffer, sizeof(buffer));
        if (got == 0) {
            // Zero is also what a failed read returns; only the stream status
            // tells end of file from an error that would leave a truncated copy.
            if (SDL_GetIOStatus(in) != SDL_IO_STATUS_EOF) {
                PortLog::Write( "metroid_prime_port: reading the picked image failed: %s\n",
                                SDL_GetError());
                ok = false;
            }
            break;
        }
        if (SDL_WriteIO(out, buffer, got) != got) {
            PortLog::Write( "metroid_prime_port: writing %s failed: %s\n", partial.string().c_str(),
                            SDL_GetError());
            ok = false;
            break;
        }
        done += static_cast<Sint64>(got);
        if (total > 0) {
            const int percent = static_cast<int>(done * 100 / total);
            // Every 5% rather than every chunk: a 1.5 GB image would otherwise
            // put 3000 lines in the log.
            if (percent / 5 != lastPercent / 5) {
                lastPercent = percent;
                PortLog::Write( "metroid_prime_port: copying the disc image, %d%%\n", percent);
            }
        }
    }
    if (!SDL_FlushIO(out)) {
        PortLog::Write( "metroid_prime_port: flushing %s failed: %s\n", partial.string().c_str(),
                        SDL_GetError());
        ok = false;
    }
    if (!SDL_CloseIO(in)) {
        PortLog::Write( "metroid_prime_port: closing the picked image failed: %s\n", SDL_GetError());
    }
    if (!SDL_CloseIO(out)) {
        PortLog::Write( "metroid_prime_port: closing %s failed: %s\n", partial.string().c_str(),
                        SDL_GetError());
        ok = false;
    }
    if (!ok) {
        std::error_code ec;
        std::filesystem::remove(partial, ec);
        return {};
    }
    // What landed on disk, not what was handed to the writer: a short write that
    // only fails at close looks identical to a good copy otherwise, and a
    // truncated image fails to parse as a disc with no further clue.
    std::error_code ec;
    const auto written = std::filesystem::file_size(partial, ec);
    if (ec || static_cast<Sint64>(written) != done || (total > 0 && done != total)) {
        PortLog::Write( "metroid_prime_port: %s is %lld bytes on disk, copied %lld of %lld\n",
                        partial.string().c_str(), static_cast<long long>(written),
                        static_cast<long long>(done), static_cast<long long>(total));
        std::filesystem::remove(partial, ec);
        return {};
    }
    std::filesystem::rename(partial, target, ec);
    if (ec) {
        PortLog::Write( "metroid_prime_port: could not rename %s to %s: %s\n",
                        partial.string().c_str(), target.string().c_str(), ec.message().c_str());
        std::filesystem::remove(partial, ec);
        return {};
    }
    PortLog::Write( "metroid_prime_port: copied the disc image to %s (%lld bytes)\n",
                    target.string().c_str(), static_cast<long long>(done));
    return target.string();
}
#endif  // __ANDROID__

// Asks for the disc image with the platform's file dialog and remembers the
// choice. SDL delivers the result on another thread, so this pumps events until
// it arrives; the callback also fires with an empty list if the dialog fails.
std::string AskForDiscImage() {
    static std::atomic< bool > answered{false};
    static std::string chosen;
    // Static because the callback cannot capture, but reset on every call: the
    // stale-disc retry asks a second time, and without this it would return the
    // first answer at once without showing a dialog. A first call only returns
    // early (timeout, quit, no window) on the way to exiting, so no callback
    // from it can still be pending here.
    answered.store(false);
    chosen.clear();
    const SDL_DialogFileFilter filters[] = {
        {"GameCube disc image", "iso;gcm;rvz;wbfs;ciso;nkit"},
        {"All files", "*"},
    };
    int windowCount = 0;
    SDL_Window** windows = SDL_GetWindows(&windowCount);
    SDL_Window* window = windows != nullptr && windowCount > 0 ? windows[0] : nullptr;
    SDL_free(windows);
    if (window == nullptr) {
        // Headless, as on a build runner: nothing to show a dialog on, so say
        // no disc was given rather than waiting for an answer that cannot come.
        PortLog::Write( "metroid_prime_port: no window to ask for a disc image on\n");
        return {};
    }
    PortLog::Write( "metroid_prime_port: no disc image found; asking for one\n");
    SDL_ShowOpenFileDialog(
        [](void*, const char* const* files, int) {
            if (files != nullptr && files[0] != nullptr) {
                chosen = files[0];
            }
            answered.store(true);
        },
        nullptr, window, filters, 2, nullptr, false);
    // Wait for the answer, but not forever: a dialog that never calls back
    // would otherwise hang a scripted or headless run.
    const Uint64 deadline = SDL_GetTicks() + 5 * 60 * 1000;
    while (!answered.load()) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                PortLog::Write( "metroid_prime_port: disc selection cancelled\n");
                return {};
            }
        }
        if (SDL_GetTicks() > deadline) {
            PortLog::Write( "metroid_prime_port: disc selection timed out\n");
            return {};
        }
        SDL_Delay(10);
    }
    if (!chosen.empty()) {
#if defined(__ANDROID__)
        // The picker returns a content:// URI. Copy it to a real file so that the
        // remembered setting needs no grant on the next launch.
        if (std::strncmp(chosen.c_str(), "content://", 10) == 0) {
            const std::string local = CopyDiscFromContentUri(chosen);
            if (!local.empty()) {
                chosen = local;
            }
        }
#endif
        PortDebug::SetDiscPath(chosen.c_str());
        // Persist immediately: the settings are otherwise only written from the
        // overlay's draw path, which never runs if the game cannot frame.
        PortDebug::SaveSettingsNow();
        PortLog::Write( "metroid_prime_port: disc image set to %s\n", chosen.c_str());
    }
    return chosen;
}

// Default texture-replacement folder next to the executable.
const char* DefaultTexturesPath() {
    static const std::string sPath = [] {
#if defined(__ANDROID__)
        char* pref = SDL_GetPrefPath(nullptr, "Metroid Prime");
        if (pref == nullptr) {
            return std::string();
        }
        const std::string dir = std::string(pref) + "textures";
        SDL_free(pref);
#else
        const char* base = SDL_GetBasePath();
        if (base == nullptr) {
            return std::string();
        }
        const std::string dir = std::string(base) + "textures";
#endif
        std::error_code ec;
        return std::filesystem::is_directory(dir, ec) ? dir : std::string();
    }();
    return sPath.empty() ? nullptr : sPath.c_str();
}
} // namespace

int main(int argc, char** argv) {
    // A symbolised stack and a minidump for any crash, before anything else runs.
    PortInstallCrashHandler();
    if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
        std::printf("Metroid Prime native port %s\n", MP_BUILD_REVISION);
        return 0;
    }
#if !defined(__ANDROID__)
    // --import [name [argument]]: run a mod importer (port_importers.h) from
    // the terminal, without starting the game.
    if (argc >= 2 && std::strcmp(argv[1], "--import") == 0) {
        return PortImporters::RunFromCommandLine(argc, argv);
    }
    // --import-remastered <image.nsp> [key file]: build the remastered-models
    // mod from the user's own copy, without starting the game. The disc comes
    // from MP_DISC or the path the game remembers.
    if (argc >= 2 && std::strcmp(argv[1], "--import-remastered") == 0) {
        if (argc < 3) {
            std::fprintf(stderr, "usage: %s --import-remastered <image.nsp> [key file]\n", argv[0]);
            return 2;
        }
        PortDebug::LoadDiscPath();
        const char* disc = ResolveDiscPath(1, argv);
        if (disc == nullptr || !aurora_dvd_open(disc)) {
            std::fprintf(stderr, "cannot open the Metroid Prime disc image; set MP_DISC\n");
            return 1;
        }
        const DVDDiskID* id = DVDGetCurrentDiskID();
        int result = 1;
        if (id == nullptr || std::memcmp(id->gameName, "GM8E", 4) != 0 || std::memcmp(id->company, "01", 2) != 0 ||
            id->diskNumber != 0 || id->gameVersion != 0) {
            std::fprintf(stderr, "unsupported disc; expected GM8E01 USA revision 0\n");
        } else {
            result = PortRemastered::RunImportFromCommandLine(argv[2], argc >= 4 ? argv[3] : "");
        }
        aurora_dvd_close();
        return result;
    }
#endif
    PortLog::Write( "metroid_prime_port: build %s\n", MP_BUILD_REVISION);
    PortRandomizer::EnsureLoaded();
    PortAp::EnsureLoaded();
    // A 16:9 window when widescreen is requested; the game's render mode is
    // widened to match. Values are the default window size only.
    const bool widescreen = PortDebug::AspectMode() != PortDebug::kAspect_4_3;
    // MP_DUMP_TEXTURES=1 writes every source texture to
    // <cachePath>/texture_dumps as DDS, so replacement packs can be authored.
    const char* dumpEnv = std::getenv("MP_DUMP_TEXTURES");
    const bool dumpTextures = dumpEnv != nullptr && dumpEnv[0] != '\0' && std::strcmp(dumpEnv, "0") != 0;
    std::string resourcesPath;
#if defined(__ANDROID__)
    if (char* pref = SDL_GetPrefPath(nullptr, "Metroid Prime")) {
        resourcesPath = pref;
        SDL_free(pref);
    }
#endif
    // MP_MEM1_MB raises the MEM1 arena above its 24 MB default, which grows the
    // CGameAllocator heap, so a very heavy mod model stops running it out of room.
    // Unset, 0 or anything below the default keeps 24 MB; capped at 1 GB so the
    // byte count stays in AuroraConfig's u32. It costs host memory, not GPU memory.
    //
    // A mod with room geometry draws several times what the game's own rooms do, so
    // with one installed the arena starts at kRoomGeoMem1MB and a frame's buffers at
    // kRoomGeoFrameBuffers times their size. MP_FRAME_BUFFERS=<n> sets that scale itself.
    const unsigned long kRoomGeoMem1MB = 256;
    const unsigned long kRoomGeoFrameBuffers = 12;
    const bool roomGeometry = PortMods::HasRoomGeometry();
    uint32_t mem1Size = MEM1_DEFAULT_SIZE;
    {
        const char* e = std::getenv("MP_MEM1_MB");
        const unsigned long mb = e != nullptr ? std::strtoul(e, nullptr, 10) : roomGeometry ? kRoomGeoMem1MB : 0;
        if (mb > MEM1_DEFAULT_SIZE / (1024 * 1024)) {
            mem1Size = static_cast<uint32_t>(std::min(mb, 1024UL) * 1024 * 1024);
            PortLog::Write("port: MEM1 arena raised to %u MB (%s)\n", mem1Size / (1024 * 1024),
                           e != nullptr ? "MP_MEM1_MB" : "room geometry");
        }
    }
    uint32_t frameBufferScale = roomGeometry ? kRoomGeoFrameBuffers : 1;
    if (const char* e = std::getenv("MP_FRAME_BUFFERS")) {
        frameBufferScale = static_cast<uint32_t>(std::clamp(std::strtoul(e, nullptr, 10), 1UL, 16UL));
    }
    if (frameBufferScale > 1) {
        PortLog::Write("port: frame buffers at %ux (%s)\n", frameBufferScale,
                       std::getenv("MP_FRAME_BUFFERS") != nullptr ? "MP_FRAME_BUFFERS" : "room geometry");
    }
    AuroraConfig config = {
        .appName = "Metroid Prime",
        .userPath = std::getenv("MP_USER_PATH"),
        .cachePath = std::getenv("MP_CACHE_PATH"),
        .resourcesPath = resourcesPath.empty() ? nullptr : resourcesPath.c_str(),
        .desiredBackend = BACKEND_AUTO,
        .vsync = false,
        .allowTextureDumps = dumpTextures,
        // Keep the internal framebuffer at the game's logical size so its two
        // framebuffer allocations fit in MEM1; Aurora upscales to the window.
        .windowWidth = static_cast<uint32_t>(widescreen ? 854 : 640),
        .windowHeight = 480,
        .mem1Size = mem1Size,
        .mem2Size = ARAM_DEFAULT_SIZE,
        .frameBufferScale = frameBufferScale,
    };
#if !defined(__ANDROID__)
    // The window icon, for a bare binary that no desktop entry describes.
    // Android takes its icon from the APK.
    static uint8_t windowIcon[] = {
#include "port_window_icon.inc"
    };
    config.iconRGBA8 = windowIcon;
    config.iconWidth = 64;
    config.iconHeight = 64;
#endif
    config.msaa = static_cast<uint32_t>(PortDebug::Msaa());
    config.maxTextureAnisotropy = static_cast<uint16_t>(PortDebug::Anisotropy());

#if defined(_WIN32)
    // SDL's Windows joystick thread held the joystick lock while the game
    // thread sat in SDL_GetGamepadPlayerIndex (PADRead) during an elevator's
    // world load, and the game froze for good with a GameCube adapter and a
    // headset's controllers attached. Polling the joysticks from the game
    // thread's event pump instead never contends with it.
    SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "0");
#endif

#if defined(__ANDROID__)
    // SDL3 drops touch-derived mouse events by default, and ImGui's SDL3
    // backend only understands mouse events. The touch overlay in Java claims
    // gameplay touches, so whatever reaches SDL here is meant for ImGui.
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "1");
    config.logCallback = AndroidLogCallback;
#endif
    // SDL3 reaches for its Wayland backend whenever a Wayland display is
    // reachable, and does so even without WAYLAND_DISPLAY — it falls back to the
    // default socket in XDG_RUNTIME_DIR. Under GNOME that backend never returns
    // from SDL_ShowWindow: it dispatches pending events, libdecor's client-side
    // decoration configure re-enters GTK layout from inside that dispatch, and the
    // process spins at 100% before the first frame is ever presented. Nothing in
    // the port can fix that, so whenever an X display is available ask for the X11
    // backend instead. SDL_VIDEODRIVER still wins, which is also how anyone who
    // wants Wayland opts back in.
    {
        const char* requested = SDL_GetHint(SDL_HINT_VIDEO_DRIVER);
        const char* x11 = std::getenv("DISPLAY");
        const auto present = [](const char* v) { return v != nullptr && v[0] != '\0'; };
        const bool unnamed = !present(requested);
        if (unnamed && present(x11)) {
            SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11");
            PortLog::Write(
                         "port: using SDL's x11 video driver on %s; its wayland backend hangs on "
                         "window creation under GNOME (libdecor). Set SDL_VIDEODRIVER to "
                         "override.\n",
                         x11);
        } else if (unnamed) {
            // Nothing to fall back to: SDL will use Wayland, and on GNOME that is
            // the hang above. Say so before the silence, since there is no way to
            // tell from inside the hang.
            std::fputs("port: no DISPLAY set, so SDL will use its wayland backend, which hangs on "
                       "window creation under GNOME (libdecor). Run under an X display, or set "
                       "SDL_VIDEODRIVER yourself.\n",
                       stderr);
        }
    }
    // PortVr: the OpenXR instance and the adapter it wants come before Aurora
    // picks a device, so the eyes are copied on the compositor's own GPU.
    PortVr::ApplyVrEnvironmentOverrides();
    PortVr::PushVrSettingsToAurora();
    const PortVr::OpenXRStartupResult vrStartup = PortVr::OpenXRPrepareAurora(config);
    if (vrStartup == PortVr::OpenXRStartupResult::Unavailable) {
        PortLog::Write("port: OpenXR unavailable: %s\n", PortVr::OpenXRLastError().c_str());
        if (PortVr::GetVrSettings().required) {
            return 1;
        }
    }
    const AuroraInfo auroraInfo = aurora_initialize(argc, argv, &config);
    // From what the device gave, which can be less than was asked for.
    if (aurora_get_frame_buffer_scale() != frameBufferScale) {
        PortLog::Write("port: frame buffers at %ux, all this device allows\n", aurora_get_frame_buffer_scale());
    }
    PortRoomGeo::SetBuffersReady(aurora_get_frame_buffer_scale() > 1);
    // Apply the persisted render scale. Vsync is applied on the first drawn
    // frame (once the swapchain surface exists) so it uses real capabilities.
    VISetFrameBufferScale(PortDebug::RenderScale());
    // Fit the internal EFB to the game's render-mode aspect rather than the
    // window aspect, so fixed 4:3/16:9 modes are never stretched when the window
    // shape differs; the present letterboxes instead.
    AuroraSetViewportPolicy(AURORA_VIEWPORT_FIT);

    // Optional HD texture replacements, in Aurora's naming convention
    // (tex1_<w>x<h>_<texhash>[_<tluthash>]_<format>.dds/.png); a per-device
    // subfolder is selected from the connected controller. Aurora also accepts
    // Dolphin format names such as CMPR and RGBA8.
    const char* textures = std::getenv("MP_TEXTURES");
    if (textures == nullptr || textures[0] == '\0') {
        textures = DefaultTexturesPath();
    }
    // The user's own pack, over the built-in set. Kept in the pref folder, which
    // updates never replace (the built-in set is read-only in an AppImage or
    // Flatpak, and re-copied on every Android launch).
    std::string userTextures;
    if (const char* env = std::getenv("MP_USER_TEXTURES"); env != nullptr && env[0] != '\0') {
        userTextures = env;
    } else if (char* pref = SDL_GetPrefPath(nullptr, "Metroid Prime")) {
        userTextures = std::string(pref) + "user_textures";
        SDL_free(pref);
    }
    PortTextures::Initialize(textures, userTextures.c_str());
    // Binding-aware prompt icons, served from <textures>/bindings.
    PortPrompts::Initialize(textures);

    // Disc image: an explicit argument or MP_DISC, else the path saved on a
    // previous launch, else a copy beside the executable, else ask for one.
    PortDebug::LoadDiscPath();
    std::string discImage;
    if (const char* resolved = ResolveDiscPath(argc, argv); resolved != nullptr) {
        discImage = resolved;
    } else {
        discImage = AskForDiscImage();
    }
    if (discImage.empty()) {
        PortLog::Write(
                     "metroid_prime_port: no disc image given.\n"
                     "  usage: %s <path to Metroid Prime (USA) (v1.00).iso>\n"
                     "  or set MP_DISC, or place the image next to the executable.\n", argv[0]);
        aurora_shutdown();
        return 1;
    }
    const char* discPath = discImage.c_str();

    if (!aurora_dvd_open(discPath)) {
        ReportDiscOpenFailure(discPath);
        // A remembered disc goes stale whenever its permission lapses: on Android
        // the provider can reclaim a persisted URI grant, and on desktop the file
        // may have been moved or deleted. Retrying once through the picker turns
        // an unexplained exit into a recoverable prompt.
        const bool fromArgs = argc > 1 || std::getenv("MP_DISC") != nullptr;
        // DiscPath() is null when no disc is remembered, and comparing a
        // std::string with a null pointer is undefined (it calls strlen(NULL)).
        const char* rememberedDisc = PortDebug::DiscPath();
        if (rememberedDisc != nullptr && discImage == rememberedDisc && !fromArgs) {
            PortLog::Write( "metroid_prime_port: asking for the disc image again\n");
            PortDebug::SetDiscPath("");
            PortDebug::SaveSettingsNow();
            discImage = AskForDiscImage();
            if (discImage.empty()) {
                PortLog::Write( "metroid_prime_port: no disc image given.\n"
                                "  usage: %s <path to Metroid Prime (USA) (v1.00).iso>\n",
                                argv[0]);
                aurora_shutdown();
                return 1;
            }
            discPath = discImage.c_str();
            if (!aurora_dvd_open(discPath)) {
                ReportDiscOpenFailure(discPath);
                aurora_shutdown();
                return 1;
            }
        } else {
            aurora_shutdown();
            return 1;
        }
    }
    std::printf("metroid_prime_port: disc mounted: %s\n", discPath);
    const DVDDiskID* discId = DVDGetCurrentDiskID();
    if (discId == nullptr || std::memcmp(discId->gameName, "GM8E", 4) != 0 ||
        std::memcmp(discId->company, "01", 2) != 0 || discId->diskNumber != 0 || discId->gameVersion != 0) {
        PortLog::Write( "metroid_prime_port: unsupported disc; expected GM8E01 USA revision 0.\n");
        aurora_dvd_close();
        aurora_shutdown();
        return 1;
    }
#if !defined(__ANDROID__)
    // A Remastered import finished in the last session becomes the mod now,
    // before anything has a file of the old one open.
    if (PortRemastered::ApplyPendingImport()) {
        PortLog::Write("metroid_prime_port: installed the imported Remastered models\n");
    }
#endif
    PortMods::Initialize();

    // Prime the window/event state so the game's first aurora_begin_frame can
    // succeed (the game submits GX during early init, before its main loop).
    aurora_update();

    // PortVr: the session, the eye swapchains and the pacing thread, while
    // Aurora's frame worker is still idle. A failure falls back to the desktop
    // unless the settings require the headset.
    if (vrStartup == PortVr::OpenXRStartupResult::Prepared) {
        if (PortVr::OpenXRStartAfterAurora(auroraInfo.backend)) {
            PortLog::Write("port: OpenXR session started\n");
        } else {
            PortLog::Write("port: OpenXR did not start: %s\n", PortVr::OpenXRLastError().c_str());
            if (PortVr::GetVrSettings().required) {
                aurora_dvd_close();
                aurora_shutdown();
                return 1;
            }
        }
    }

    int result = 1;
    try {
        result = metroid_main(argc, argv);
    } catch (const std::exception& error) {
        PortLog::Write( "metroid_prime_port: %s\n", error.what());
    }

    // PortVr: stop publishing stereo work and destroy the session before
    // Aurora's device goes.
    PortVr::OpenXRShutdownBeforeAurora();
    AIPortShutdown();
#if !defined(__ANDROID__)
    // An import still running reads the disc.
    PortRemastered::StopImport();
#endif
    aurora_dvd_close();
    aurora_shutdown();
    return result;
}
