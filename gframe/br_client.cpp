#include "br_client.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "game.h"
#include "game_config.h"
#include "client_card.h"
#include "client_field.h"
#include "duelclient.h"
#include "data_manager.h"
#include "core_utils.h"
#include "common.h"
#include "mdfx.h"
#include "fmt.h"
#include "windbot.h"
#include "windbot_panel.h"

namespace ygo {
namespace brc {

namespace {
constexpr uint8_t NONE = 0xff;

std::vector<ClientCard*> pool;         // cards made for pile lists of offstage seats, freed in Reset
std::atomic<bool> force_live{ false }; // duel thread -> main thread: a prompt needs the field, close the table
bool animate_top = false;              // the live top half changed (gMutex)
uint8_t last_top = NONE;               // seat in the top half at the last field reload
uint32_t hint_until = 0;               // our turn started: pulse the seats we can attack until then
int last_idle_turn = -1;               // turn of our last idle prompt (gMutex)
uint32_t board_version[4]{};           // bumped by every MSG_SEAT_BOARD (gMutex)

// top bar input (main thread)
mdfx::SeatHudRects rects;
std::vector<uint8_t> chip_seat;
int hover_chip = -1;
bool hover_table_btn = false;

// the table (main thread)
bool table_open = false;
float table_anim = 0.f;
uint32_t last_ms = 0;
int focus_seat = -1;                   // seat opened from its plate: highlighted for a moment
uint32_t focus_until = 0;
std::vector<mdfx::TableHit> hits;      // last frame's hit areas
std::vector<uint8_t> table_seat;       // table index -> seat
int hover_hit = -1, hover_board = -1;
uint32_t shown_code = 0;

// an offstage seat's frozen board, read once per MSG_SEAT_BOARD
struct Frozen {
	uint32_t version = ~0u;
	mdfx::TableSeat seat;
	std::vector<uint32_t> grave, banished, extra; // codes, bottom first (0 = face-down)
};
Frozen frozen[4];

struct GuiCover { irr::gui::IGUIElement* el; bool was; };
std::vector<GuiCover> covered;

DuelInfo::BattleRoyal& BR() { return mainGame->dInfo.br; }
uint32_t Now() { return mainGame->device ? mainGame->device->getTimer()->getRealTime() : 0; }

ClientCard* NewCard(uint8_t loc, uint32_t seq) {
	auto* c = new ClientCard{};
	c->controler = 1;
	c->location = loc;
	c->sequence = seq;
	c->position = POS_FACEDOWN;
	c->curAlpha = 255;
	pool.push_back(c);
	return c;
}

// seat in the bottom / top half of the field right now
uint8_t BottomSeat() { return BR().slot_seat[mainGame->LocalPlayer(0) & 1]; }
uint8_t TopSeat() { return BR().slot_seat[mainGame->LocalPlayer(1) & 1]; }
// the seat the HUD and the table are drawn for: you, or the bottom seat when spectating
uint8_t MeSeat() { return BR().my_seat < 4 ? BR().my_seat : (BottomSeat() & 3); }

bool Offstage(uint8_t seat) {
	const auto& br = BR();
	return seat < br.count && seat < 4 && br.seats[seat].slot > 1;
}
// field index (0 bottom, 1 top) of a seat on the field, -1 offstage
int FieldIndex(uint8_t seat) {
	if(Offstage(seat)) return -1;
	if(seat == (BottomSeat() & 3)) return 0;
	if(seat == (TopSeat() & 3)) return 1;
	return -1;
}
bool Allies(uint8_t a, uint8_t b) {
	const auto& br = BR();
	return br.teams && a < 4 && b < 4 && a != b && br.seats[a].team == br.seats[b].team;
}

// "[AI] Kaiba - DSOD B" -> "Kaiba" and, from the bot list, the full deck name "DSOD Blue-Eyes"
struct Names { std::wstring raw, name, deck; };
std::map<std::wstring, Names> name_cache;
const Names& SplitName(const std::wstring& raw) {
	auto it = name_cache.find(raw);
	if(it != name_cache.end())
		return it->second;
	Names n;
	n.raw = raw;
	std::wstring rest = raw;
	const bool bot = rest.rfind(L"[AI] ", 0) == 0 || rest.rfind(L"[AI]", 0) == 0;
	if(bot)
		rest = rest.substr(rest[4] == L' ' ? 5 : 4);
	std::wstring full = rest;
	if(bot && !rest.empty()) {
		// the lobby cuts names at 20 characters: the bot whose name starts with what is left is the one
		for(const auto& b : mainGame->gBot.bots)
			if(b.name.rfind(rest, 0) == 0) { full = b.name; break; }
	}
	const auto dash = full.find(L" - ");
	n.name = dash == std::wstring::npos ? full : full.substr(0, dash);
	if(dash != std::wstring::npos)
		n.deck = full.substr(dash + 3);
	if(n.name.empty())
		n.name = raw;
	return name_cache.emplace(raw, std::move(n)).first->second;
}
std::wstring SeatName(uint8_t s) { return s < 4 ? SplitName(BR().seats[s].name).name : L""; }

bool CanTarget(uint8_t seat) {
	const auto& br = BR();
	if(!br.active || br.my_seat >= 4 || br.active_seat != br.my_seat || seat >= br.count)
		return false;
	if(br.seats[br.my_seat].slot > 1 || DuelClient::answered)
		return false;
	const auto msg = mainGame->dInfo.curMsg;
	if(msg != MSG_SELECT_IDLECMD && msg != MSG_SELECT_BATTLECMD)
		return false;
	if(seat == br.active_seat || seat == br.target_seat || !br.seats[seat].alive)
		return false;
	if(br.teams && br.seats[seat].team == br.seats[br.my_seat].team)
		return false;
	return true;
}

mdfx::TableCard FromCard(const ClientCard* c, bool monsterZone) {
	mdfx::TableCard t;
	if(!c) return t;
	t.present = true;
	t.code = c->code;
	t.faceDown = !(c->position & POS_FACEUP);
	t.defense = monsterZone && (c->position & POS_DEFENSE);
	t.monster = monsterZone;
	t.atk = c->attack;
	t.def = c->defense;
	t.linkMon = (c->type & TYPE_LINK) != 0;
	t.link = static_cast<int>(c->link);
	t.materials = static_cast<int>(c->overlayed.size());
	return t;
}
mdfx::TableCard PileTop(const ClientCard* c) {
	mdfx::TableCard t = FromCard(c, false);
	t.faceDown = t.code == 0;
	return t;
}

// a seat on the field: straight from the live lists
void FromLive(int p, uint8_t seat, mdfx::TableSeat& t) {
	auto& d = mainGame->dField;
	const bool mine = seat == BR().my_seat;
	for(size_t i = 0; i < 7 && i < d.mzone[p].size(); ++i)
		t.mz[i] = FromCard(d.mzone[p][i], true);
	for(size_t i = 0; i < 6 && i < d.szone[p].size(); ++i)
		t.sz[i] = FromCard(d.szone[p][i], false);
	t.hand.clear();
	for(auto* c : d.hand[p]) {
		mdfx::TableCard h;
		h.present = true;
		h.code = (mine && c) ? c->code : 0; // anime rule: nobody sees a teammate's hand
		t.hand.push_back(h);
	}
	t.deck = static_cast<uint32_t>(d.deck[p].size());
	t.extra = static_cast<uint32_t>(d.extra[p].size());
	t.extraUp = static_cast<uint32_t>(std::max(0, d.extra_p_count[p]));
	t.grave = static_cast<uint32_t>(d.grave[p].size());
	t.banished = static_cast<uint32_t>(d.remove[p].size());
	t.graveTop = d.grave[p].empty() ? mdfx::TableCard{} : PileTop(d.grave[p].back());
	t.banishTop = d.remove[p].empty() ? mdfx::TableCard{} : PileTop(d.remove[p].back());
	t.lp = mainGame->dInfo.lp[p];
	t.known = true;
}

// MSG_SEAT_BOARD body -> a table seat. Layout: u8 seat, u8 alive, u8 placement, i32 lp, u32 deck, hand, extra,
// extra_p_count, grave, banished, u8 blocks, per block u8 location, u32 length, query stream.
void FromBoard(uint8_t seat, Frozen& f) {
	auto& t = f.seat;
	t = mdfx::TableSeat{};
	f.grave.clear(); f.banished.clear(); f.extra.clear();
	const auto& b = BR().seats[seat].board;
	if(b.size() < 32) { t.known = false; return; }
	const uint8_t* p = b.data();
	uint32_t deck, hand, extra, extra_p, grave, banished;
	std::memcpy(&deck, p + 7, 4);
	std::memcpy(&hand, p + 11, 4);
	std::memcpy(&extra, p + 15, 4);
	std::memcpy(&extra_p, p + 19, 4);
	std::memcpy(&grave, p + 23, 4);
	std::memcpy(&banished, p + 27, 4);
	t.deck = deck; t.extra = extra; t.extraUp = extra_p; t.grave = grave; t.banished = banished;
	std::vector<mdfx::TableCard> graveCards, banCards;
	size_t pos = 31;
	const uint8_t blocks = p[pos++];
	for(uint8_t k = 0; k < blocks; ++k) {
		if(pos + 5 > b.size()) break;
		const uint8_t loc = p[pos];
		uint32_t len;
		std::memcpy(&len, p + pos + 1, 4);
		pos += 5;
		if(pos + len > b.size() || len < 4) break;
		CoreUtils::QueryStream stream{ p + pos, false, len, mainGame->dInfo.legacy_race_size };
		pos += len;
		uint32_t seq = 0;
		for(const auto& q : stream.GetQueries()) {
			const uint32_t s = seq++;
			if(q.IsEmpty())
				continue;
			std::unique_ptr<ClientCard> c(new ClientCard{});
			c->controler = 1;
			c->location = loc;
			c->sequence = s;
			c->position = POS_FACEDOWN;
			std::vector<std::unique_ptr<ClientCard>> mats;
			for(size_t o = 0; o < q.OverlayCount(); ++o) {
				mats.emplace_back(new ClientCard{});
				c->overlayed.push_back(mats.back().get());
			}
			c->UpdateInfo(q);
			switch(loc) {
			case LOCATION_MZONE: if(s < 7) t.mz[s] = FromCard(c.get(), true); break;
			case LOCATION_SZONE: if(s < 6) t.sz[s] = FromCard(c.get(), false); break;
			case LOCATION_HAND: { mdfx::TableCard h; h.present = true; h.code = c->code; t.hand.push_back(h); break; }
			case LOCATION_GRAVE: f.grave.push_back(c->code); graveCards.push_back(PileTop(c.get())); break;
			case LOCATION_REMOVED: f.banished.push_back((c->position & POS_FACEUP) ? c->code : 0); banCards.push_back(PileTop(c.get())); break;
			case LOCATION_EXTRA: f.extra.push_back((c->position & POS_FACEUP) || seat == BR().my_seat ? c->code : 0); break;
			default: break;
			}
			c->overlayed.clear();
		}
	}
	// a hand block missing (or shorter than the count): card backs for the rest
	while(t.hand.size() < hand) { mdfx::TableCard h; h.present = true; t.hand.push_back(h); }
	if(seat != BR().my_seat)
		for(auto& h : t.hand) h.code = 0;
	if(!graveCards.empty()) t.graveTop = graveCards.back();
	if(!banCards.empty()) t.banishTop = banCards.back();
	t.known = true;
}

// every seat for the table, your side first (near row), the rest across the table
void BuildTable(mdfx::TableView& v) {
	auto& br = BR();
	const auto& di = mainGame->dInfo;
	const uint8_t me = MeSeat();
	v = mdfx::TableView{};
	v.startlp = di.startlp;
	v.turn = di.turn;
	v.teams = br.teams;
	std::vector<uint8_t> nearS, farS;
	const uint8_t n = std::min<uint8_t>(br.count, 4);
	if(br.teams) {
		nearS.push_back(me);
		for(uint8_t s = 0; s < n; ++s)
			if(s != me) (br.seats[s].team == br.seats[me].team ? nearS : farS).push_back(s);
	} else if(n == 4) {
		nearS = { me, static_cast<uint8_t>((me + 1) % 4) };
		farS = { static_cast<uint8_t>((me + 3) % 4), static_cast<uint8_t>((me + 2) % 4) };
	} else {
		nearS.push_back(me);
		for(uint8_t s = 0; s < n; ++s)
			if(s != me) farS.push_back(s);
	}
	table_seat.clear();
	auto add = [&](uint8_t s, bool far) {
		const auto& seat = br.seats[s];
		mdfx::TableSeat t;
		const int fi = FieldIndex(s);
		if(fi >= 0) {
			FromLive(fi, s, t);
		} else {
			auto& f = frozen[s];
			if(f.version != board_version[s]) {
				FromBoard(s, f);
				f.version = board_version[s];
			}
			t = f.seat;
			t.lp = seat.lp;
			t.known = t.known && seat.seen;
		}
		const auto& nm = SplitName(seat.name);
		t.name = nm.name;
		t.deck = nm.deck;
		t.team = br.teams ? seat.team : -1;
		t.colorIndex = s;
		t.placement = seat.placement;
		t.alive = seat.alive;
		t.turn = s == br.active_seat;
		t.target = s == br.target_seat;
		t.isYou = s == br.my_seat;
		t.ally = Allies(s, me) && !t.isYou;
		t.canTarget = CanTarget(s);
		t.far = far;
		table_seat.push_back(s);
		v.seats.push_back(std::move(t));
	};
	for(auto s : nearS) add(s, false);
	for(auto s : farS) add(s, true);
	if(br.active_seat < 4 && br.target_seat < 4)
		v.caption = epro::format(L"TURN {}  ·  {}  ▸  {}", di.turn, SeatName(br.active_seat), SeatName(br.target_seat));
	v.open = table_anim;
	v.hoverSeat = hover_board;
	v.hoverHit = hover_hit;
	if(focus_seat >= 0 && Now() < focus_until)
		for(size_t i = 0; i < table_seat.size(); ++i)
			if(table_seat[i] == focus_seat) v.focusSeat = static_cast<int>(i);
}

void OpenTable(int focus) {
	table_open = true;
	focus_seat = focus;
	focus_until = Now() + 900;
}
void CloseTable() {
	table_open = false;
	hover_hit = hover_board = -1;
}

// a pile of any seat in the card list window
void ShowPile(uint8_t seat, uint32_t location) {
	auto& d = mainGame->dField;
	int loc_id = 0;
	switch(location) {
	case LOCATION_GRAVE: loc_id = 1004; break;
	case LOCATION_REMOVED: loc_id = 1005; break;
	case LOCATION_EXTRA: loc_id = 1006; break;
	default: return;
	}
	d.display_cards.clear();
	const int fi = FieldIndex(seat);
	if(fi >= 0) {
		const auto& list = location == LOCATION_GRAVE ? d.grave[fi] : location == LOCATION_REMOVED ? d.remove[fi] : d.extra[fi];
		d.display_cards.assign(list.crbegin(), list.crend());
	} else {
		const auto& f = frozen[seat];
		const auto& codes = location == LOCATION_GRAVE ? f.grave : location == LOCATION_REMOVED ? f.banished : f.extra;
		for(auto it = codes.crbegin(); it != codes.crend(); ++it) {
			auto* c = NewCard(static_cast<uint8_t>(location), static_cast<uint32_t>(codes.crend() - it - 1));
			c->code = *it;
			c->position = *it ? POS_FACEUP : POS_FACEDOWN;
			d.display_cards.push_back(c);
		}
	}
	if(d.display_cards.empty())
		return;
	mainGame->wCardDisplay->setText(epro::format(L"{} · {}({})", SeatName(seat), gDataManager->GetSysString(loc_id), d.display_cards.size()).data());
	d.ShowLocationCard();
}

// the innermost hit under the mouse (cards and buttons sit inside boards)
int HitAt(const irr::core::vector2di& m) {
	int best = -1;
	for(int i = 0; i < static_cast<int>(hits.size()); ++i) {
		if(!hits[i].rect.isPointInside(m))
			continue;
		if(best < 0 || hits[i].kind != mdfx::TableHit::BOARD)
			best = i; // later entries are drawn above earlier ones
	}
	return best;
}
bool InTable(const irr::core::vector2di& m) {
	return table_anim > 0.01f && mdfx::TableArea().isPointInside(m);
}
}

// ------------------------------------------------------------------------------------- duel thread
void OnBoard(const uint8_t* p, size_t len) {
	auto& br = BR();
	if(len < 32)
		return;
	const uint8_t seat = p[0];
	if(seat >= 4)
		return;
	br.active = true;
	auto& s = br.seats[seat];
	s.board.assign(p, p + len);
	s.seen = true;
	std::memcpy(&s.lp, p + 3, 4);
	std::memcpy(&s.deck, p + 7, 4);
	std::memcpy(&s.hand, p + 11, 4);
	std::memcpy(&s.extra, p + 15, 4);
	std::memcpy(&s.grave, p + 23, 4);
	std::memcpy(&s.removed, p + 27, 4);
	++board_version[seat];
}

void OnPrompt(uint8_t msg) {
	// these prompts pick cards/zones on the field or open a window: close the table
	switch(msg) {
	case MSG_SELECT_IDLECMD:
	case MSG_SELECT_BATTLECMD:
	case MSG_ROCK_PAPER_SCISSORS:
		return;
	default:
		if(BR().active)
			force_live = true;
	}
}

void OnIdle(bool my_turn) {
	// only the first idle of each of our turns (reset with the duel)
	const bool my_turn_start = my_turn && last_idle_turn != mainGame->dInfo.turn;
	if(my_turn_start)
		last_idle_turn = mainGame->dInfo.turn;
	if(my_turn_start && BR().active && !mainGame->dInfo.isCatchingUp) {
		hint_until = Now() + 3500;
		const auto& br = BR();
		uint8_t n = 0; // other opponents we could switch to
		for(uint8_t s = 0; s < br.count && s < 4 && br.my_seat < 4; ++s)
			n += s != br.my_seat && s != br.target_seat && br.seats[s].alive && !(br.teams && br.seats[s].team == br.seats[br.my_seat].team);
		if(n)
			mdfx::SeatToast(epro::format(L"Your turn · attacking {} · click another opponent's plate to switch", SeatName(BR().target_seat)), 0);
	}
}

void OnReloadField() {
	if(!BR().active)
		return;
	const uint8_t top = TopSeat();
	if(top != last_top)
		animate_top = last_top != NONE;
	last_top = top;
}

void Reset() {
	CoverGui(false);
	auto& d = mainGame->dField;
	const std::set<ClientCard*> mine(pool.begin(), pool.end());
	if(mine.count(d.hovered_card)) d.hovered_card = nullptr;
	if(mine.count(d.clicked_card)) d.clicked_card = nullptr;
	if(std::any_of(d.display_cards.begin(), d.display_cards.end(), [&](ClientCard* c) { return mine.count(c) != 0; })) {
		d.display_cards.clear();
		if(mainGame->wCardDisplay)
			mainGame->wCardDisplay->setVisible(false);
	}
	for(auto* c : pool)
		delete c;
	pool.clear();
	for(auto& f : frozen)
		f = Frozen{};
	for(auto& v : board_version)
		v = 0;
	name_cache.clear();
	table_open = false;
	table_anim = 0.f;
	hits.clear();
	table_seat.clear();
	hover_hit = hover_board = hover_chip = -1;
	hover_table_btn = false;
	focus_seat = -1;
	shown_code = 0;
	last_top = NONE;
	animate_top = false;
	force_live = false;
	hint_until = 0;
	last_idle_turn = -1;
	chip_seat.clear();
	rects = {};
}

// ------------------------------------------------------------------------------------- main thread
namespace {
// cards come down onto the table: a short eased drop with a fade
void DealIn(ClientCard* c) {
	auto& d = mainGame->dField;
	irr::core::vector3df pos, rot;
	d.GetCardDrawCoordinates(c, &pos, &rot, true);
	c->curPos = pos + irr::core::vector3df(0.f, 0.f, 0.45f);
	c->curRot = rot;
	c->curAlpha = 0;
	c->mTransform.setTranslation(c->curPos);
	c->mTransform.setRotationRadians(rot);
	c->aniFrame = 0;
	d.MoveCard(c, 11);
	d.FadeCard(c, 255, 11);
}
}

void BeginFrame() {
	if(!BR().active || !mainGame->dInfo.isInDuel)
		return;
	if(animate_top) {
		animate_top = false;
		if(!mainGame->dInfo.isCatchingUp)
			for(auto* list : { &mainGame->dField.mzone[1], &mainGame->dField.szone[1] })
				for(auto* c : *list)
					if(c && !c->is_moving) DealIn(c);
	}
}

void EndFrame() {}

bool Viewing() { return false; }
ClientCard* ViewCard(uint32_t, uint32_t) { return nullptr; }
bool ShowViewPile(uint32_t) { return false; }
bool TableOpen() { return table_open || table_anim > 0.f; }

void RequestTarget(uint8_t seat) {
	if(!CanTarget(seat))
		return;
	if(mainGame->dInfo.curMsg == MSG_SELECT_IDLECMD)
		DuelClient::SetResponseI((static_cast<int>(seat) << 16) | 9);
	else
		DuelClient::SetResponseI((static_cast<int>(seat) << 16) | 4);
	DuelClient::SendResponse();
}

bool DrawHud(int turn) {
	auto& br = BR();
	if(!br.active)
		return false;
	auto& d = mainGame->dField;
	const auto& di = mainGame->dInfo;
	mdfx::SeatHud hud;
	const uint8_t me = MeSeat();
	const int meField = FieldIndex(me);
	hud.lp = meField >= 0 ? di.lp[meField] : br.seats[me].lp;
	hud.startlp = di.startlp;
	hud.name = SeatName(me);
	hud.bottomTurn = br.active_seat == me;
	hud.turn = turn;
	hud.bottomTeam = br.teams ? br.seats[me].team : -1;
	const uint32_t now = Now();
	hud.targetHint = now < hint_until;
	hud.tableOpen = table_open;
	hud.tableHover = hover_table_btn;
	chip_seat.clear();
	for(uint8_t s = 0; s < br.count && s < 4; ++s) {
		if(s == me)
			continue;
		const auto& seat = br.seats[s];
		mdfx::SeatChip c;
		c.name = SeatName(s);
		c.alive = seat.alive;
		c.placement = seat.placement;
		c.turn = s == br.active_seat;
		c.target = s == br.target_seat;
		c.isYou = s == br.my_seat;
		c.ally = Allies(s, me);
		c.team = br.teams ? seat.team : -1;
		c.colorIndex = s;
		const int fi = FieldIndex(s);
		c.onField = fi >= 0;
		if(fi >= 0) {
			c.lp = di.lp[fi];
			c.hand = static_cast<uint32_t>(d.hand[fi].size());
			c.deck = static_cast<uint32_t>(d.deck[fi].size());
		} else {
			c.lp = seat.lp;
			c.hand = seat.hand;
			c.deck = seat.deck;
		}
		const int idx = static_cast<int>(chip_seat.size());
		c.hovered = idx == hover_chip;
		c.canTarget = CanTarget(s);
		c.targetHover = c.canTarget && c.hovered;
		chip_seat.push_back(s);
		hud.chips.push_back(std::move(c));
	}
	return mdfx::DrawSeatHud(hud, rects);
}

void DrawTable() {
	if(!BR().active || !mainGame->dInfo.isInDuel) {
		hits.clear();
		return;
	}
	if(force_live.exchange(false))
		CloseTable();
	const uint32_t now = Now();
	const float dt = last_ms ? std::min(100.f, static_cast<float>(now - last_ms)) : 16.f;
	last_ms = now;
	table_anim = table_open ? std::min(1.f, table_anim + dt / 170.f) : std::max(0.f, table_anim - dt / 130.f);
	if(table_anim <= 0.f) {
		hits.clear();
		return;
	}
	mdfx::TableView v;
	BuildTable(v);
	mdfx::DrawTable(v, hits);
}

void CoverGui(bool on) {
	if(on) {
		if(!covered.empty() || table_anim < 0.35f)
			return;
		for(irr::gui::IGUIElement* el : { static_cast<irr::gui::IGUIElement*>(mainGame->wPhase), static_cast<irr::gui::IGUIElement*>(mainGame->wChat),
										  static_cast<irr::gui::IGUIElement*>(mainGame->stHintMsg), static_cast<irr::gui::IGUIElement*>(mainGame->wCmdMenu),
										  static_cast<irr::gui::IGUIElement*>(mainGame->fpsCounter) }) {
			if(!el || !el->isVisible())
				continue;
			covered.push_back({ el, true });
			el->setVisible(false);
		}
	} else {
		for(auto& c : covered)
			c.el->setVisible(c.was);
		covered.clear();
	}
}

bool OnMouse(const irr::SEvent& event) {
	if(!BR().active || !mainGame->dInfo.isInDuel)
		return false;
	if(event.EventType == irr::EET_KEY_INPUT_EVENT) {
		if(!event.KeyInput.PressedDown)
			return false;
		const auto key = event.KeyInput.Key;
		if(key != irr::KEY_TAB && !(key == irr::KEY_ESCAPE && table_open))
			return false;
		auto* focus = mainGame->env->getFocus();
		if(focus && focus->getType() == irr::gui::EGUIET_EDIT_BOX)
			return false;
		if(table_open) CloseTable(); else OpenTable(-1);
		return true;
	}
	if(event.EventType != irr::EET_MOUSE_INPUT_EVENT)
		return false;
	const irr::core::vector2di m(event.MouseInput.X, event.MouseInput.Y);
	// the card list window opened from the table keeps its own mouse
	if(mainGame->wCardDisplay && mainGame->wCardDisplay->isVisible() && mainGame->wCardDisplay->getAbsolutePosition().isPointInside(m))
		return false;
	const bool inTable = InTable(m);
	const bool onTableBtn = rects.table.getArea() > 0 && rects.table.isPointInside(m);
	int chip = -1;
	for(size_t i = 0; i < rects.chip.size(); ++i)
		if(rects.chip[i].isPointInside(m))
			chip = static_cast<int>(i);
	switch(event.MouseInput.Event) {
	case irr::EMIE_MOUSE_MOVED: {
		hover_chip = chip;
		hover_table_btn = onTableBtn;
		if(!table_open) {
			hover_hit = hover_board = -1;
			return false;
		}
		hover_hit = inTable ? HitAt(m) : -1;
		hover_board = -1;
		for(const auto& h : hits)
			if(h.kind == mdfx::TableHit::BOARD && h.rect.isPointInside(m))
				hover_board = h.seat;
		if(hover_hit >= 0 && hits[hover_hit].code && hits[hover_hit].code != shown_code) {
			shown_code = hits[hover_hit].code;
			mainGame->ShowCardInfo(shown_code);
		}
		return inTable;
	}
	case irr::EMIE_LMOUSE_LEFT_UP: {
		if(onTableBtn) {
			if(table_open) CloseTable(); else OpenTable(-1);
			return true;
		}
		if(chip >= 0 && chip < static_cast<int>(chip_seat.size())) {
			const uint8_t s = chip_seat[chip];
			if(CanTarget(s))
				RequestTarget(s);
			else
				OpenTable(s);
			return true;
		}
		if(!inTable)
			return false;
		const int h = HitAt(m);
		if(h < 0)
			return true;
		const auto hit = hits[h];
		const uint8_t seat = hit.seat >= 0 && hit.seat < static_cast<int>(table_seat.size()) ? table_seat[hit.seat] : NONE;
		switch(hit.kind) {
		case mdfx::TableHit::CLOSE: CloseTable(); break;
		case mdfx::TableHit::TARGET: if(seat != NONE) RequestTarget(seat); break;
		case mdfx::TableHit::PILE: if(seat != NONE) ShowPile(seat, hit.location); break;
		case mdfx::TableHit::CARD: if(hit.code) mainGame->ShowCardInfo(hit.code); break;
		default: break;
		}
		return true;
	}
	case irr::EMIE_RMOUSE_LEFT_UP:
		if(inTable) { CloseTable(); return true; }
		return chip >= 0 || onTableBtn;
	case irr::EMIE_LMOUSE_PRESSED_DOWN:
	case irr::EMIE_RMOUSE_PRESSED_DOWN:
	case irr::EMIE_LMOUSE_DOUBLE_CLICK:
	case irr::EMIE_MOUSE_WHEEL:
		return inTable || chip >= 0 || onTableBtn;
	default:
		return inTable;
	}
}

// ------------------------------------------------------------------------------------- test aid
// BR_AUTOTEST=Pharaoh,Kaiba,Joey,Mai: from the main menu (or the LAN screen), host with the saved settings
// (system.conf), add those bots (bots.json deck or name), start. BR_AUTOTEST_SEAT=player keeps you in the
// first seat with your last deck (ready) instead of spectating. BR_AUTOTEST_TABLE=1 opens and closes the
// table every few seconds once the duel runs; on your own turn it opens the table, switches the target once
// and ends the turn. Chains are passed automatically.
void AutoTest() {
	static const char* env = std::getenv("BR_AUTOTEST");
	if(!env || !*env)
		return;
	static const char* seatEnv = std::getenv("BR_AUTOTEST_SEAT");
	static const bool asPlayer = seatEnv && std::string(seatEnv) == "player";
	static const char* tableEnv = std::getenv("BR_AUTOTEST_TABLE");
	static const bool cycleTable = tableEnv && *tableEnv && *tableEnv != '0';
	static int step = 0;
	static uint32_t at = 0;
	static std::vector<std::wstring> names;
	static size_t next_bot = 0;
	const uint32_t now = Now();
	auto click = [](irr::gui::IGUIElement* el) {
		irr::SEvent ev{};
		ev.EventType = irr::EET_GUI_EVENT;
		ev.GUIEvent.Caller = el;
		ev.GUIEvent.Element = nullptr;
		ev.GUIEvent.EventType = irr::gui::EGET_BUTTON_CLICKED;
		mainGame->menuHandler.OnEvent(ev);
	};
	if(names.empty()) {
		std::string cur;
		for(const char* c = env;; ++c) {
			if(*c == ',' || !*c) { if(!cur.empty()) names.push_back(std::wstring(cur.begin(), cur.end())); cur.clear(); if(!*c) break; }
			else cur += *c;
		}
	}
	switch(step) {
	case 0:
		if(!mainGame->coreloaded || mainGame->dInfo.isInDuel || mainGame->dInfo.isInLobby)
			break;
		if(!at) at = now + 2500;
		if(now < at)
			break;
		if(mainGame->wMainMenu && mainGame->wMainMenu->isVisible()) {
			click(mainGame->btnLanMode);
			at = now + 1500;
			break;
		}
		if(mainGame->btnCreateHost->isEnabled()) {
			click(mainGame->btnCreateHost);
			step = 10;
			at = now + 1200;
		}
		break;
	case 10:
		if(now > at) { click(mainGame->btnHostConfirm); step = 1; at = now + 1500; }
		break;
	case 1:
		if(mainGame->dInfo.isInLobby && now > at) {
			if(asPlayer) {
				click(mainGame->btnHostPrepReady);
			} else {
				click(mainGame->btnHostPrepOB);
			}
			step = 2;
			at = now + 1500;
		}
		break;
	case 2:
		if(now > at) {
			if(next_bot >= names.size()) { step = 3; at = now + 3000; break; }
			const auto& want = names[next_bot++];
			const auto& bots = mainGame->gBot.bots;
			const WindBot* pick = nullptr;
			for(const auto& b : bots) if(b.deck == want) { pick = &b; break; }
			if(!pick) for(const auto& b : bots) if(b.name.find(want) != std::wstring::npos) { pick = &b; break; }
			if(pick) {
				try {
					const int port = std::stoi(gGameConfig->serverport);
					pick->Launch(port, mainGame->dInfo.secret.pass, false, 0, nullptr);
				} catch(...) {}
			}
			at = now + 3500;
		}
		break;
	case 3:
		if(mainGame->dInfo.isInDuel) { step = 4; at = now + 10000; break; }
		if(now > at && mainGame->btnHostPrepStart->isEnabled()) { click(mainGame->btnHostPrepStart); at = now + 4000; }
		break;
	case 4: {
		if(!BR().active)
			break;
		if(mainGame->btnChainIgnore && !mainGame->btnChainIgnore->isPressed())
			mainGame->btnChainIgnore->setPressed(true);
		// our own turn: table open, one target switch, then end the turn
		static int myStage = 0;
		static int myTurn = -1;
		static uint32_t myAt = 0;
		const auto& br = BR();
		if(asPlayer && br.my_seat < 4 && br.active_seat == br.my_seat && mainGame->dInfo.curMsg == MSG_SELECT_IDLECMD && !DuelClient::answered) {
			if(myTurn != mainGame->dInfo.turn) { myTurn = mainGame->dInfo.turn; myStage = 0; myAt = now + 2500; }
			if(now < myAt)
				break;
			switch(myStage) {
			case 0: OpenTable(-1); myStage = 1; myAt = now + 4000; break;
			case 1: {
				uint8_t pick = NONE;
				for(uint8_t s = 0; s < br.count && s < 4; ++s)
					if(CanTarget(s)) pick = s;
				myStage = 2;
				myAt = now + 4500;
				if(pick != NONE) RequestTarget(pick);
				break;
			}
			case 2: CloseTable(); myStage = 3; myAt = now + 3000; break;
			case 3:
				myStage = 4;
				DuelClient::SetResponseI(7); // end phase
				DuelClient::SendResponse();
				break;
			default: break;
			}
			break;
		}
		if(!cycleTable || now < at)
			break;
		if(table_open) { CloseTable(); at = now + 7000; }
		else { OpenTable(-1); at = now + 9000; }
		break;
	}
	default:
		break;
	}
}

}
}
