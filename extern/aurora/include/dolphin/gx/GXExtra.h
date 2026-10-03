#ifndef DOLPHIN_GXEXTRA_H
#define DOLPHIN_GXEXTRA_H
// Extra types for PC
#ifdef TARGET_PC
#include <dolphin/gx/GXStruct.h>
#include <dolphin/types.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  float r;
  float g;
  float b;
  float a;
} GXColorF32;

void GXDestroyTexObj(GXTexObj* obj);
void GXDestroyTlutObj(GXTlutObj* obj);
void GXDestroyCopyTex(void* dest);
// Aurora extension: offsets indexed fetches from an array by `base` elements.
// Stays in effect until changed; GXSetArray does not reset it.
void GXSetArrayBaseIndex(GXAttr attr, u32 base);
// Aurora extension: PBR shading for the following draws (see GX_AURORA_SET_PBR).
void GXSetPBR(GXBool enable);
// Aurora extension: distance-field texturing for the following draws (see
// GX_AURORA_SET_SDF). 0 turns it off.
void GXSetSDF(u8 edge);
// Aurora extension: the PBR environment probe (see GX_AURORA_COPY_PROBE_FACE and
// GX_AURORA_SET_PBR_PROBE).
void GXCopyProbeFace(u32 face);
void GXSetPBRProbe(const f32 viewToProbe[3][3], f32 weight);
// Aurora extension: the emissive multiplier and backlight weight of the following PBR
// draws (see GX_AURORA_SET_PBR_MATERIAL). `heightBlend` above 0 is the threshold of a
// height-blended alpha (0: the base map's alpha is the opacity), and `mode` 1 draws the
// surface's own colour with no lighting, 2 has the base map's alpha mask the glow instead
// of being the opacity, 4 has the vertex colour tint the surface; the sum of those. `layer` is the blend of a second layer (texture maps 4-6:
// base, MR, normal) over the first by the vertex alpha and the two base maps' alphas: the
// width of its edge, then the scale and offset of the first layer's height and of the
// second's. A width of 0 is no second layer. `kind` is one of Remastered's special
// surfaces, a strength and four parameters of it: 1 lays the second layer on what faces
// `up` (world up in view space; the vertex alpha lifts it), 2 has map 4 as a detail map
// multiplied into the base, 3 scales the glow by the vertex alpha (lava), 4 is ice: map 4
// is seen inside the surface, at a depth of the base map's alpha times the fourth
// parameter, through a fresnel of power and weight the first two; the third scales the
// normal map and the strength is the inside's glow.
void GXSetPBRMaterial(const f32 emissive[3], const f32 backlight[3], f32 heightBlend, f32 mode, const f32 layer[5],
                      const f32 kind[6], const f32 up[3]);
// Aurora extension: room cubes (see GX_AURORA_CREATE_PBR_CUBE). `texels` is RGBA16Float,
// every mip of face 0 from the largest down, then face 1 and so on; it is copied.
void GXCreatePBRCube(u32 id, u32 size, u32 mipCount, const void* texels, u32 length);
void GXDestroyPBRCube(u32 id);
// params: exposure, the mip a roughness of 1 samples, the mip the ambient samples along
// the normal, and the scale that takes that sample to 1 for an average direction (0 = the
// ambient is left alone). Id 0, or one never created, selects the probe.
void GXSetPBRCube(u32 id, const f32 params[4]);
// Aurora extension: baked ambient light as a function of the normal n (view space), per
// colour channel c: base[c] + lobe[c] * pow(clamp(0.5 + 0.5 * dot(n, dir[c]), 0, 1), power[c]).
// The rows are base, lobe, power, then the direction of red, green and blue; the
// directions are not unit length (shorter is more even). The luminance of the GX ambient
// colour scales the result. Null goes back to the GX ambient alone.
void GXSetPBRAmbient(const f32 rows[6][3], f32 mode);
// Aurora extension: ambient volumes (see GX_AURORA_CREATE_PBR_VOLUME). `texels` is, for
// every point with x fastest and z slowest: all the means (RGBA16Float), then all the lobes
// (RGBA16Float), then the direction of red, of green and of blue (RGBA8, 0..255 is -1..1
// along the volume's axes, with that channel's sharpness in alpha); 28 bytes a point.
void GXCreatePBRVolume(u32 id, u32 sizeX, u32 sizeY, u32 sizeZ, const void* texels, u32 length);
void GXDestroyPBRVolume(u32 id);
// Rows 0 to 2 take a view-space position (w: the offset) to the volume's texture
// coordinates, rows 3 to 5 a view-space normal to the volume's axes. w of row 3 scales the
// light (0: no volume), w of row 4 is how far along the normal the sample is taken.
void GXSetPBRVolume(u32 id, const f32 rows[6][4]);
// Aurora extension: what PBR surfaces drawn from now on show, for debugging: 0 the shaded
// result, 1 base colour, 2 normal (view space), 3 roughness, 4 metalness, 5 occlusion,
// 6 the diffuse ambient, 7 the reflection, 8 the glow, 9 the lit level in stops around
// middle grey (blue under, red over), 10 the special surface's kind. Not a FIFO command:
// it rides with the next GXSetPBRMaterial.
void GXSetPBRDebugView(u32 view);
// Aurora extension: a three-piece tone curve over the lit colour x, which is taken as
// already exposed (see GX_AURORA_SET_PBR_TONE). Row 0 is the toe, (a x + b) x^2 + c x below
// z of row 1; row 1 the line S x + y0 (x, y) from there to its w; row 2 the shoulder
// x t / (1 + t) + w with t = y x + z. Null, or a slope of 0, is no curve.
void GXSetPBRTone(const f32 rows[3][4]);

void GXColor4f32(float r, float g, float b, float a);

#ifdef __cplusplus
}
#endif
#endif

#endif
