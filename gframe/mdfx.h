#ifndef MDFX_H
#define MDFX_H

// Progression "duel polish" layer: eased card movement, chain-link stamps, resolve flashes,
// summon shockwaves, damage shake and full-screen cut-ins for the Egyptian Gods, Exodia and
// other listed cards. Everything is drawn from textures generated in code (no image files).
// Triggers are called from the duel thread while it holds mainGame->gMutex; Update/Draw run on
// the main thread under the same mutex.

#include <cstdint>
#include <irrlicht.h>

namespace irr { namespace gui { class CGUITTFont; } }

namespace mdfx {

enum class Cutin : uint8_t { NONE, OBELISK, SLIFER, RA, EXODIA_WIN, ACE, ORICHALCOS };

void Init(irr::video::IVideoDriver* driver, irr::gui::CGUITTFont* bannerFont, irr::gui::CGUITTFont* numberFont);
bool Enabled();

// easing used by ClientCard movement (t in 0..1)
float EaseMove(float t);
// how high a moving card lifts (world units) for a move of this length
float MoveLift(float distance);
// card moves run this much longer than stock EDOPRO
float MoveTimeScale();

// --- triggers (duel thread, gMutex held) ---
Cutin CutinFor(uint32_t code);
// starts a cut-in; returns its length in 60 fps frames so the caller can WaitFrameSignal on it
int StartCutin(Cutin kind, uint32_t code, const irr::core::vector3df& fieldPos, bool opponent);
bool CutinActive();
void ChainStamp(const irr::core::vector3df& pos, int link, bool opponent);
void ResolveFlash(const irr::core::vector3df& pos, int link);
void SummonImpact(const irr::core::vector3df& pos, int level, bool special);
void Damage(int amount, bool self);
void Shake(float pixels, float ms);
void Clear();

// --- main thread ---
void Update(uint32_t delta_ms);
irr::core::vector3df CameraOffset();
void Draw(irr::scene::ISceneManager* smgr);

}

#endif
