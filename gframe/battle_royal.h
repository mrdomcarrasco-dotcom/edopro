#ifndef BATTLE_ROYAL_H
#define BATTLE_ROYAL_H

// Battle Royal (DUEL_BATTLE_ROYAL) bookkeeping shared by the server (GenericDuel) and its headless test.
// No EDOPRO dependencies on purpose: the router only knows seats, engine slots and who is a bot.
//
// The core runs a normal 1v1 between engine slots 0 and 1 and swaps whole boards ("seats") in and out
// of them (MSG_SEAT_SWAP). The server must:
//  - send every prompt / private message of slot p to the duelist whose seat sits in slot p,
//  - never send duel messages to a bot whose seat is offstage (WindBot would apply them to a board it
//    does not have),
//  - when a bot's seat comes back, make the bot see a normal 1v1 again: if the bot believes it is the
//    other engine player (its MSG_START "type"), send it a new MSG_START with the right type; pad its
//    turn counter with MSG_NEW_TURN messages it missed; then a MSG_RELOAD_FIELD rebuilds its board.
// Human EDOPRO clients that announced Battle Royal support (CTOS_BR_CLIENT) receive the public view
// while offstage and flip their own perspective on MSG_SEAT_SWAP.

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

namespace ygo {

#ifndef MSG_SEAT_SWAP
#define MSG_SEAT_SWAP            166
#define MSG_SEAT_TURN            167
#define MSG_SEAT_ELIMINATED      168
#endif
#ifndef MSG_SEAT_TARGET
#define MSG_SEAT_TARGET          171
#define MSG_SEAT_BOARD           172
#endif
#ifndef MSG_SEAT_INFO
// server-made (not from the core): u8 your seat (0xff = spectator/replay), u8 seat count,
// then per seat: u16 name[20]; then u8 team battle royal (0/1) and per seat u8 team (lobby side)
#define MSG_SEAT_INFO            169
#endif

class BattleRoyalRouter {
public:
	static constexpr uint8_t NONE = 0xff;
	struct Seat {
		bool bot = true;          // no CTOS_BR_CLIENT: WindBot or a stock client
		uint8_t slot = NONE;      // engine slot the seat sits in, NONE = offstage
		uint8_t view = 0;         // engine player the duelist believes it is (MSG_START type & 0xf)
		bool alive = true;
		uint8_t placement = 0;
		int32_t lp = 0;
		uint32_t turns_seen = 0;  // MSG_NEW_TURN messages the duelist received since its last MSG_START
	};
	// what the server must send to one bot before the MSG_RELOAD_FIELD of a turn start
	struct BotResync {
		uint8_t seat;
		bool send_start;          // new MSG_START with type = start_type
		uint8_t start_type;
		uint32_t fake_turns;      // MSG_NEW_TURN messages to send after the MSG_START (turn counter catch-up)
	};

	std::vector<Seat> seats;
	std::array<uint8_t, 2> slot_seat{ { 0, 1 } };
	uint8_t active_seat = 0, target_seat = 1, turn_slot = 0;
	uint32_t turn_count = 0;      // MSG_NEW_TURN messages so far
	bool dirty = false;           // a seat swap happened since the last MSG_SEAT_TURN

	// seats in core order (seat t + 2*d), views = the MSG_START type each duelist got (its team)
	void Init(const std::vector<bool>& is_bot, const std::vector<uint8_t>& views) {
		seats.assign(is_bot.size(), Seat{});
		for(size_t i = 0; i < seats.size(); ++i) {
			seats[i].bot = is_bot[i];
			seats[i].view = views[i];
		}
		slot_seat = { { 0, 1 } };
		if(seats.size() > 0) seats[0].slot = 0;
		if(seats.size() > 1) seats[1].slot = 1;
		active_seat = 0;
		target_seat = 1;
		turn_slot = 0;
		turn_count = 0;
		dirty = false;
	}
	bool OnStage(uint8_t seat) const {
		return seat < seats.size() && seats[seat].slot != NONE;
	}
	// may this seat's duelist receive duel messages right now?
	bool Receives(uint8_t seat) const {
		return seat < seats.size() && (!seats[seat].bot || seats[seat].slot != NONE);
	}
	uint8_t SeatInSlot(uint8_t slot) const {
		return slot < 2 ? slot_seat[slot] : NONE;
	}
	// MSG_SEAT_SWAP payload (after the message id): u8 slot, u8 seat_in, u8 seat_out|0xff, u8 seat_count, ...
	// returns false on a malformed message
	bool OnSeatSwap(const uint8_t* p, size_t len) {
		if(len < 4)
			return false;
		const uint8_t slot = p[0], in = p[1], out = p[2];
		if(slot > 1 || in >= seats.size())
			return false;
		if(out < seats.size() && seats[out].slot == slot)
			seats[out].slot = NONE;
		const uint8_t prev = slot_seat[slot];
		if(prev < seats.size() && seats[prev].slot == slot)
			seats[prev].slot = NONE;
		seats[in].slot = slot;
		slot_seat[slot] = in;
		if(len >= 8) {
			int32_t lp;
			std::memcpy(&lp, p + 4, sizeof(lp));
			seats[in].lp = lp;
		}
		dirty = true;
		return true;
	}
	// MSG_SEAT_TURN payload: u8 turn slot, u8 active, u8 target, u8 count, u8 alive count,
	// then per seat u8 slot|0xff, u8 alive, u8 placement, i32 lp.
	// Returns what each bot needs before the full reload; reload_field tells whether to send one.
	std::vector<BotResync> OnSeatTurn(const uint8_t* p, size_t len, bool& reload_field) {
		std::vector<BotResync> out;
		reload_field = false;
		if(len < 5)
			return out;
		turn_slot = p[0];
		active_seat = p[1];
		target_seat = p[2];
		const uint8_t count = p[3];
		const uint8_t* q = p + 5;
		for(uint8_t s = 0; s < count && s < seats.size(); ++s) {
			if(static_cast<size_t>(q + 7 - p) > len)
				break;
			const uint8_t slot = q[0];
			seats[s].slot = slot < 2 ? slot : NONE;
			if(slot < 2)
				slot_seat[slot] = s;
			seats[s].alive = q[1] != 0;
			seats[s].placement = q[2];
			std::memcpy(&seats[s].lp, q + 3, sizeof(int32_t));
			q += 7;
		}
		if(!dirty)
			return out;
		dirty = false;
		reload_field = true;
		for(uint8_t s = 0; s < seats.size(); ++s) {
			auto& seat = seats[s];
			if(!seat.bot || seat.slot == NONE)
				continue;
			BotResync r{ s, false, seat.slot, 0 };
			if(seat.view != seat.slot) {
				r.send_start = true;
				seat.view = seat.slot;
				seat.turns_seen = 0;
			}
			r.fake_turns = turn_count - seat.turns_seen;
			seat.turns_seen = turn_count;
			if(r.send_start || r.fake_turns)
				out.push_back(r);
		}
		return out;
	}
	// MSG_NEW_TURN went out: everyone who receives duel messages saw it
	void OnNewTurn() {
		++turn_count;
		for(auto& seat : seats) {
			if(!seat.bot || seat.slot != NONE)
				++seat.turns_seen;
		}
	}
	// MSG_SEAT_ELIMINATED payload: u8 seat, u8 slot, u8 reason, u8 placement, u8 seats left
	void OnEliminated(const uint8_t* p, size_t len) {
		if(len < 4 || p[0] >= seats.size())
			return;
		seats[p[0]].alive = false;
		seats[p[0]].placement = p[3];
	}
	// MSG_WIN's player is the winner's slot
	uint8_t WinnerSeat(uint8_t slot) const {
		return slot < 2 ? slot_seat[slot] : NONE;
	}
};

// Seat order used by the core (field::battle_royal_init): team 0 duelist 0, team 1 duelist 0, then for
// every further duelist index the team 0 one before the team 1 one.
// Returns (team, duelist index) per seat.
inline std::vector<std::array<uint8_t, 2>> BattleRoyalSeatOrder(size_t team0, size_t team1) {
	std::vector<std::array<uint8_t, 2>> order;
	const size_t n = team0 > team1 ? team0 : team1;
	for(size_t d = 0; d < n; ++d) {
		if(d < team0) order.push_back({ { 0, static_cast<uint8_t>(d) } });
		if(d < team1) order.push_back({ { 1, static_cast<uint8_t>(d) } });
	}
	return order;
}

}

#endif // BATTLE_ROYAL_H
