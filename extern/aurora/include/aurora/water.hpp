#pragma once

#include <cstdint>

#include <dolphin/gx.h>

// Port extension: Remastered's water surface (CWaterSceneNode, WaterRenderVolume), drawn
// natively. The vertex and pixel shaders are Remastered's (_fv_00dddc8 and its TOP/BOTTOM
// pixel shaders, build/mpr/water/A-shader.md, D-rain.md): the mesh is displaced by the flow
// maps and two Gerstner-like waves, lit by the room's ambient volume and probe cube, rippled
// by rain, and fogged by the volumetric fog's froxels. Its blend (dst = o0 + dst * (1 - o1),
// a per-channel transmittance) is done in the shader against a snapshot of the scene, which
// also gives it the depth behind the surface for its depth fades.
namespace aurora::gfx::water {
struct Vertex {
  float pos[3];     // Remastered's model space (y up)
  float uv[2];      // TEXCOORD_0
  uint8_t color[4]; // COLOR, RGBA: rgb scales the waves per vertex
};
static_assert(sizeof(Vertex) == 24);

enum class Cull : uint8_t { None, Front, Back };

struct DrawDesc {
  bool bottom = false; // the camera is under the surface (the BOTTOM pixel shader)
  Cull cull = Cull::None; // which faces go (counter-clockwise in window space is the front)
  bool rain = false;   // the rain ripples (needs rainNoise)
  const GXTexObj* normalMap = nullptr;  // null: flat
  const GXTexObj* sourceFlow = nullptr; // null: no flow (xy 0.5, z 0, w 1)
  const GXTexObj* rainNoise = nullptr;
  // Remastered's uniforms: uc_surface S[0..7], the rain R[0..2], uc_waveParams W[0..2]
  // (water::SetupSurfaceParamsData, SetupWaveSim).
  float surface[8][4] = {};
  float rainParams[3][4] = {};
  float waves[3][4] = {};
  // The probe cube (probe::cube_view id, 0 for none) and its texels' scale, mips, and the
  // rows of view direction -> cube direction.
  uint32_t cube = 0;
  float cubeScale = 0.f; // the cube's texels times this; the TOP shader also times the volume's max
  float cubeMips = 0.f;  // the mip read at roughness 1
  float viewToCube[3][4] = {};
  // The ambient volume (probe::create_volume id, 0 for none): rows of view position ->
  // texture coordinates, and what its mean is multiplied by (rgb; w for its largest channel).
  // Without a volume, `fallbackMean` is the mean.
  uint32_t volume = 0;
  float viewToVolume[3][4] = {};
  float volumeScale[4] = {};
  float fallbackMean[4] = {};
  // The tone curve the EFB is drawn through (as GXSetPBRTone; w of row 0 is ignored).
  float tone[3][4] = {};
  // The projection's near and far, and the GX depth range the world draws in.
  float zNear = 1.f, zFar = 1000.f, zMin = 0.f, zMax = 1.f;
};

// Records the surface into the current pass with the projection and the current position
// matrix the GX state holds at this call (view * model, model = Remastered's mesh space ->
// world; the normal matrix is its inverse transpose).
// Breaks the pass first to snapshot its colour and depth. Indices are a triangle list.
void draw(const DrawDesc& desc, const Vertex* verts, uint32_t vertexCount, const uint32_t* indices,
          uint32_t indexCount);
void shutdown();
} // namespace aurora::gfx::water
