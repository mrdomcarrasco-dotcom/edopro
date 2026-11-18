#ifndef BR_CLIENT_H
#define BR_CLIENT_H

// Battle Royal client side (DUEL_BATTLE_ROYAL): the seat HUD, viewing any seat's board, free targeting
// and the BR_AUTOTEST test aid. The engine keeps two seats "on the field" (engine slots 0/1); every other
// seat's board is frozen offstage and arrives as MSG_SEAT_BOARD. Viewing one of them draws it in the
// top half of the field (read-only) for as long as the view is on; the live cards stay untouched.

#include <cstddef>
#include <cstdint>
#include <irrlicht.h>

namespace ygo {
class ClientCard;
namespace brc {

// --- duel thread (gMutex held) ---
void OnBoard(const uint8_t* p, size_t len);  // MSG_SEAT_BOARD body (after the message id)
void OnPrompt(uint8_t msg);                    // a prompt arrived: field-picking prompts need the live board
void OnIdle(bool my_turn_start);               // MSG_SELECT_IDLECMD / BATTLECMD for us
void OnReloadField();                          // MSG_RELOAD_FIELD rebuilt the field
void Reset();                                  // duel start / end

// --- main thread ---
void BeginFrame();                // gMutex held: swap the viewed board into the top half
void EndFrame();                  // ...and back before the lock is released
bool DrawHud(int turn);           // false: not a Battle Royal, draw the normal HUD
bool OnMouse(const irr::SEvent& event); // HUD hover/click; true = consumed
bool Viewing();                   // the top half shows a frozen board
ClientCard* ViewCard(uint32_t location, uint32_t sequence); // a card of the viewed board under the mouse
bool ShowViewPile(uint32_t location); // card list of the viewed board's GY / banished / Extra Deck
void RequestTarget(uint8_t seat); // our turn: switch the target to this seat
void AutoTest();                  // BR_AUTOTEST=<bot>,<bot>,...: host, spectate, add the bots, start

}
}

#endif // BR_CLIENT_H
