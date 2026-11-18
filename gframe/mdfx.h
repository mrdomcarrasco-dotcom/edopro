#ifndef MDFX_H
#define MDFX_H

// Progression "duel polish" layer for EDOPRO, timed against a frame-by-frame study of Master Duel's
// presentation (yugioh-progression docs/MD-STUDY.md), drawn entirely with our own procedural art:
// eased card moves, a draw presentation, land-and-settle summons, activation dim + glow, CHAIN / RESOLVE
// displays with a linked card stack, phase and turn bands, crimson damage frame, 1 s LP roll,
// flash-shatter-dust destroys, 1.4 s ace/god summon scenes and the Exodia win sequence.
// Triggers are called from the duel thread while it holds mainGame->gMutex; Update/Draw run on the
// main thread under the same mutex. "effects off" in mdfx.conf restores stock EDOPRO.

#include <cstdint>
#include <string>
#include <vector>
#include <irrlicht.h>

namespace irr { namespace gui { class CGUITTFont; } }

namespace mdfx {

enum class Cutin : uint8_t { NONE, OBELISK, SLIFER, RA, ACE, ORICHALCOS };

struct Fonts {
	irr::gui::CGUITTFont* plate = nullptr; // name plate (~30 px)
	irr::gui::CGUITTFont* big = nullptr;   // CHAIN / RESOLVE / banners (~56 px)
	irr::gui::CGUITTFont* statline = nullptr; // ATK / DEF line (~18 px)
	irr::gui::CGUITTFont* num = nullptr;   // chain numerals (EDOPRO's lpcFont)
	irr::gui::CGUITTFont* lp = nullptr;    // HUD life points (number font, ~34 px)
	irr::gui::CGUITTFont* name = nullptr;  // HUD duelist names (~15 px)
	irr::gui::CGUITTFont* tag = nullptr;   // small caps tags (~11 px, display face)
	irr::gui::CGUITTFont* small = nullptr; // small plain text (~12 px, text face)
};
void Init(irr::video::IVideoDriver* driver, const Fonts& fonts);
bool Enabled();

// card movement
float EaseMove(float t);
float MoveLift(float distance);
float MoveTimeScale();
int LpRollFrames();
// a card that lands oversized and settles into its zone
void Pop(float& from, float& t, float& dur, float scale, float ms);
float PopScale(float from, float t, float dur);

// --- triggers (duel thread, gMutex held); the int ones return 60 fps frames to WaitFrameSignal on ---
Cutin CutinFor(uint32_t code);
int StartCutin(Cutin kind, uint32_t code, bool opponent);
int ExodiaWin();
// duel end: VICTORY / DEFEAT title over rays (outcome 1 = you win, 2 = you lose); 0 = not shown, use stock text
int Result(int outcome, const std::wstring& reason);
void Activate(const irr::core::vector3df& pos, bool monster);
int ChainDisplay(const std::vector<uint32_t>& codes);
int ResolveDisplay(const std::vector<uint32_t>& codes, int resolving, bool first);
void ResolveGlow(const irr::core::vector3df& pos);
void SummonFlash(const irr::core::vector3df& pos, bool special);
void Damage(int amount, bool self, const irr::core::vector3df& at);
int Destroy(const irr::core::vector3df& pos);
int PhaseBanner(const std::wstring& text, bool myTurn, bool turnChange);
void Clear();

// --- duel HUD (replaces the stock LP bars when the Progression art is installed) ---
struct HudInfo {
	int lp[2]{};
	int startlp = 8000;
	std::wstring name[2];
	bool myTurn = true;
	int turn = 0;
};
bool DrawHud(const HudInfo& info); // false -> caller draws the stock LP bars
// dark gold-edged backdrop for small field text (ATK/DEF, pile counts); no-op when effects are off
void Pill(const irr::core::recti& r);
// the big LP change number ("-3000", "+500") as title-art digits; false -> caller draws EDOPRO's text
bool LpNumber(const std::wstring& text, int player, int framesLeft);

// --- Battle Royal HUD (replaces DrawHud when a Battle Royal is on) ---
struct SeatChip {
	std::wstring name;
	int lp = 0;
	uint32_t hand = 0, deck = 0, extra = 0, grave = 0, banished = 0;
	bool alive = true;
	int placement = 0;
	bool turn = false;      // the turn player
	bool target = false;    // the turn player's current target
	bool onField = false;   // sits in the top half of the field right now (live)
	bool viewing = false;   // the top half shows this seat's frozen board
	bool hovered = false;
	bool canTarget = false; // it is our turn and we may switch our target to this seat
	bool targetHover = false;
	bool isYou = false;
	int team = -1;          // team battle royal: 0 / 1, else -1
	int colorIndex = 0;     // per-seat accent in a free-for-all
};
struct SeatHud {
	int lp = 0;             // bottom plate (you, or the seat at the bottom when spectating)
	int startlp = 8000;
	std::wstring name;
	bool bottomTurn = false;
	int turn = 0;
	int bottomTeam = -1;
	std::vector<SeatChip> chips;
	// centre-line status pill: live = "LIVE · <name>", else "VIEWING <name> · frozen" + Back to live
	bool viewing = false;
	std::wstring fieldName;
	bool backHover = false;
	bool targetHint = false; // our turn just started: pulse the seats we can target
};
struct SeatHudRects {
	std::vector<irr::core::recti> chip, target;
	irr::core::recti back;
};
bool DrawSeatHud(const SeatHud& in, SeatHudRects& out);
// short notice under the HUD ("Kaiba targets Joey", "You are under attack", eliminations); kind 0 info, 1 you, 2 out
void SeatToast(const std::wstring& text, int kind);

// --- test aids (environment variables, off unless set) ---
// MDFX_AUTOPUZZLE=<puzzle path>: start that puzzle from the main menu. MDFX_CAPTURE=<dir>: write the
// rendered duel frames there as PNGs every MDFX_CAPTURE_MS (default 100) ms.
const char* AutoPuzzle();
void DevCapture(bool inDuel);
void DevDemo(bool inDuel); // MDFX_DEMO_CHAIN=<codes>: play CHAIN / RESOLVE once the duel is idle

// --- main thread ---
void Update(uint32_t delta_ms);
irr::core::vector3df CameraOffset();
void Draw(irr::scene::ISceneManager* smgr);

}

#endif
