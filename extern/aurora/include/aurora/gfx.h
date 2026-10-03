#ifndef AURORA_GFX_H
#define AURORA_GFX_H

#ifdef __cplusplus
#include <cstdint>

extern "C" {
#else
#include "stdint.h"
#endif

#if !defined(NDEBUG) && !defined(AURORA_GFX_DEBUG_GROUPS)
#define AURORA_GFX_DEBUG_GROUPS
#endif

void push_debug_group(const char* label);
void pop_debug_group();

typedef struct {
  uint32_t queuedPipelines;
  uint32_t createdPipelines;
  uint32_t drawCallCount;
  uint32_t mergedDrawCallCount;
  uint32_t lastVertSize;
  uint32_t lastUniformSize;
  uint32_t lastIndexSize;
  uint32_t lastStorageSize;
  uint32_t lastTextureUploadSize;
} AuroraStats;

const AuroraStats* aurora_get_stats();
// Live GPU textures: [0] sampled only, [1] render targets/copies. Bytes are the mip chain's size.
typedef struct {
  uint32_t count[2];
  uint64_t bytes[2];
} AuroraTextureStats;
void aurora_get_texture_stats(AuroraTextureStats* out);
// The AuroraConfig::frameBufferScale in use: the device may allow less than was asked for.
uint32_t aurora_get_frame_buffer_scale();
float aurora_get_fps();

void aurora_enable_vsync(bool enabled);

/* Stereo (OpenXR) controls, lifted from Wiicompiled VR's aurora fork. */

// Live scene interpolation at the headset's display deadlines (retained replay;
// not ported yet, so the getter stays false).
void aurora_set_stereo_frame_interpolation(bool enabled);
bool aurora_get_stereo_frame_interpolation();
void aurora_set_stereo_motion_logging(bool enabled);
// How each immersive eye is replayed (retained for the replay path).
void aurora_set_stereo_stop_at_display_copy(bool enabled);
void aurora_set_stereo_skip_copy_clears(bool enabled);
void aurora_set_stereo_single_pass_eyes(bool enabled);
// Foveated eyes (Quest, fragment density maps): unavailable on the desktop.
void aurora_set_stereo_foveation(uint32_t level);
bool aurora_stereo_foveation_available();
// The virtual screen the immersive frame's 2D layer is folded onto, in metres.
void aurora_set_stereo_hud_screen(bool enabled, float widthMeters, float distanceMeters);
bool aurora_get_stereo_hud_screen_enabled();
// The game picture's width over height, and the desktop snapshot's that Aurora
// letterboxes the picture into. False before the first frame.
bool aurora_get_stereo_screen_aspects(float* pictureAspect, float* snapshotAspect);

typedef enum {
  AURORA_STEREO_MIRROR_NORMAL = 0,
  AURORA_STEREO_MIRROR_BOTH = 1,
  AURORA_STEREO_MIRROR_LEFT = 2,
  AURORA_STEREO_MIRROR_RIGHT = 3,
  AURORA_STEREO_MIRROR_NONE = 4,
} AuroraStereoMirrorView;
// What the desktop window shows while the headset is running.
void aurora_set_stereo_mirror_view(AuroraStereoMirrorView view);

// --- stereo replay draw routes ---------------------------------------------
// How the per-eye replay treats the GX draws that follow the marker
// (AuroraSetStereoDrawRoute in dolphin/gx/GXAurora.h, ordered with the draws
// through the GX FIFO). Orthographic draws are always identical in both eyes.
typedef enum AuroraStereoDrawRoute {
  AURORA_STEREO_ROUTE_WORLD = 0,           // a game camera draw: eye frustum and eye pose composed in
  AURORA_STEREO_ROUTE_HEAD_LOCKED = 1,     // a camera-space draw (HUD), scaled by the head-locked scale
  AURORA_STEREO_ROUTE_SCREEN_2D = 2,       // 2D content (reserved: drawn like FULLSCREEN for now)
  AURORA_STEREO_ROUTE_FULLSCREEN = 3,      // identical in both eyes, game projection untouched
  AURORA_STEREO_ROUTE_PER_EYE_RESOLVE = 4, // reserved: the pass's EFB copy replayed per eye (drawn like WORLD)
  AURORA_STEREO_ROUTE_SKIP = 5,            // not drawn in the eyes
  // HEAD_LOCKED, with its orthographic draws laid on the head-locked plane (AuroraSetStereoHeadLockedPlane in
  // dolphin/gx/GXAurora.h) and its EFB copies taken from each eye through that plane: a 2D window onto the view,
  // such as Metroid Prime's scan visor, keeps pointing where the head does and magnifies each eye's own view.
  AURORA_STEREO_ROUTE_HEAD_LOCKED_2D = 6,
  // A draw attached to the camera that stands for infinity, such as Metroid Prime's sky (a dome of some sixty
  // units centred on the camera, pushed into the far depth range): the eyes see it with the head's rotation
  // only, no eye offset, so it has no disparity and does not shift when the head moves.
  AURORA_STEREO_ROUTE_SKY = 7,
} AuroraStereoDrawRoute;
// AURORA_STEREO_ROUTE_HEAD_LOCKED draws are scaled about the camera origin
// before the eye offset: sizeScale sets their angular size, distanceScale
// their distance (0.75 and 0.75: the HUD at three quarters of its authored
// size and distance). Read at frame begin.
void aurora_set_stereo_head_locked(float sizeScale, float distanceScale);
// Whether immersive packets are replayed per eye (default true). Off, they are
// shown as the mono image on the virtual screen, which is the fail-safe path.
void aurora_set_stereo_immersive_replay(bool enabled);
bool aurora_get_stereo_immersive_replay(void);

#ifdef __cplusplus
}
#endif

#endif
