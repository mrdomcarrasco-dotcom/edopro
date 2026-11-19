#ifndef BR_CLIENT_H
#define BR_CLIENT_H

// Battle Royal client side (DUEL_BATTLE_ROYAL): the seat HUD, the table (every seat's board at once), free
// targeting and the BR_AUTOTEST test aid. The engine keeps two seats "on the field" (engine slots 0/1); every
// other seat's board is frozen offstage and arrives as MSG_SEAT_BOARD. The table draws the live seats from the
// field and the offstage ones from their last board, your side at the bottom; teammates' hands stay hidden.

#include <cstddef>
#include <cstdint>
#include <string>
#include <irrlicht.h>

namespace ygo {
class ClientCard;
namespace brc {

// --- duel thread (gMutex held) ---
void OnBoard(const uint8_t* p, size_t len);  // MSG_SEAT_BOARD body (after the message id)
void OnPrompt(uint8_t msg);                    // a prompt arrived: field-picking prompts need the live board
void OnIdle(bool my_turn);                     // MSG_SELECT_IDLECMD while it is our turn
void OnReloadField();                          // MSG_RELOAD_FIELD rebuilt the field
void Reset();                                  // duel start / end

// --- main thread ---
void BeginFrame();                // gMutex held: swap the viewed board into the top half
void EndFrame();                  // ...and back before the lock is released
bool DrawHud(int turn);           // false: not a Battle Royal, draw the normal HUD
bool OnMouse(const irr::SEvent& event); // HUD hover/click; true = consumed
// --- Battle Royal 2x2 field (main thread, gMutex held): EDOPRO's field once per column, your team at the
// bottom and the other two seats across. Every 3D draw of live player p goes through Board(p); the boards of
// the two offstage seats are drawn from their last MSG_SEAT_BOARD by DrawOffstage. Identity when off.
bool Wide();
const irr::core::matrix4& Board(int p);
const irr::core::matrix4& Column(int c);
const irr::core::matrix4& CardMatrix(const ClientCard* c); // your own hand stays full size at the bottom
irr::core::vector3df Point(int p, const irr::core::vector3df& v);
irr::core::vector3df CardPoint(const ClientCard* c, const irr::core::vector3df& v);
bool MapMouse(irr::core::vector3df& pt); // field point -> the space of the live board under it; false: none
void DrawOffstage();
std::wstring DisplayName(uint8_t seat); // "[AI] Kaiba - DSOD B" -> "Kaiba"
bool Viewing();                   // legacy (the 3D field always shows the live board now): false
bool TableOpen();                 // the table is open (or still fading)
void DrawTable();                 // gMutex held: before env->drawAll, so windows open above it
void CoverGui(bool on);           // hide the field buttons / chat under the table while drawAll runs
ClientCard* ViewCard(uint32_t location, uint32_t sequence); // a card of the viewed board under the mouse
bool ShowViewPile(uint32_t location); // card list of the viewed board's GY / banished / Extra Deck
void RequestTarget(uint8_t seat); // our turn: switch the target to this seat
void AutoTest();                  // BR_AUTOTEST=<bot>,<bot>,...: host, spectate, add the bots, start

}
}

#endif // BR_CLIENT_H
