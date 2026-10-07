#ifndef AURORA_GFX_H
#define AURORA_GFX_H

#ifdef __cplusplus
#include <cstddef>
#include <cstdint>

extern "C" {
#else
#include "stddef.h"
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
  uint32_t renderPassCount; // render passes the frame's EFB recording encoded
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
// The MiB set aside for AuroraConfig::residentGeometryMiB: the device may allow less.
uint32_t aurora_get_resident_geometry_mib();
// The bytes of it in use, as of the last frame processed.
uint64_t aurora_get_resident_geometry_used();
float aurora_get_fps();
// Whether the device samples BC and ASTC 4x4 compressed textures (false until it exists).
void aurora_get_texture_support(bool* bc, bool* astc);
// A GX texture object's (GXTexObj*) base level decoded to RGBA8 into `out` (cap bytes); false for
// palette or compressed PC formats and when it does not fit.
bool aurora_gx_texobj_rgba8(const void* obj, uint32_t* width, uint32_t* height, uint8_t* out, size_t cap);

void aurora_enable_vsync(bool enabled);

/* Stereo (OpenXR) controls, lifted from Wiicompiled VR's aurora fork. */

// Live scene interpolation at the headset's display deadlines (retained replay;
// not ported yet, so the getter stays false).
void aurora_set_stereo_frame_interpolation(bool enabled);
bool aurora_get_stereo_frame_interpolation();
void aurora_set_stereo_motion_logging(bool enabled);
// GX draw commands issued so far (GXBegin and display list calls), a running
// count the game samples around a section to know what it draws.
uint32_t aurora_gx_draw_commands_issued(void);
// How each immersive eye is replayed (retained for the replay path).
void aurora_set_stereo_stop_at_display_copy(bool enabled);
void aurora_set_stereo_skip_copy_clears(bool enabled);
void aurora_set_stereo_single_pass_eyes(bool enabled);
// Fixed foveated rendering of the immersive eyes: 0 off, 1 low, 2 medium, 3
// high (gfx/foveation.hpp's rings; menus on the virtual screen never are).
// Live, but the Vulkan device only gets fragment density maps when
// AuroraConfig::xrFragmentDensityMap asked for them at creation (the Quest
// build's patched Dawn); aurora_stereo_foveation_available says whether this
// session has them. Unavailable on the desktop.
void aurora_set_stereo_foveation(uint32_t level);
uint32_t aurora_get_stereo_foveation();
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
  // A 2D layout drawn for the whole picture (Metroid Prime's morph ball HUD): its mono picture, perspective and
  // orthographic draws alike, is laid on the virtual screen (aurora_set_stereo_screen_2d) hung ahead in the game
  // camera's space, so its corners stay in sight and the head looks around it. Drawn like FULLSCREEN without one.
  AURORA_STEREO_ROUTE_SCREEN_2D = 2,
  AURORA_STEREO_ROUTE_FULLSCREEN = 3,      // identical in both eyes, game projection untouched
  AURORA_STEREO_ROUTE_PER_EYE_RESOLVE = 4, // reserved: the pass's EFB copy replayed per eye (drawn like WORLD)
  // Not drawn in the eyes. An EFB copy made under it is taken from the mono EFB only and the eyes sample that copy:
  // an image that is not a view, such as a shadow rendered from a light. When that copy also clears the whole EFB,
  // its render pass is not replayed into the eyes at all, since nothing could see it there.
  AURORA_STEREO_ROUTE_SKIP = 5,
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
// The virtual screen AURORA_STEREO_ROUTE_SCREEN_2D draws are laid on: widthMeters across and distanceMeters
// ahead of the game camera, its height following the picture's aspect, the game drawing unitsPerMeter units to
// the metre. A zero turns it off. Read at frame begin.
void aurora_set_stereo_screen_2d(float widthMeters, float distanceMeters, float unitsPerMeter);
// Whether immersive packets are replayed per eye (default true). Off, they are
// shown as the mono image on the virtual screen, which is the fail-safe path.
void aurora_set_stereo_immersive_replay(bool enabled);
bool aurora_get_stereo_immersive_replay(void);
// Whether an immersive frame draws both eyes in one Vulkan multiview render pass
// when the device can (default true). Read at frame begin.
void aurora_set_stereo_multiview(bool enabled);
// The device can (PrimedGun's patched Dawn on Vulkan, with the OpenXR interop).
bool aurora_get_stereo_multiview_available(void);
// Whether the FIFO processor resolves indexed vertex attributes into direct ones
// before the upload (default false): one fetch per attribute in the vertex shader
// instead of an index and a dependent array fetch, which stalls the Quest's GPU,
// for more CPU work per vertex. Takes effect at the next frame.
void aurora_set_gx_deindex_vertices(bool enabled);

#ifdef __cplusplus
}
#endif

#endif
