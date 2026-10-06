// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Quest launcher's native half: the PC launcher's Qt-free core
// (launcher/core) behind JNI, for org.primedgun.v2.launcher.LauncherNative
// (quest/app). Both launchers then edit port_settings.ini through the same key
// table, the same line-preserving file editor and the same cannon pack code,
// which port_launcher_tests checks.
//
// The launcher process loads this library alone: no SDL, no Aurora, no game.
// Only strings, numbers and arrays cross the boundary. Every entry point holds
// one lock (the panel's UI thread and its background work can both call in)
// and catches, since an exception must not unwind into the JVM.

#include "cannon_textures.h"
#include "dds_preview.h"
#include "disc_probe.h"
#include "launcher_keys.h"
#include "port_settings_file.h"
#include "primedgun_import.h"
#include "settings_model.h"

#include "port_build_info.h"

#include <jni.h>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using namespace PrimedGunLauncher;

namespace {

struct LauncherState {
  bool ready = false;
  fs::path settingsFile;
  Cannon::Folders cannon;
  SettingsModel model;
  // The file as the launcher last read or wrote it, to tell the game's own
  // rewrite apart from nothing having happened.
  std::string knownText;
};

std::mutex gLock;
LauncherState gState;

// --- strings -----------------------------------------------------------------
// Through UTF-16, not GetStringUTFChars / NewStringUTF: those speak modified
// UTF-8, which differs from the core's UTF-8 for characters past U+FFFF.

std::string FromJava(JNIEnv* env, jstring text) {
  if (text == nullptr) {
    return {};
  }
  const jsize length = env->GetStringLength(text);
  const jchar* units = env->GetStringChars(text, nullptr);
  std::string out;
  out.reserve(static_cast<size_t>(length));
  for (jsize i = 0; i < length; ++i) {
    uint32_t code = units[i];
    if (code >= 0xD800 && code <= 0xDBFF && i + 1 < length && units[i + 1] >= 0xDC00 &&
        units[i + 1] <= 0xDFFF) {
      code = 0x10000 + ((code - 0xD800) << 10) + (units[i + 1] - 0xDC00);
      ++i;
    }
    if (code < 0x80) {
      out += static_cast<char>(code);
    } else if (code < 0x800) {
      out += static_cast<char>(0xC0 | (code >> 6));
      out += static_cast<char>(0x80 | (code & 0x3F));
    } else if (code < 0x10000) {
      out += static_cast<char>(0xE0 | (code >> 12));
      out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (code & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (code >> 18));
      out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (code & 0x3F));
    }
  }
  env->ReleaseStringChars(text, units);
  return out;
}

jstring ToJava(JNIEnv* env, std::string_view text) {
  std::u16string units;
  units.reserve(text.size());
  for (size_t i = 0; i < text.size();) {
    const auto byte = static_cast<unsigned char>(text[i]);
    uint32_t code = 0xFFFD;
    size_t extra = 0;
    if (byte < 0x80) {
      code = byte;
    } else if ((byte & 0xE0) == 0xC0) {
      code = byte & 0x1F;
      extra = 1;
    } else if ((byte & 0xF0) == 0xE0) {
      code = byte & 0x0F;
      extra = 2;
    } else if ((byte & 0xF8) == 0xF0) {
      code = byte & 0x07;
      extra = 3;
    }
    ++i;
    for (size_t k = 0; k < extra; ++k, ++i) {
      if (i >= text.size() || (static_cast<unsigned char>(text[i]) & 0xC0) != 0x80) {
        code = 0xFFFD;
        break;
      }
      code = (code << 6) | (static_cast<unsigned char>(text[i]) & 0x3F);
    }
    if (code >= 0x10000) {
      code -= 0x10000;
      units += static_cast<char16_t>(0xD800 + (code >> 10));
      units += static_cast<char16_t>(0xDC00 + (code & 0x3FF));
    } else {
      units += static_cast<char16_t>(code);
    }
  }
  return env->NewString(reinterpret_cast<const jchar*>(units.data()), static_cast<jsize>(units.size()));
}

jobjectArray ToJavaArray(JNIEnv* env, const std::vector<std::string>& items) {
  jclass stringClass = env->FindClass("java/lang/String");
  jobjectArray array = env->NewObjectArray(static_cast<jsize>(items.size()), stringClass, nullptr);
  for (size_t i = 0; i < items.size(); ++i) {
    jstring item = ToJava(env, items[i]);
    env->SetObjectArrayElement(array, static_cast<jsize>(i), item);
    env->DeleteLocalRef(item);
  }
  env->DeleteLocalRef(stringClass);
  return array;
}

fs::path PathFromJava(JNIEnv* env, jstring text) {
  const std::string utf8 = FromJava(env, text);
  return fs::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string PathToUtf8(const fs::path& path) {
  const std::u8string text = path.u8string();
  return std::string(text.begin(), text.end());
}

std::string ReadText(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// --- settings ----------------------------------------------------------------

bool LoadLocked() {
  PortSettingsFile file;
  const bool ok = file.Load(gState.settingsFile);
  gState.model.Load(file);
  gState.knownText = file.Text();
  return ok;
}

// Writes `key` alone, or every change when empty. Returns the error, or "".
std::string SaveLocked(std::string_view key) {
  PortSettingsFile file;
  if (!file.Load(gState.settingsFile)) {
    return "cannot read " + PathToUtf8(gState.settingsFile);
  }
  if (key.empty()) {
    gState.model.ApplyChanges(file);
  } else {
    file.Set(std::string(key), gState.model.Value(key));
  }
  std::string error;
  if (!file.Save(gState.settingsFile, error)) {
    return error.empty() ? "cannot write " + PathToUtf8(gState.settingsFile) : error;
  }
  if (key.empty()) {
    gState.model.MarkSaved();
  } else {
    gState.model.MarkSaved(key);
  }
  gState.knownText = file.Text();
  return {};
}

const char* KindName(KeyKind kind) {
  switch (kind) {
  case KeyKind::Bool:
    return "bool";
  case KeyKind::Float:
    return "float";
  case KeyKind::Int:
    return "int";
  case KeyKind::Choice:
    return "choice";
  }
  return "bool";
}

const char* DiscCheckName(DiscCheck check) {
  switch (check) {
  case DiscCheck::Ok:
    return "ok";
  case DiscCheck::WrongGame:
    return "wrong_game";
  case DiscCheck::WrongRevision:
    return "wrong_revision";
  case DiscCheck::UnsupportedFormat:
    return "unsupported_format";
  case DiscCheck::Unverified:
    return "unverified";
  case DiscCheck::Unreadable:
    return "unreadable";
  }
  return "unreadable";
}

void ThrowToJava(JNIEnv* env, const char* what) {
  if (env->ExceptionCheck()) {
    return;
  }
  if (jclass error = env->FindClass("java/lang/IllegalStateException")) {
    env->ThrowNew(error, what);
    env->DeleteLocalRef(error);
  }
}

// Runs `body` under the lock; a C++ exception becomes a Java one.
template <typename Result, typename Body>
Result Guarded(JNIEnv* env, Result fallback, Body&& body) {
  try {
    const std::lock_guard<std::mutex> lock(gLock);
    return body();
  } catch (const std::exception& e) {
    ThrowToJava(env, e.what());
  } catch (...) {
    ThrowToJava(env, "unknown native error");
  }
  return fallback;
}

} // namespace

#define LAUNCHER_JNI(ret, name) \
  extern "C" JNIEXPORT ret JNICALL Java_org_primedgun_v2_launcher_LauncherNative_##name

// --- setup -------------------------------------------------------------------

// The game's user folder (MP_USER_PATH: port_settings.ini, user_textures, the
// cannon library) and the cannon slots the APK carries, unpacked. Seeds the
// user's cannon library and reads the settings. False when the settings file
// exists but cannot be read.
LAUNCHER_JNI(jboolean, init)(JNIEnv* env, jclass, jstring userFolder, jstring shippedCannon) {
  return Guarded<jboolean>(env, JNI_FALSE, [&] {
    const fs::path user = PathFromJava(env, userFolder);
    gState.settingsFile = user / "port_settings.ini";
    gState.cannon.library = user / "primedgun" / "cannon_textures";
    gState.cannon.shippedLibrary = PathFromJava(env, shippedCannon);
    gState.cannon.userTextures = user / "user_textures";
    gState.ready = true;
    Cannon::SeedLibrary(gState.cannon);
    return LoadLocked() ? JNI_TRUE : JNI_FALSE;
  });
}

LAUNCHER_JNI(jstring, buildRevision)(JNIEnv* env, jclass) {
  return ToJava(env, MP_BUILD_REVISION);
}

// Every key the launcher edits, one per entry, tab-separated:
// key, kind (bool/float/int/choice), default, min, max, step, active (1/0),
// reset by Reset All (1/0), choices (comma-separated).
LAUNCHER_JNI(jobjectArray, keyTable)(JNIEnv* env, jclass) {
  std::vector<std::string> rows;
  for (const KeyInfo& info : LauncherKeys()) {
    std::string row(info.key);
    row += '\t';
    row += KindName(info.kind);
    row += '\t';
    row += info.defaultValue;
    row += '\t' + FormatFloat(info.uiMin) + '\t' + FormatFloat(info.uiMax) + '\t' +
           FormatFloat(info.uiStep);
    row += info.active ? "\t1" : "\t0";
    row += info.resetAll ? "\t1" : "\t0";
    row += '\t';
    row += info.choices;
    rows.push_back(std::move(row));
  }
  return ToJavaArray(env, rows);
}

// --- settings ----------------------------------------------------------------

// Reads the file again and drops every unsaved change.
LAUNCHER_JNI(jboolean, reload)(JNIEnv* env, jclass) {
  return Guarded<jboolean>(env, JNI_FALSE, [&] {
    return gState.ready && LoadLocked() ? JNI_TRUE : JNI_FALSE;
  });
}

// Whether the file differs from what the launcher last read or wrote: the game
// rewrote it (it does on every exit).
LAUNCHER_JNI(jboolean, changedOnDisk)(JNIEnv* env, jclass) {
  return Guarded<jboolean>(env, JNI_FALSE, [&] {
    if (!gState.ready) {
      return JNI_FALSE;
    }
    PortSettingsFile file;
    return file.Load(gState.settingsFile) && file.Text() != gState.knownText ? JNI_TRUE : JNI_FALSE;
  });
}

LAUNCHER_JNI(jstring, value)(JNIEnv* env, jclass, jstring key) {
  return Guarded<jstring>(env, nullptr, [&] {
    return ToJava(env, gState.model.Value(FromJava(env, key)));
  });
}

// Stored as the game would write it back; an unknown key is ignored.
LAUNCHER_JNI(void, set)(JNIEnv* env, jclass, jstring key, jstring value) {
  Guarded<int>(env, 0, [&] {
    gState.model.Set(FromJava(env, key), FromJava(env, value));
    return 0;
  });
}

LAUNCHER_JNI(void, resetToDefault)(JNIEnv* env, jclass, jstring key) {
  Guarded<int>(env, 0, [&] {
    gState.model.ResetToDefault(FromJava(env, key));
    return 0;
  });
}

LAUNCHER_JNI(void, resetAll)(JNIEnv* env, jclass) {
  Guarded<int>(env, 0, [&] {
    gState.model.ResetAll();
    return 0;
  });
}

LAUNCHER_JNI(jboolean, dirty)(JNIEnv* env, jclass) {
  return Guarded<jboolean>(env, JNI_FALSE, [&] { return gState.model.Dirty() ? JNI_TRUE : JNI_FALSE; });
}

// Writes the changed keys over the file as it is now. Returns the error, or "".
LAUNCHER_JNI(jstring, save)(JNIEnv* env, jclass) {
  return Guarded<jstring>(env, nullptr, [&] {
    if (!gState.ready) {
      return ToJava(env, "the launcher is not initialised");
    }
    if (!gState.model.Dirty()) {
      return ToJava(env, "");
    }
    return ToJava(env, SaveLocked({}));
  });
}

// Writes one key at once (the cannon slot, whose files are already applied).
LAUNCHER_JNI(jstring, saveKey)(JNIEnv* env, jclass, jstring key) {
  return Guarded<jstring>(env, nullptr, [&] {
    if (!gState.ready) {
      return ToJava(env, "the launcher is not initialised");
    }
    return ToJava(env, SaveLocked(FromJava(env, key)));
  });
}

// --- disc --------------------------------------------------------------------

LAUNCHER_JNI(jboolean, isSupportedDiscName)(JNIEnv* env, jclass, jstring name) {
  return Guarded<jboolean>(env, JNI_FALSE, [&] {
    return IsSupportedDiscExtension(PathFromJava(env, name)) ? JNI_TRUE : JNI_FALSE;
  });
}

// The first bytes of a picked file (0x8008 cover every format the port reads),
// and the extension of its name. Returns check|gameId|revision.
LAUNCHER_JNI(jstring, probeDisc)(JNIEnv* env, jclass, jstring extension, jbyteArray header,
                                 jint length) {
  return Guarded<jstring>(env, nullptr, [&] {
    std::vector<uint8_t> bytes(static_cast<size_t>(std::max<jint>(length, 0)));
    if (!bytes.empty()) {
      env->GetByteArrayRegion(header, 0, length, reinterpret_cast<jbyte*>(bytes.data()));
    }
    const DiscInfo info = ProbeDiscBytes(FromJava(env, extension), bytes.data(), bytes.size());
    return ToJava(env, std::string(DiscCheckName(info.check)) + '|' + info.gameId + '|' +
                           std::to_string(info.revision));
  });
}

// --- cannon textures -----------------------------------------------------------

LAUNCHER_JNI(jstring, cannonLibraryFolder)(JNIEnv* env, jclass) {
  return Guarded<jstring>(env, nullptr, [&] { return ToJava(env, PathToUtf8(gState.cannon.library)); });
}

// Where an applied slot goes: the pack root, or each device folder of a pack
// split by device.
LAUNCHER_JNI(jobjectArray, cannonPackFolders)(JNIEnv* env, jclass) {
  return Guarded<jobjectArray>(env, nullptr, [&] {
    std::vector<std::string> folders;
    for (const fs::path& folder : Cannon::PackFolders(gState.cannon)) {
      folders.push_back(PathToUtf8(folder / std::u8string(Cannon::kPackFolder.begin(),
                                                          Cannon::kPackFolder.end())));
    }
    return ToJavaArray(env, folders);
  });
}

// The slot's file for texture `index`, or "" when it has none. Slot 0
// (Default) answers with the unmodified texture, for its preview.
LAUNCHER_JNI(jstring, cannonSource)(JNIEnv* env, jclass, jint slot, jint index) {
  return Guarded<jstring>(env, nullptr, [&] {
    if (index < 0 || index >= static_cast<jint>(Cannon::kTextureNames.size())) {
      return ToJava(env, "");
    }
    const fs::path file = slot <= 0 ? Cannon::DefaultPreview(gState.cannon, index)
                                    : Cannon::ResolveSource(gState.cannon, slot, index);
    return ToJava(env, PathToUtf8(file));
  });
}

// Each returns the error, or "".
LAUNCHER_JNI(jstring, cannonApply)(JNIEnv* env, jclass, jint slot) {
  return Guarded<jstring>(env, nullptr, [&] {
    std::string error;
    if (!Cannon::ApplySlot(gState.cannon, slot, error)) {
      return ToJava(env, error.empty() ? "the slot could not be applied" : error);
    }
    return ToJava(env, "");
  });
}

LAUNCHER_JNI(jstring, cannonImport)(JNIEnv* env, jclass, jint slot, jint index, jstring source) {
  return Guarded<jstring>(env, nullptr, [&] {
    std::string error;
    if (Cannon::ImportIntoSlot(gState.cannon, slot, index, PathFromJava(env, source), error).empty()) {
      return ToJava(env, error.empty() ? "the texture could not be imported" : error);
    }
    return ToJava(env, "");
  });
}

LAUNCHER_JNI(jstring, cannonRemoveShine)(JNIEnv* env, jclass, jint slot) {
  return Guarded<jstring>(env, nullptr, [&] {
    std::string error;
    if (Cannon::RemoveShine(gState.cannon, slot, error).empty()) {
      return ToJava(env, error.empty() ? "the shine could not be removed" : error);
    }
    return ToJava(env, "");
  });
}

LAUNCHER_JNI(jstring, cannonRestoreShine)(JNIEnv* env, jclass, jint slot) {
  return Guarded<jstring>(env, nullptr, [&] {
    std::string error;
    if (Cannon::RestoreShine(gState.cannon, slot, error).empty()) {
      return ToJava(env, error.empty() ? "the shine could not be restored" : error);
    }
    return ToJava(env, "");
  });
}

// A DXT1 .dds as a thumbnail, {width, height, ARGB pixels...}, or null for
// anything else; Android's BitmapFactory reads the PNG slots itself. The tab
// shows 56 dp previews, so an upscaled texture is sampled down to 256 texels a
// side, and one too large to decode cheaply gets no preview at all.
LAUNCHER_JNI(jintArray, decodeDds)(JNIEnv* env, jclass, jstring path) {
  return Guarded<jintArray>(env, nullptr, [&]() -> jintArray {
    constexpr uint32_t kMaxDecodedSide = 4096;
    constexpr int kThumbnailSide = 256;
    const std::string bytes = ReadText(PathFromJava(env, path));
    // DDS_HEADER: height at file offset 12, width at 16, little-endian.
    const auto field = [&bytes](size_t offset) {
      uint32_t value = 0;
      for (size_t i = 0; i < 4 && offset + i < bytes.size(); ++i) {
        value |= static_cast<uint32_t>(static_cast<uint8_t>(bytes[offset + i])) << (8 * i);
      }
      return value;
    };
    if (bytes.size() < 20 || field(12) > kMaxDecodedSide || field(16) > kMaxDecodedSide) {
      return nullptr;
    }
    const RgbaImage image =
        DecodeDxt1Dds(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    if (image.Empty()) {
      return nullptr;
    }
    const int step = std::max({1, (image.width + kThumbnailSide - 1) / kThumbnailSide,
                               (image.height + kThumbnailSide - 1) / kThumbnailSide});
    const int width = (image.width + step - 1) / step;
    const int height = (image.height + step - 1) / step;
    std::vector<jint> out(static_cast<size_t>(width) * static_cast<size_t>(height) + 2);
    out[0] = width;
    out[1] = height;
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        const size_t source = static_cast<size_t>(y * step) * static_cast<size_t>(image.width) +
                              static_cast<size_t>(x * step);
        const uint8_t* p = &image.pixels[source * 4];
        out[2 + static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)] =
            static_cast<jint>((static_cast<uint32_t>(p[3]) << 24) |
                              (static_cast<uint32_t>(p[0]) << 16) |
                              (static_cast<uint32_t>(p[1]) << 8) | p[2]);
      }
    }
    jintArray array = env->NewIntArray(static_cast<jsize>(out.size()));
    if (array == nullptr) {
      // OutOfMemoryError: no preview rather than a pending exception.
      env->ExceptionClear();
      return nullptr;
    }
    env->SetIntArrayRegion(array, 0, static_cast<jsize>(out.size()), out.data());
    return array;
  });
}

// --- PrimedGun transfer ----------------------------------------------------------

// PrimedGun's settings from an old install's user folder (Config/PrimedGun.ini,
// or Config/Qt.ini from older builds) as port keys: the file read, then
// key=value entries. Empty when the folder holds neither.
LAUNCHER_JNI(jobjectArray, readOldSettings)(JNIEnv* env, jclass, jstring userFolder) {
  return Guarded<jobjectArray>(env, nullptr, [&] {
    const OldSettings old = ReadOldSettings(PathFromJava(env, userFolder));
    std::vector<std::string> out;
    if (!old.source.empty()) {
      out.push_back(PathToUtf8(old.source));
      for (const auto& [key, value] : old.values) {
        out.push_back(key + '=' + value);
      }
    }
    return ToJavaArray(env, out);
  });
}
