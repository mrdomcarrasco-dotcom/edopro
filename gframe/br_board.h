#ifndef BR_BOARD_H
#define BR_BOARD_H

// Battle Royal: MSG_SEAT_BOARD (core packet body, after the message id) -> the copy for the seat's own duelist
// and the public copy for everyone else, each starting with the message id. Every block's query is re-encoded
// the way GenericDuel::RefreshLocation does it (owner: hidden-checked, others: public only).
// Body: u8 seat, u8 alive, u8 placement, i32 lp, u32 deck, hand, extra, extra_p_count, grave, banished,
// u8 block count, then per block u8 location, u32 length, query stream (u32 size + one query per card).
// Kept free of server state so battle-royal-tools/br_board_test.cpp checks the exact code.

#include <cstdint>
#include <cstring>
#include <vector>
#include "core_utils.h"

namespace ygo {

#ifndef MSG_SEAT_BOARD
#define MSG_SEAT_BOARD 172
#endif

inline bool BattleRoyalBoardBuffers(const uint8_t* p, size_t len, std::vector<uint8_t>& full, std::vector<uint8_t>& pub) {
	static constexpr size_t header = 1 + 1 + 1 + 4 + 6 * 4; // seat, alive, placement, lp, 6 counts
	if(len < header + 1)
		return false;
	full.assign({ static_cast<uint8_t>(MSG_SEAT_BOARD) });
	full.insert(full.end(), p, p + header + 1);
	pub = full;
	const uint8_t blocks = p[header];
	size_t pos = header + 1;
	for(uint8_t b = 0; b < blocks; ++b) {
		if(pos + 5 > len)
			return false;
		const uint8_t loc = p[pos];
		uint32_t blen;
		std::memcpy(&blen, p + pos + 1, sizeof(blen));
		pos += 5;
		if(pos + blen > len || blen < 4)
			return false;
		std::vector<uint8_t> raw(p + pos, p + pos + blen);
		pos += blen;
		CoreUtils::QueryStream query(raw.data());
		for(auto* out : { &full, &pub }) {
			out->push_back(loc);
			const auto at = out->size();
			out->resize(at + sizeof(uint32_t));
			if(out == &full)
				query.GenerateBuffer(*out, true);
			else
				query.GeneratePublicBuffer(*out);
			const uint32_t written = static_cast<uint32_t>(out->size() - at - sizeof(uint32_t));
			std::memcpy(out->data() + at, &written, sizeof(written));
		}
	}
	return pos == len;
}

}

#endif // BR_BOARD_H
