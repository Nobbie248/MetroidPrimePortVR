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
#include "port_paths.h"
#include "port_apclient.h"
#include "port_randomizer.h"
#include "port_textures.h"
#include "port_prompts.h"
#include "port_build_info.h"
#include "crash_handler.h"
#include "port_log.h"
#include "port_log_file.h"
#include "port_mods.h"
#include "port_room_geo.h"
#include "port_importers.h"
#include "port_remastered_import.h"
#include "port_gci.h"

#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_filesystem.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_properties.h>
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
    char line[2048];
    std::snprintf(line, sizeof(line), "[%s] %.*s", module != nullptr ? module : "", static_cast< int >(len),
                  message);
    __android_log_write(priority, "aurora", line);
    PortLogFile::Write("aurora", line);
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

// How well a file next to the executable fits as the disc: plain images (.iso,
// .gcm, NKit's .nkit.iso) start with the disc header, so the wrong game or
// region can be skipped before it fails to boot; the compressed formats keep
// it elsewhere and are taken on trust, after any plain image that matched.
enum class DiscMatch { No, Maybe, Yes };

DiscMatch MatchDiscImage(const std::filesystem::path& path) {
    if (!IsDiscImage(path)) {
        return DiscMatch::No;
    }
    const std::string ext = LowerExtension(path);
    if (ext != ".iso" && ext != ".gcm") {
        return DiscMatch::Maybe;
    }
    // Through SDL with a UTF-8 name: std::fopen on Windows takes the ANSI code
    // page, which cannot name every folder a user might unpack the game into.
    SDL_IOStream* file = SDL_IOFromFile(PortPaths::detail::ToUtf8(path).c_str(), "rb");
    if (file == nullptr) {
        return DiscMatch::No;
    }
    Uint8 header[8] = {};
    const size_t got = SDL_ReadIO(file, header, sizeof(header));
    SDL_CloseIO(file);
    // Game id, maker, disc number, revision: GM8E01, disc 0, v1.00.
    return got == sizeof(header) && std::memcmp(header, "GM8E01", 6) == 0 && header[6] == 0 && header[7] == 0
               ? DiscMatch::Yes
               : DiscMatch::No;
}

// Looks for a disc image next to the executable (and in its immediate
// subdirectories) so a copied build is self-contained and starts without
// asking. For an AppImage that is the folder the .AppImage file is in.
std::string FindDiscNextToExecutable() {
#if defined(__ANDROID__)
    const char* rawBase = SDL_GetBasePath();
    const std::string base = rawBase != nullptr ? rawBase : "";
#else
    const std::string base = PortPaths::detail::ExecutableFolder();
#endif
    if (base.empty()) {
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
        const fs::path baseDir = PortPaths::detail::FromUtf8(base);
        std::vector<fs::path> dirs{baseDir};
        for (fs::directory_iterator it(baseDir, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code entryEc;
            if (it->is_directory(entryEc)) {
                dirs.push_back(it->path());
            }
        }
        // Directory order is the file system's; sorted, the same image wins on
        // every launch when there are several.
        std::sort(dirs.begin() + 1, dirs.end());
        std::string maybe;
        for (const fs::path& dir : dirs) {
            std::vector<fs::path> files;
            for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
                std::error_code entryEc;
                if (it->is_regular_file(entryEc) && IsDiscImage(it->path())) {
                    files.push_back(it->path());
                }
            }
            std::sort(files.begin(), files.end());
            for (const fs::path& file : files) {
                const DiscMatch match = MatchDiscImage(file);
                if (match == DiscMatch::Yes) {
                    return PortPaths::detail::ToUtf8(file);
                }
                if (match == DiscMatch::Maybe && maybe.empty()) {
                    maybe = PortPaths::detail::ToUtf8(file);
                }
            }
        }
        if (!maybe.empty()) {
            return maybe;
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
            const std::string local = PortPaths::UserFolder() + "disc.iso";
            std::error_code ec;
            static const std::string sLocal =
                !PortPaths::UserFolder().empty() && std::filesystem::exists(local, ec) ? local : std::string();
            if (!sLocal.empty()) {
                return sLocal.c_str();
            }
            return saved;
        }
        // The copy made from a picked file sits in the data folder, which may
        // have moved since (port_data_folder.h): prefer the copy in the folder
        // in use, so deleting the old one loses nothing.
        if (const char* name = std::strrchr(saved, '/'); name != nullptr && std::strcmp(name, "/disc.iso") == 0) {
            const std::string moved = PortPaths::UserFolder() + "disc.iso";
            std::error_code ec;
            static const std::string sMoved =
                !PortPaths::UserFolder().empty() && moved != saved && std::filesystem::exists(moved, ec) ? moved
                                                                                                         : std::string();
            if (!sMoved.empty()) {
                return sMoved.c_str();
            }
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
    if (sFound.empty()) {
        return nullptr;
    }
    PortLog::Write("metroid_prime_port: using the disc image next to the executable: %s\n", sFound.c_str());
    return sFound.c_str();
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
    const std::string& folder = PortPaths::UserFolder();
    if (folder.empty()) {
        PortLog::Write( "metroid_prime_port: no data folder to copy the disc into\n");
        return {};
    }
    const std::filesystem::path target = std::filesystem::path(folder) / "disc.iso";
    // Copied under another name and renamed when complete: a copy killed part
    // way (the app closed during a multi-minute copy) must not leave a
    // truncated disc.iso, which ResolveDiscPath would prefer on every launch.
    const std::filesystem::path partial = std::filesystem::path(folder) / "disc.iso.part";

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
        // The copy takes minutes on a device, long enough for the screen to go
        // off; as in AskForDiscImage, nothing else drops a destroyed surface.
        aurora_release_lost_surface();
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
    // Static: SDL reads the filters until the dialog closes, which can be after
    // a timed-out wait has returned.
    static const SDL_DialogFileFilter filters[] = {
        {"Metroid Prime disc image (iso, gcm, rvz, wbfs, ciso, nkit)", "iso;gcm;rvz;wbfs;ciso;nkit"},
        {"All files", "*"},
    };
    if (const char* env = std::getenv("MP_NO_DISC_DIALOG"); env != nullptr && env[0] == '1') {
        // For scripted runs with a window, such as the packaged startup check
        // on a build runner, where the dialog would sit open until it times out.
        PortLog::Write( "metroid_prime_port: not asking for a disc image (MP_NO_DISC_DIALOG)\n");
        return {};
    }
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
    // A titled dialog: an untitled file picker on first launch does not say
    // what it wants. (Android's picker shows no title.)
    const SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, const_cast<SDL_DialogFileFilter*>(filters));
    SDL_SetNumberProperty(props, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, 2);
    SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, window);
    SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING,
                          "Select your Metroid Prime disc image (GameCube, USA, v1.00)");
    SDL_ShowFileDialogWithProperties(
        SDL_FILEDIALOG_OPENFILE,
        [](void*, const char* const* files, int) {
            if (files != nullptr && files[0] != nullptr) {
                chosen = files[0];
            }
            answered.store(true);
        },
        nullptr, props);
    SDL_DestroyProperties(props);
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
        // Android's picker covers the app and destroys its surface. Nothing
        // draws a frame here, so drop the swapchain now or it stays attached to
        // the dead window, which can lose the device (the likely cause of a
        // crash reported on the first launch, the one that asks for the disc).
        aurora_release_lost_surface();
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
    // --import-remastered-movies does only the menu movies, into that mod.
    const bool importMovies = argc >= 2 && std::strcmp(argv[1], "--import-remastered-movies") == 0;
    if (importMovies || (argc >= 2 && std::strcmp(argv[1], "--import-remastered") == 0)) {
        if (argc < 3) {
            std::fprintf(stderr, "usage: %s %s <image.nsp> [key file]\n", argv[0], argv[1]);
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
            result = PortRemastered::RunImportFromCommandLine(argv[2], argc >= 4 ? argv[3] : "", importMovies);
        }
        aurora_dvd_close();
        return result;
    }
#endif
    // The file log starts first so it holds everything after it, build id included.
    {
        const char* e = std::getenv("MP_LOG_FILE");
        const bool logFile = e != nullptr ? e[0] != '\0' && std::strcmp(e, "0") != 0 : PortDebug::LogFile();
        if (logFile && !PortLogFile::Start()) {
            PortLog::Write("port: cannot write the log to %s\n", PortLogFile::Path().c_str());
        }
    }
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
#if defined(__ANDROID__)
    // Every scale step costs about 13 MB in each of the six copies of a frame's
    // buffers; at 12x a tablet's game ran at 1.3 GB and Android killed it for
    // memory. 6x still holds the heaviest room measured (20 MiB of vertices) 1.5x.
    const unsigned long kRoomGeoFrameBuffers = 6;
#else
    const unsigned long kRoomGeoFrameBuffers = 12;
#endif
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
    // Settings, mods, save states and the shader caches sit next to the
    // executable when that folder can be written to (port_paths.h).
    const std::string& userFolder = PortPaths::UserFolder();
    const char* cacheEnv = std::getenv("MP_CACHE_PATH");
#if defined(__ANDROID__)
    // The shader caches stay in app storage when the data moves to shared
    // storage: they are disposable, and SQLite is slow on the shared mount.
    const std::string defaultCache = PortPaths::detail::PrivateFolder();
#else
    const std::string& defaultCache = userFolder;
#endif
    const std::string cacheFolder = cacheEnv != nullptr && cacheEnv[0] != '\0' ? cacheEnv : defaultCache;
    PortLog::Write("port: user folder %s%s\n", userFolder.empty() ? "(none)" : userFolder.c_str(),
#if defined(__ANDROID__)
                   PortPaths::IsPortable() ? " (shared storage)" : "");
#else
                   PortPaths::IsPortable() ? " (next to the executable)" : "");
#endif
    AuroraConfig config = {
        .appName = "Metroid Prime",
        .userPath = userFolder.empty() ? nullptr : userFolder.c_str(),
        .cachePath = cacheFolder.empty() ? nullptr : cacheFolder.c_str(),
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
    config.startFullscreen = PortDebug::Fullscreen();
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
#if !defined(_WIN32) && !defined(__ANDROID__)
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
#endif
    // PortVr: the OpenXR instance and the adapter it wants come before Aurora
    // picks a device, so the eyes are copied on the compositor's own GPU.
    PortVr::ApplyVrEnvironmentOverrides();
    PortVr::PushVrSettingsToAurora();
    const PortVr::OpenXRStartupResult vrStartup = PortVr::OpenXRPrepareAurora(config);
    if (vrStartup == PortVr::OpenXRStartupResult::Unavailable) {
        PortLog::Write("port: OpenXR unavailable: %s\n", PortVr::OpenXRLastError().c_str());
        // PortVr: a Quest has no desktop to fall back to.
        if (PortVr::GetVrSettings().required || PortVr::OpenXRHeadsetIsOnlyDisplay()) {
            PortVr::OpenXRRequestAppQuit("The headset could not start: " + PortVr::OpenXRLastError());
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
    // The user's own pack, over the built-in set. Kept in the user folder, under
    // a name updates never replace (the built-in set is read-only in an AppImage or
    // Flatpak, and re-copied on every Android launch).
    std::string userTextures;
    if (const char* env = std::getenv("MP_USER_TEXTURES"); env != nullptr && env[0] != '\0') {
        userTextures = env;
    } else if (!PortPaths::UserFolder().empty()) {
        userTextures = PortPaths::UserFolder() + "user_textures";
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
    // A Remastered import finished in the last session becomes the mod now,
    // before anything has a file of the old one open.
    if (PortRemastered::ApplyPendingImport()) {
        PortLog::Write("metroid_prime_port: installed the imported Remastered models\n");
    }
    // PortVr: a memory card the Quest launcher handed over goes into the card
    // before the game mounts it, in the folder DolphinCMemoryCardSys picks.
    {
#if defined(__ANDROID__)
        const std::string cardBase = PortPaths::UserFolder();
#else
        const std::string cardBase = PortPaths::CardFolder();
#endif
        const std::filesystem::path user = PortGci::PathFromString(PortPaths::UserFolder());
        const std::filesystem::path card = PortGci::PathFromString(cardBase) / "USA" / "Card A";
        if (!cardBase.empty() && PortGci::ImportPending(user, card)) {
            PortLog::Write("metroid_prime_port: imported the memory card the launcher left in %s\n",
                           PortGci::PathString(user / "primedgun" / "pending_import").c_str());
        }
    }
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
            if (PortVr::GetVrSettings().required || PortVr::OpenXRHeadsetIsOnlyDisplay()) {
                PortVr::OpenXRRequestAppQuit("The headset session could not start: " + PortVr::OpenXRLastError());
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
    // Port: Android never runs exit handlers (SDL_main returns into Java and the
    // process is later killed), so the atexit save would not happen there.
    PortDebug::SaveSettingsNow();

    // PortVr: stop publishing stereo work and destroy the session before
    // Aurora's device goes.
    PortVr::OpenXRShutdownBeforeAurora();
    AIPortShutdown();
    // An import still running reads the disc.
    PortRemastered::StopImport();
    aurora_dvd_close();
    aurora_shutdown();
    return result;
}
