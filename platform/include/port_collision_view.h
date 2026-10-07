#ifndef METROID_PRIME_PORT_PORT_COLLISION_VIEW_H
#define METROID_PRIME_PORT_PORT_COLLISION_VIEW_H

class CGameArea;
class CStateManager;

// Draws what Samus collides with: each visible area's static collision triangles, shaded
// by which way they face and tinted by material, and the boxes of solid actors (gates,
// platforms, blocks) in orange. "Only" draws nothing else of the world but Samus, which
// shows walls that have no surface drawn on them. MP_COLLISION_VIEW=off|overlay|only,
// console `collision`, F1 > Debug > Rendering.
namespace PortCollisionView {

enum class Mode { Off, Overlay, Only };
Mode GetMode();
void SetMode(Mode mode);
const char* ModeName(Mode mode);
bool ParseMode(const char* name, Mode& mode);
inline bool Only() { return GetMode() == Mode::Only; }

// Called by CStateManager::DrawWorld once the world is drawn, with the areas it drew.
void Draw(const CStateManager& mgr, const CGameArea* const* areas, int count);

} // namespace PortCollisionView

#endif
