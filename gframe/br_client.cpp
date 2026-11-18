#include "br_client.h"
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
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

// the frozen board of one offstage seat, as cards of our own (never mixed with the live field's cards)
struct View {
	uint8_t seat = NONE;
	std::vector<ClientCard*> mzone, szone, hand, grave, remove, extra, deck;
	std::set<ClientCard*> overlays;
	int extra_p = 0;
	bool placed = false;
};
View view;
std::vector<ClientCard*> pool;        // every card made for a view, freed in Reset (piles shown in the card
                                      // list window may still point at them)
bool swapped = false;
std::set<ClientCard*> saved_overlays;
int saved_extra_p = 0;
std::atomic<bool> force_live{ false }; // duel thread -> main thread: a prompt needs the live field
bool view_dirty = false;               // a new board for the viewed seat arrived (gMutex)
bool animate_top = false;              // the live top half changed (gMutex)
uint8_t last_top = NONE;               // seat in the top half at the last field reload
uint32_t hint_until = 0;               // our turn started: pulse the seats we can attack until then
int last_idle_turn = -1;               // turn of our last idle prompt (gMutex)

// HUD input state (main thread)
mdfx::SeatHudRects rects;
std::vector<uint8_t> chip_seat;
int hover_chip = -1, hover_target = -1;
bool hover_back = false;
uint32_t hover_since = 0;
uint8_t locked_seat = NONE;  // clicked: stays until Back to live / clicking it again
uint8_t preview_seat = NONE; // hovered for a moment

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

bool Offstage(uint8_t seat) {
	const auto& br = BR();
	return seat < br.count && seat < 4 && br.seats[seat].slot > 1;
}

uint8_t WantedView() {
	const uint8_t s = preview_seat != NONE ? preview_seat : locked_seat;
	if(s == NONE || !Offstage(s) || BR().seats[s].board.empty())
		return NONE;
	return s;
}

void ClearViewLists() {
	view.mzone.assign(7, nullptr);
	view.szone.assign(8, nullptr);
	view.hand.clear();
	view.grave.clear();
	view.remove.clear();
	view.extra.clear();
	view.deck.clear();
	view.overlays.clear();
	view.extra_p = 0;
	view.placed = false;
}

// MSG_SEAT_BOARD body -> view cards. Layout: u8 seat, u8 alive, u8 placement, i32 lp, u32 deck, hand, extra,
// extra_p_count, grave, banished, u8 blocks, per block u8 location, u32 length, query stream.
bool Build(uint8_t seat) {
	ClearViewLists();
	view.seat = NONE;
	const auto& b = BR().seats[seat].board;
	if(b.size() < 32)
		return false;
	const uint8_t* p = b.data();
	uint32_t deck, extra_p;
	std::memcpy(&deck, p + 7, 4);
	std::memcpy(&extra_p, p + 19, 4);
	view.extra_p = static_cast<int>(extra_p);
	size_t pos = 31;
	const uint8_t blocks = p[pos++];
	for(uint8_t k = 0; k < blocks; ++k) {
		if(pos + 5 > b.size())
			return false;
		const uint8_t loc = p[pos];
		uint32_t len;
		std::memcpy(&len, p + pos + 1, 4);
		pos += 5;
		if(pos + len > b.size() || len < 4)
			return false;
		CoreUtils::QueryStream stream{ p + pos, false, len, mainGame->dInfo.legacy_race_size };
		pos += len;
		uint32_t seq = 0;
		for(const auto& q : stream.GetQueries()) {
			const uint32_t s = seq++;
			if(q.IsEmpty())
				continue;
			auto* c = NewCard(loc, s);
			for(size_t o = 0; o < q.OverlayCount(); ++o) {
				auto* ov = NewCard(LOCATION_OVERLAY, static_cast<uint32_t>(o));
				ov->overlayTarget = c;
				ov->position = POS_FACEUP;
				c->overlayed.push_back(ov);
				view.overlays.insert(ov);
			}
			switch(loc) {
			case LOCATION_MZONE: if(s < 7) view.mzone[s] = c; break;
			case LOCATION_SZONE: if(s < 8) view.szone[s] = c; break;
			case LOCATION_HAND: c->sequence = static_cast<uint32_t>(view.hand.size()); view.hand.push_back(c); break;
			case LOCATION_GRAVE: c->sequence = static_cast<uint32_t>(view.grave.size()); view.grave.push_back(c); break;
			case LOCATION_REMOVED: c->sequence = static_cast<uint32_t>(view.remove.size()); view.remove.push_back(c); break;
			case LOCATION_EXTRA: c->sequence = static_cast<uint32_t>(view.extra.size()); view.extra.push_back(c); break;
			default: break;
			}
			// UpdateInfo may move a card to its spot: the view lists must be in the field (Place) for that,
			// so only the data is read here
			c->UpdateInfo(q);
		}
	}
	for(uint32_t i = 0; i < deck; ++i)
		view.deck.push_back(NewCard(LOCATION_DECK, i));
	view.seat = seat;
	return true;
}

template<typename F>
void ForViewCards(F&& f) {
	auto& d = mainGame->dField;
	for(auto* list : { &d.mzone[1], &d.szone[1], &d.hand[1], &d.grave[1], &d.remove[1], &d.extra[1], &d.deck[1] })
		for(auto* c : *list)
			if(c) f(c);
	for(auto* c : view.overlays)
		f(c);
}

void Swap() {
	auto& d = mainGame->dField;
	std::swap(d.mzone[1], view.mzone);
	std::swap(d.szone[1], view.szone);
	std::swap(d.hand[1], view.hand);
	std::swap(d.grave[1], view.grave);
	std::swap(d.remove[1], view.remove);
	std::swap(d.extra[1], view.extra);
	std::swap(d.deck[1], view.deck);
	std::swap(d.extra_p_count[1], view.extra_p);
	if(!swapped) {
		saved_overlays = d.overlay_cards;
		std::set<ClientCard*> shown;
		for(auto* c : saved_overlays)
			if(c->overlayTarget && c->overlayTarget->controler == 0)
				shown.insert(c);
		shown.insert(view.overlays.begin(), view.overlays.end());
		d.overlay_cards = std::move(shown);
	} else {
		d.overlay_cards = std::move(saved_overlays);
		saved_overlays.clear();
	}
	swapped = !swapped;
}

// cards come down onto the table: a short eased drop with a fade (both halves use it)
void DealIn(ClientCard* c, bool animate) {
	auto& d = mainGame->dField;
	irr::core::vector3df pos, rot;
	d.GetCardDrawCoordinates(c, &pos, &rot, true);
	c->curPos = pos;
	c->curRot = rot;
	c->mTransform.setTranslation(pos);
	c->mTransform.setRotationRadians(rot);
	c->is_moving = false;
	c->is_fading = false;
	c->aniFrame = 0;
	c->curAlpha = 255;
	if(!animate || mainGame->dInfo.isCatchingUp)
		return;
	c->curPos = pos + irr::core::vector3df(0.f, 0.f, 0.45f);
	c->curAlpha = 0;
	c->mTransform.setTranslation(c->curPos);
	d.MoveCard(c, 11);
	d.FadeCard(c, 255, 11);
}

void DropView(bool animate_live) {
	if(view.seat == NONE)
		return;
	ClearViewLists();
	view.seat = NONE;
	auto& d = mainGame->dField;
	if(d.hovered_card && d.hovered_card->controler == 1 && std::find(pool.begin(), pool.end(), d.hovered_card) != pool.end())
		d.hovered_card = nullptr;
	if(animate_live)
		animate_top = true;
}

std::wstring SeatName(uint8_t s) {
	const auto& br = BR();
	return s < 4 ? br.seats[s].name : L"";
}

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
	if(seat == view.seat || seat == locked_seat || seat == preview_seat)
		view_dirty = true;
}

void OnPrompt(uint8_t msg) {
	// these prompts pick cards/zones on the live field: show it
	switch(msg) {
	case MSG_SELECT_IDLECMD:
	case MSG_SELECT_BATTLECMD:
	case MSG_SELECT_YESNO:
	case MSG_SELECT_OPTION:
	case MSG_SELECT_EFFECTYN:
	case MSG_ANNOUNCE_RACE:
	case MSG_ANNOUNCE_ATTRIB:
	case MSG_ANNOUNCE_CARD:
	case MSG_ANNOUNCE_NUMBER:
	case MSG_ROCK_PAPER_SCISSORS:
		return;
	default:
		if(BR().active && (locked_seat != NONE || preview_seat != NONE))
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
			mdfx::SeatToast(epro::format(L"Your turn · attacking {} · pick another opponent above to switch", SeatName(BR().target_seat)), 0);
	}
}

void OnReloadField() {
	if(!BR().active)
		return;
	const uint8_t top = TopSeat();
	if(top != last_top)
		animate_top = last_top != NONE;
	last_top = top;
	view_dirty = true;
}

void Reset() {
	if(swapped)
		Swap();
	ClearViewLists();
	view.seat = NONE;
	// nothing may keep pointing at a view card once they are freed
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
	locked_seat = preview_seat = NONE;
	hover_chip = hover_target = -1;
	hover_back = false;
	last_top = NONE;
	animate_top = view_dirty = false;
	force_live = false;
	hint_until = 0;
	last_idle_turn = -1;
	chip_seat.clear();
	rects = {};
}

// ------------------------------------------------------------------------------------- main thread
void BeginFrame() {
	auto& br = BR();
	if(!br.active || !mainGame->dInfo.isInDuel || swapped)
		return;
	if(force_live.exchange(false)) {
		locked_seat = preview_seat = NONE;
	}
	if(locked_seat != NONE && !Offstage(locked_seat))
		locked_seat = NONE; // it came onto the field: live is the view now
	const uint8_t want = WantedView();
	if(want != view.seat || (view_dirty && want != NONE)) {
		const bool was = view.seat != NONE;
		DropView(want == NONE && was);
		if(want != NONE)
			Build(want);
	}
	view_dirty = false;
	if(view.seat == NONE) {
		if(animate_top) {
			animate_top = false;
			for(auto* list : { &mainGame->dField.mzone[1], &mainGame->dField.szone[1] })
				for(auto* c : *list)
					if(c && !c->is_moving) DealIn(c, true);
		}
		return;
	}
	Swap();
	if(!view.placed) {
		view.placed = true;
		ForViewCards([](ClientCard* c) { DealIn(c, (c->location & (LOCATION_MZONE | LOCATION_SZONE)) != 0); });
	}
}

void EndFrame() {
	if(swapped)
		Swap();
}

bool Viewing() {
	return view.seat != NONE;
}

ClientCard* ViewCard(uint32_t location, uint32_t sequence) {
	if(view.seat == NONE || swapped)
		return nullptr;
	switch(location) {
	case LOCATION_MZONE: return sequence < view.mzone.size() ? view.mzone[sequence] : nullptr;
	case LOCATION_SZONE: return sequence < view.szone.size() ? view.szone[sequence] : nullptr;
	case LOCATION_GRAVE: return view.grave.empty() ? nullptr : view.grave.back();
	case LOCATION_REMOVED: return view.remove.empty() ? nullptr : view.remove.back();
	case LOCATION_EXTRA: return view.extra.empty() ? nullptr : view.extra.back();
	case LOCATION_DECK: return view.deck.empty() ? nullptr : view.deck.back();
	default: return nullptr;
	}
}

bool ShowViewPile(uint32_t location) {
	if(view.seat == NONE || swapped)
		return false;
	auto& d = mainGame->dField;
	int loc_id = 0;
	switch(location) {
	case LOCATION_GRAVE: loc_id = 1004; d.display_cards.assign(view.grave.crbegin(), view.grave.crend()); break;
	case LOCATION_REMOVED: loc_id = 1005; d.display_cards.assign(view.remove.crbegin(), view.remove.crend()); break;
	case LOCATION_EXTRA: loc_id = 1006; d.display_cards.assign(view.extra.crbegin(), view.extra.crend()); break;
	case LOCATION_MZONE: {
		loc_id = 1007;
		d.display_cards.clear();
		for(auto* c : view.mzone)
			if(c) d.display_cards.insert(d.display_cards.end(), c->overlayed.begin(), c->overlayed.end());
		break;
	}
	default: return true; // swallow the click: the viewed board is read-only
	}
	if(d.display_cards.empty())
		return true;
	mainGame->wCardDisplay->setText(epro::format(L"{} · {}({})", SeatName(view.seat), gDataManager->GetSysString(loc_id), d.display_cards.size()).data());
	d.ShowLocationCard();
	return true;
}

void RequestTarget(uint8_t seat) {
	if(!CanTarget(seat))
		return;
	if(mainGame->dInfo.curMsg == MSG_SELECT_IDLECMD)
		DuelClient::SetResponseI((static_cast<int>(seat) << 16) | 9);
	else
		DuelClient::SetResponseI((static_cast<int>(seat) << 16) | 4);
	locked_seat = preview_seat = NONE;
	DuelClient::SendResponse();
}

bool DrawHud(int turn) {
	auto& br = BR();
	if(!br.active)
		return false;
	auto& d = mainGame->dField;
	const auto& di = mainGame->dInfo;
	mdfx::SeatHud hud;
	const uint8_t bottom = BottomSeat() & 3, top = TopSeat() & 3;
	hud.lp = di.lp[0];
	hud.startlp = di.startlp;
	hud.name = br.seats[bottom].name;
	if(br.teams)
		hud.name += br.seats[bottom].team == 0 ? L" · A" : L" · B";
	hud.bottomTurn = br.active_seat == bottom;
	hud.turn = turn;
	hud.bottomTeam = br.teams ? br.seats[bottom].team : -1;
	const uint32_t now = Now();
	hud.targetHint = now < hint_until;
	chip_seat.clear();
	for(uint8_t s = 0; s < br.count && s < 4; ++s) {
		if(s == bottom)
			continue;
		const auto& seat = br.seats[s];
		mdfx::SeatChip c;
		c.name = seat.name;
		c.alive = seat.alive;
		c.placement = seat.placement;
		c.turn = s == br.active_seat;
		c.target = s == br.target_seat;
		c.onField = s == top && view.seat == NONE;
		c.viewing = s == view.seat;
		c.isYou = s == br.my_seat;
		c.team = br.teams ? seat.team : -1;
		c.colorIndex = s;
		if(s == top) {
			// on the field: live numbers (while a frozen board is swapped in, the live lists sit in "view")
			c.lp = di.lp[1];
			c.hand = static_cast<uint32_t>(swapped ? view.hand.size() : d.hand[1].size());
			c.deck = static_cast<uint32_t>(swapped ? view.deck.size() : d.deck[1].size());
		} else {
			c.lp = seat.lp;
			c.hand = seat.hand;
			c.deck = seat.deck;
		}
		const int idx = static_cast<int>(chip_seat.size());
		c.hovered = idx == hover_chip;
		c.canTarget = CanTarget(s);
		c.targetHover = c.canTarget && idx == hover_target;
		chip_seat.push_back(s);
		hud.chips.push_back(std::move(c));
	}
	hud.viewing = view.seat != NONE;
	hud.fieldName = hud.viewing ? SeatName(view.seat) : SeatName(top);
	hud.backHover = hover_back;
	// a hover becomes a preview after a short pause (no flicker while the mouse crosses the bar)
	if(hover_chip >= 0 && hover_chip < static_cast<int>(chip_seat.size()) && hover_target < 0) {
		const uint8_t s = chip_seat[hover_chip];
		preview_seat = (now - hover_since > 220 && Offstage(s)) ? s : NONE;
	} else
		preview_seat = NONE;
	return mdfx::DrawSeatHud(hud, rects);
}

bool OnMouse(const irr::SEvent& event) {
	if(!BR().active || !mainGame->dInfo.isInDuel || event.EventType != irr::EET_MOUSE_INPUT_EVENT)
		return false;
	const irr::core::vector2di m(event.MouseInput.X, event.MouseInput.Y);
	switch(event.MouseInput.Event) {
	case irr::EMIE_MOUSE_MOVED: {
		int chip = -1, target = -1;
		for(size_t i = 0; i < rects.chip.size(); ++i) {
			if(rects.chip[i].isPointInside(m))
				chip = static_cast<int>(i);
			if(rects.target.size() > i && rects.target[i].getArea() > 0 && rects.target[i].isPointInside(m))
				target = static_cast<int>(i);
		}
		if(chip != hover_chip)
			hover_since = Now();
		hover_chip = chip;
		hover_target = target;
		hover_back = rects.back.getArea() > 0 && rects.back.isPointInside(m);
		return false;
	}
	case irr::EMIE_LMOUSE_LEFT_UP: {
		if(rects.back.getArea() > 0 && rects.back.isPointInside(m)) {
			locked_seat = preview_seat = NONE;
			return true;
		}
		for(size_t i = 0; i < rects.chip.size() && i < chip_seat.size(); ++i) {
			if(!rects.chip[i].isPointInside(m))
				continue;
			const uint8_t s = chip_seat[i];
			if(rects.target.size() > i && rects.target[i].getArea() > 0 && rects.target[i].isPointInside(m) && CanTarget(s)) {
				RequestTarget(s);
				return true;
			}
			if(Offstage(s))
				locked_seat = (locked_seat == s) ? NONE : s;
			else
				locked_seat = NONE; // it is on the field: back to live
			preview_seat = NONE;
			hover_since = Now() + 100000; // no hover preview until the mouse leaves and comes back
			return true;
		}
		return false;
	}
	case irr::EMIE_LMOUSE_PRESSED_DOWN:
	case irr::EMIE_RMOUSE_PRESSED_DOWN:
	case irr::EMIE_RMOUSE_LEFT_UP: {
		for(const auto& r : rects.chip)
			if(r.isPointInside(m))
				return true;
		return rects.back.getArea() > 0 && rects.back.isPointInside(m);
	}
	default:
		return false;
	}
}

// ------------------------------------------------------------------------------------- test aid
// BR_AUTOTEST=Pharaoh,Kaiba,Joey,Mai: from the main menu, host with the saved settings (system.conf), go to
// the spectator seat, add those bots (bots.json deck or name), start. BR_AUTOTEST_VIEW=1 also cycles the
// viewed board every few seconds once the duel runs.
void AutoTest() {
	static const char* env = std::getenv("BR_AUTOTEST");
	if(!env || !*env)
		return;
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
		if(mainGame->coreloaded && !mainGame->dInfo.isInDuel && !mainGame->dInfo.isInLobby && mainGame->btnCreateHost->isEnabled()) {
			if(!at) at = now + 2500;
			if(now > at) { click(mainGame->btnCreateHost); step = 10; at = now + 1200; }
		}
		break;
	case 10:
		if(now > at) { click(mainGame->btnHostConfirm); step = 1; at = now + 1500; }
		break;
	case 1:
		if(mainGame->dInfo.isInLobby && now > at) { click(mainGame->btnHostPrepOB); step = 2; at = now + 1500; }
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
		if(mainGame->dInfo.isInDuel) { step = 4; at = now + 8000; break; }
		if(now > at && mainGame->btnHostPrepStart->isEnabled()) { click(mainGame->btnHostPrepStart); at = now + 4000; }
		break;
	case 4: {
		static const char* cyc = std::getenv("BR_AUTOTEST_VIEW");
		if(!cyc || !*cyc || !BR().active || now < at)
			break;
		at = now + 4000;
		// next offstage seat with a board, then live, round and round
		static int k = 0;
		const auto& br = BR();
		uint8_t pick = NONE;
		for(int i = 0; i < 4 && pick == NONE; ++i) {
			const uint8_t s = static_cast<uint8_t>((k + i) % 5);
			if(s < br.count && Offstage(s) && !br.seats[s].board.empty())
				pick = s;
			if(s == 4) break;
		}
		k = (k + 1) % 5;
		locked_seat = pick;
		break;
	}
	default:
		break;
	}
}

}
}
