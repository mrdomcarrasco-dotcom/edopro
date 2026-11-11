// Progression "duel polish" layer. See mdfx.h.
#include "mdfx.h"
#include <algorithm>
#include <cmath>
#include <map>
#include <random>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include "game.h"
#include "game_config.h"
#include "data_manager.h"
#include "image_manager.h"
#include "sound_manager.h"
#include "CGUITTFont/CGUITTFont.h"
#include "fmt.h"
#include "utils.h"

namespace mdfx {
using namespace irr;
using ygo::mainGame;
using ygo::gDataManager;
using ygo::gSoundManager;
using ygo::Utils;

namespace {

constexpr float PI = 3.14159265f;

video::IVideoDriver* drv = nullptr;
gui::CGUITTFont* fBanner = nullptr;
gui::CGUITTFont* fNum = nullptr;
video::ITexture* tGlow = nullptr;   // soft radial glow
video::ITexture* tRing = nullptr;   // thin ring
video::ITexture* tRays = nullptr;   // radial light rays
video::ITexture* tStreak = nullptr; // horizontal light streak
video::ITexture* tSeal = nullptr;   // circle + five-point star
bool enabled = true;
std::map<uint32_t, Cutin> listed;   // mdfx.conf
std::map<uint32_t, video::ITexture*> artCache;
std::mt19937 rng{ 20261110u };

float frand(float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); }
float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }
float smooth(float a, float b, float x) { float t = clamp01((x - a) / (b - a)); return t * t * (3.f - 2.f * t); }
float outCubic(float t) { t = clamp01(t); float u = 1.f - t; return 1.f - u * u * u; }
float outQuint(float t) { t = clamp01(t); float u = 1.f - t; return 1.f - u * u * u * u * u; }
float outBack(float t, float s = 1.7f) { t = clamp01(t) - 1.f; return t * t * ((s + 1.f) * t + s) + 1.f; }
video::SColor col(float a, const video::SColor& c) {
	return video::SColor((u32)(clamp01(a) * 255.f), c.getRed(), c.getGreen(), c.getBlue());
}
// additive textures: rgb carries the intensity, so fade with the vertex rgb
video::SColor addc(float a, const video::SColor& c) {
	a = clamp01(a);
	return video::SColor(255, (u32)(c.getRed() * a), (u32)(c.getGreen() * a), (u32)(c.getBlue() * a));
}

template<typename F>
video::ITexture* MakeTex(const char* name, u32 w, u32 h, F f) {
	auto img = drv->createImage(video::ECF_A8R8G8B8, core::dimension2du(w, h));
	if(!img) return nullptr;
	for(u32 y = 0; y < h; ++y)
		for(u32 x = 0; x < w; ++x) {
			const float u = (x + 0.5f) / w * 2.f - 1.f, v = (y + 0.5f) / h * 2.f - 1.f;
			const u32 a = (u32)(clamp01(f(u, v)) * 255.f);
			img->setPixel(x, y, video::SColor(a, a, a, a));
		}
	auto tex = drv->addTexture(name, img);
	img->drop();
	return tex;
}

float SegDist(float px, float py, float ax, float ay, float bx, float by) {
	const float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
	const float t = clamp01((wx * vx + wy * vy) / (vx * vx + vy * vy));
	const float dx = wx - vx * t, dy = wy - vy * t;
	return std::sqrt(dx * dx + dy * dy);
}

void BuildTextures() {
	tGlow = MakeTex("mdfx_glow", 128, 128, [](float u, float v) {
		const float r = std::sqrt(u * u + v * v);
		const float a = 1.f - r;
		return a <= 0.f ? 0.f : a * a;
	});
	tRing = MakeTex("mdfx_ring", 256, 256, [](float u, float v) {
		const float r = std::sqrt(u * u + v * v);
		const float d = (r - 0.86f) / 0.07f;
		return std::exp(-d * d) + 0.35f * std::exp(-((r - 0.80f) / 0.18f) * ((r - 0.80f) / 0.18f)) * (r < 0.86f);
	});
	tRays = MakeTex("mdfx_rays", 512, 512, [](float u, float v) {
		const float r = std::sqrt(u * u + v * v);
		if(r > 1.f) return 0.f;
		const float ang = std::atan2(v, u);
		// 18 rays of uneven width
		float ray = 0.5f + 0.5f * std::cos(ang * 18.f) * (0.7f + 0.3f * std::cos(ang * 7.f + 1.3f));
		ray = std::pow(clamp01(ray), 6.f);
		const float fall = (1.f - r) * (1.f - r);
		return ray * fall * 1.4f + std::pow(1.f - r, 5.f) * 0.6f;
	});
	tStreak = MakeTex("mdfx_streak", 256, 32, [](float u, float v) {
		const float across = std::exp(-(v * v) / 0.06f);
		const float along = 1.f - std::fabs(u);
		return across * along * along;
	});
	tSeal = MakeTex("mdfx_seal", 512, 512, [](float u, float v) {
		const float r = std::sqrt(u * u + v * v);
		float d = std::fabs(r - 0.92f);
		d = std::min(d, std::fabs(r - 0.84f));
		// five-point star through the points of the inner circle
		float px[5], py[5];
		for(int i = 0; i < 5; ++i) {
			const float a = -PI / 2.f + i * 2.f * PI / 5.f;
			px[i] = std::cos(a) * 0.84f;
			py[i] = std::sin(a) * 0.84f;
		}
		for(int i = 0; i < 5; ++i) {
			const int j = (i + 2) % 5;
			d = std::min(d, SegDist(u, v, px[i], py[i], px[j], py[j]));
		}
		const float line = std::exp(-(d * d) / 0.00018f);
		const float halo = std::exp(-(d * d) / 0.004f) * 0.45f;
		return line + halo;
	});
}

// --- drawing helpers -------------------------------------------------------------------------
void Quad(video::ITexture* tex, const core::rect<s32>& src, core::vector2df c, float w, float h, float rot,
		  video::SColor color, bool additive) {
	if(!tex) return;
	const auto ss = drv->getScreenSize();
	const float sw = (float)ss.Width, sh = (float)ss.Height;
	const float cs = std::cos(rot), sn = std::sin(rot);
	const core::vector2df off[4] = { {-w / 2, -h / 2}, {w / 2, -h / 2}, {-w / 2, h / 2}, {w / 2, h / 2} };
	const auto tsz = tex->getOriginalSize();
	const core::vector2df uv[4] = {
		{ src.UpperLeftCorner.X / (float)tsz.Width, src.UpperLeftCorner.Y / (float)tsz.Height },
		{ src.LowerRightCorner.X / (float)tsz.Width, src.UpperLeftCorner.Y / (float)tsz.Height },
		{ src.UpperLeftCorner.X / (float)tsz.Width, src.LowerRightCorner.Y / (float)tsz.Height },
		{ src.LowerRightCorner.X / (float)tsz.Width, src.LowerRightCorner.Y / (float)tsz.Height } };
	video::S3DVertex vtx[4];
	for(int i = 0; i < 4; ++i) {
		const float x = c.X + off[i].X * cs - off[i].Y * sn;
		const float y = c.Y + off[i].X * sn + off[i].Y * cs;
		vtx[i].Pos = core::vector3df((x / sw - 0.5f) * 2.f, (y / sh - 0.5f) * -2.f, 1.f);
		vtx[i].TCoords = uv[i];
		vtx[i].Color = color;
	}
	const u16 idx[6] = { 0, 1, 2, 3, 2, 1 };
	video::SMaterial m;
	m.Lighting = false;
	m.BackfaceCulling = false;
	m.ZBuffer = static_cast<decltype(m.ZBuffer)>(0);
#if IRRLICHT_VERSION_MAJOR==1 && IRRLICHT_VERSION_MINOR==9
	m.ZWriteEnable = video::E_ZWRITE::EZW_OFF;
#else
	m.ZWriteEnable = false;
#endif
	m.TextureLayer[0].Texture = tex;
	m.TextureLayer[0].TextureWrapU = video::ETC_CLAMP_TO_EDGE;
	m.TextureLayer[0].TextureWrapV = video::ETC_CLAMP_TO_EDGE;
	m.MaterialType = additive ? video::EMT_TRANSPARENT_ADD_COLOR : video::EMT_TRANSPARENT_ALPHA_CHANNEL;
	const auto oldP = drv->getTransform(video::ETS_PROJECTION);
	const auto oldV = drv->getTransform(video::ETS_VIEW);
	const auto oldW = drv->getTransform(video::ETS_WORLD);
	drv->setTransform(video::ETS_PROJECTION, core::IdentityMatrix);
	drv->setTransform(video::ETS_VIEW, core::IdentityMatrix);
	drv->setTransform(video::ETS_WORLD, core::IdentityMatrix);
	drv->setMaterial(m);
	drv->drawIndexedTriangleList(vtx, 4, idx, 2);
	drv->setTransform(video::ETS_PROJECTION, oldP);
	drv->setTransform(video::ETS_VIEW, oldV);
	drv->setTransform(video::ETS_WORLD, oldW);
}
void Quad(video::ITexture* tex, core::vector2df c, float w, float h, float rot, video::SColor color, bool additive) {
	if(!tex) return;
	const auto t = tex->getOriginalSize();
	Quad(tex, core::rect<s32>(0, 0, t.Width, t.Height), c, w, h, rot, color, additive);
}

video::ITexture* Art(uint32_t code) {
	auto it = artCache.find(code);
	if(it != artCache.end())
		return it->second;
	video::ITexture* tex = nullptr;
	for(auto& dir : mainGame->pic_dirs) {
		if(dir == EPRO_TEXT("archives"))
			continue;
		for(auto ext : { EPRO_TEXT(".jpg"), EPRO_TEXT(".png") }) {
			const epro::path_string file = epro::format(EPRO_TEXT("{}{}{}"), dir, code, ext);
			if(Utils::FileExists(file)) {
				tex = drv->getTexture({ file.data(), static_cast<u32>(file.size()) });
				if(tex) break;
			}
		}
		if(tex) break;
	}
	if(!tex) {
		const auto* data = gDataManager->GetCardData(code);
		if(data && data->alias && data->alias != code)
			tex = Art(data->alias);
	}
	if(!tex)
		tex = mainGame->imageManager.GetTextureCard(code, ygo::imgType::ART);
	artCache[code] = tex;
	return tex;
}
// the artwork box of a full card picture
core::rect<s32> ArtBox(video::ITexture* tex) {
	const auto s = tex->getOriginalSize();
	return core::rect<s32>((s32)(s.Width * 0.123f), (s32)(s.Height * 0.185f), (s32)(s.Width * 0.877f), (s32)(s.Height * 0.703f));
}

void PlayFx(const char* name) {
	if(gSoundManager)
		gSoundManager->PlayFx(name);
}

// --- state -----------------------------------------------------------------------------------
struct Pulse {
	enum Kind : uint8_t { RING, STAMP, FLASH, DUST } kind;
	bool screen;               // pos2 is already in pixels
	core::vector3df pos;
	core::vector2df pos2;
	float t, dur, size;
	video::SColor color;
	int num;
};
std::vector<Pulse> pulses;

struct Spark {
	core::vector2df p, v;
	float life, max, size, gravity;
	video::SColor color;
};
std::vector<Spark> sparks;

struct Burst {
	core::vector3df pos;
	int n;
	float speed, size, gravity, life;
	video::SColor color;
};
std::vector<Burst> bursts; // 3D bursts, turned into sparks at draw time

float shakePx = 0.f, shakeT = 0.f, shakeDur = 1.f;
core::vector2df shakeNow;
float flashA = 0.f, flashDecay = 1.f;
video::SColor flashCol{ 255, 255, 255, 255 };
float tintA = 0.f, tintDecay = 1.f;
video::SColor tintCol{ 255, 255, 40, 40 };

struct CutinState {
	Cutin kind = Cutin::NONE;
	uint32_t code = 0;
	float t = 0.f, dur = 0.f, emberAcc = 0.f;
	core::vector3df fieldPos;
	bool opp = false;
	int beat = 0; // next beat to fire
	video::SColor color;
	std::wstring name;
} cut;

video::SColor GodColor(Cutin k) {
	switch(k) {
	case Cutin::OBELISK: return { 255, 70, 140, 255 };
	case Cutin::SLIFER: return { 255, 255, 70, 60 };
	case Cutin::RA: return { 255, 255, 196, 60 };
	case Cutin::EXODIA_WIN: return { 255, 255, 176, 50 };
	case Cutin::ORICHALCOS: return { 255, 60, 255, 120 };
	default: return { 255, 200, 220, 255 };
	}
}
video::SColor AttributeColor(uint32_t code) {
	const auto* d = gDataManager->GetCardData(code);
	const uint32_t a = d ? d->attribute : 0;
	if(a & 0x01) return { 255, 200, 140, 80 };  // EARTH
	if(a & 0x02) return { 255, 70, 150, 255 };  // WATER
	if(a & 0x04) return { 255, 255, 90, 50 };   // FIRE
	if(a & 0x08) return { 255, 90, 230, 120 };  // WIND
	if(a & 0x10) return { 255, 255, 230, 140 }; // LIGHT
	if(a & 0x20) return { 255, 190, 90, 255 };  // DARK
	return { 255, 255, 200, 80 };               // DIVINE / other
}

float S() { return drv->getScreenSize().Height / 640.f; }
core::vector2df Center() { const auto s = drv->getScreenSize(); return { s.Width / 2.f, s.Height / 2.f }; }

void SparkBurst(core::vector2df at, int n, video::SColor c, float speed, float size, float gravity, float life) {
	const float s = S();
	for(int i = 0; i < n; ++i) {
		const float a = frand(0.f, 2.f * PI), sp = frand(0.25f, 1.f) * speed * s;
		sparks.push_back({ at, { std::cos(a) * sp, std::sin(a) * sp }, 0.f, life * frand(0.6f, 1.f), size * s * frand(0.5f, 1.f), gravity * s, c });
	}
}
void ScreenFlash(float a, float ms, video::SColor c = { 255, 255, 255, 255 }) {
	flashA = std::max(flashA, a);
	flashDecay = a / ms;
	flashCol = c;
}
void Ring2D(core::vector2df at, float size, float dur, video::SColor c) {
	pulses.push_back({ Pulse::RING, true, {}, at, 0.f, dur, size, c, 0 });
}

const uint32_t EXODIA_PIECES[5] = { 8124921, 44519536, 70903634, 7902349, 33396948 }; // R leg, L leg, R arm, L arm, head

float CutinLength(Cutin k) {
	switch(k) {
	case Cutin::OBELISK: case Cutin::SLIFER: case Cutin::RA: return 3600.f;
	case Cutin::EXODIA_WIN: return 4800.f;
	case Cutin::ORICHALCOS: return 2600.f;
	case Cutin::ACE: return 1900.f;
	default: return 0.f;
	}
}

void LoadConf() {
	std::ifstream in("./mdfx.conf");
	std::string line;
	while(std::getline(in, line)) {
		if(line.empty() || line[0] == '#')
			continue;
		std::istringstream ss(line);
		std::string a, b;
		ss >> a >> b;
		if(a == "effects") { enabled = (b != "off" && b != "0"); continue; }
		uint32_t code = 0;
		try { code = (uint32_t)std::stoul(a); } catch(...) { continue; }
		Cutin k = Cutin::NONE;
		if(b == "obelisk") k = Cutin::OBELISK;
		else if(b == "slifer") k = Cutin::SLIFER;
		else if(b == "ra") k = Cutin::RA;
		else if(b == "ace") k = Cutin::ACE;
		else if(b == "orichalcos") k = Cutin::ORICHALCOS;
		listed[code] = k;
	}
}

} // namespace

// --- public API --------------------------------------------------------------------------------
void Init(video::IVideoDriver* driver, gui::CGUITTFont* bannerFont, gui::CGUITTFont* numberFont) {
	drv = driver;
	fBanner = bannerFont;
	fNum = numberFont;
	BuildTextures();
	LoadConf();
}
bool Enabled() { return enabled && drv; }

float EaseMove(float t) {
	if(!enabled) return t;
	// quick launch, long soft landing
	return outQuint(t);
}
float MoveLift(float distance) {
	if(!enabled || distance < 0.6f) return 0.f;
	return std::min(0.55f, distance * 0.09f);
}

float MoveTimeScale() { return enabled ? 1.3f : 1.f; }

Cutin CutinFor(uint32_t code) {
	if(!Enabled()) return Cutin::NONE;
	auto it = listed.find(code);
	if(it != listed.end()) return it->second;
	const auto* d = gDataManager->GetCardData(code);
	const uint32_t base = (d && d->alias) ? d->alias : code;
	it = listed.find(base);
	if(it != listed.end()) return it->second;
	switch(base) {
	case 10000000: return Cutin::OBELISK;
	case 10000020: return Cutin::SLIFER;
	case 10000010: return Cutin::RA;
	case 48179391: return Cutin::ORICHALCOS;
	default: return Cutin::NONE;
	}
}

int StartCutin(Cutin kind, uint32_t code, const core::vector3df& fieldPos, bool opponent) {
	if(!Enabled() || kind == Cutin::NONE) return 0;
	cut = CutinState{};
	cut.kind = kind;
	cut.code = code;
	cut.dur = CutinLength(kind);
	cut.fieldPos = fieldPos;
	cut.opp = opponent;
	cut.color = kind == Cutin::ACE ? AttributeColor(code) : GodColor(kind);
	{ const auto nm = gDataManager->GetName(code); cut.name.assign(nm.data(), nm.size()); }
	// strip "(Anime)" style suffixes from the banner
	const auto paren = cut.name.find(L" (");
	if(paren != std::wstring::npos) cut.name.erase(paren);
	if(kind == Cutin::EXODIA_WIN) cut.name = L"EXODIA  -  OBLITERATE!!";
	return (int)std::ceil(cut.dur * 60.f / 1000.f) + 6;
}
bool CutinActive() { return cut.kind != Cutin::NONE; }

void ChainStamp(const core::vector3df& pos, int link, bool opponent) {
	if(!Enabled()) return;
	const video::SColor c = opponent ? video::SColor(255, 255, 90, 90) : video::SColor(255, 80, 200, 255);
	pulses.push_back({ Pulse::STAMP, false, pos, {}, 0.f, 820.f, 1.f, c, link });
	pulses.push_back({ Pulse::RING, false, pos, {}, 0.f, 520.f, 150.f, c, 0 });
	bursts.push_back({ pos, 14, 260.f, 9.f, 0.f, 520.f, c });
	PlayFx(epro::format("chain{}", std::min(link, 6)).data());
}
void ResolveFlash(const core::vector3df& pos, int link) {
	if(!Enabled()) return;
	(void)link;
	pulses.push_back({ Pulse::FLASH, false, pos, {}, 0.f, 480.f, 170.f, { 255, 200, 240, 255 }, 0 });
	pulses.push_back({ Pulse::RING, false, pos, {}, 0.f, 420.f, 120.f, { 255, 255, 255, 255 }, 0 });
	PlayFx("resolve");
}
void SummonImpact(const core::vector3df& pos, int level, bool special) {
	if(!Enabled()) return;
	const float big = level >= 7 ? 1.6f : (level >= 5 ? 1.25f : 1.f);
	const video::SColor c = special ? video::SColor(255, 150, 210, 255) : video::SColor(255, 255, 236, 200);
	pulses.push_back({ Pulse::RING, false, pos, {}, 0.f, 520.f, 190.f * big, c, 0 });
	pulses.push_back({ Pulse::DUST, false, pos, {}, 0.f, 600.f, 120.f * big, { 255, 210, 200, 180 }, 0 });
	bursts.push_back({ pos, (int)(18 * big), 300.f * big, 10.f, 380.f, 650.f, { 255, 225, 215, 200 } });
	Shake(level >= 7 ? 9.f : (level >= 5 ? 5.f : 2.5f), 360.f);
	PlayFx(level >= 7 ? "impact_heavy" : "impact");
}
void Damage(int amount, bool self) {
	if(!Enabled() || amount <= 0) return;
	Shake(std::min(18.f, 3.f + amount / 250.f), 420.f);
	tintA = std::min(0.55f, 0.18f + amount / 6000.f) * (self ? 1.f : 0.45f);
	tintDecay = tintA / 650.f;
}
void Shake(float pixels, float ms) {
	if(!Enabled()) return;
	if(pixels >= shakePx * (1.f - shakeT / shakeDur)) {
		shakePx = pixels;
		shakeT = 0.f;
		shakeDur = ms;
	}
}
void Clear() {
	pulses.clear();
	sparks.clear();
	bursts.clear();
	cut = CutinState{};
	shakePx = flashA = tintA = 0.f;
}

// --- per frame -------------------------------------------------------------------------------
namespace {
void CutinBeats() {
	const float t = cut.t;
	const auto c = Center();
	const float s = S();
	auto beat = [&](int i, float at) { if(cut.beat == i && t >= at) { ++cut.beat; return true; } return false; };
	switch(cut.kind) {
	case Cutin::OBELISK: case Cutin::SLIFER: case Cutin::RA: {
		if(beat(0, 0.f)) { PlayFx("god_rise"); Shake(3.f, 900.f); }
		if(beat(1, 760.f)) {
			PlayFx("god_impact");
			ScreenFlash(0.9f, 420.f);
			Shake(cut.kind == Cutin::OBELISK ? 22.f : 16.f, 700.f);
			SparkBurst(c, 90, cut.color, 900.f, 16.f, 0.f, 1100.f);
			Ring2D(c, 900.f * s, 700.f, cut.color);
		}
		if(beat(2, 1400.f)) {
			if(cut.kind == Cutin::OBELISK) { Shake(14.f, 450.f); Ring2D(c, 700.f * s, 500.f, cut.color); PlayFx("impact_heavy"); }
			else if(cut.kind == Cutin::SLIFER) { Ring2D(c, 500.f * s, 450.f, cut.color); Ring2D(c, 800.f * s, 650.f, cut.color); }
			else { ScreenFlash(0.35f, 500.f, { 255, 255, 220, 140 }); }
			PlayFx("god_roar");
		}
		if(beat(3, cut.dur - 260.f)) {
			pulses.push_back({ Pulse::RING, false, cut.fieldPos, {}, 0.f, 650.f, 420.f, cut.color, 0 });
			pulses.push_back({ Pulse::DUST, false, cut.fieldPos, {}, 0.f, 800.f, 260.f, { 255, 210, 200, 180 }, 0 });
			bursts.push_back({ cut.fieldPos, 40, 600.f, 14.f, 300.f, 900.f, cut.color });
			Shake(14.f, 600.f);
			PlayFx("impact_heavy");
		}
		break;
	}
	case Cutin::EXODIA_WIN: {
		for(int i = 0; i < 5; ++i)
			if(beat(i, 300.f + i * 340.f)) { PlayFx(i == 4 ? "impact_heavy" : "exodia_piece"); Shake(i == 4 ? 9.f : 5.f, 300.f); }
		if(beat(5, 2150.f)) PlayFx("god_rise");
		if(beat(6, 2600.f)) {
			PlayFx("god_impact");
			ScreenFlash(1.f, 600.f, { 255, 255, 236, 180 });
			Shake(26.f, 900.f);
			SparkBurst(c, 140, cut.color, 1100.f, 18.f, 0.f, 1400.f);
			Ring2D(c, 1100.f * s, 900.f, cut.color);
		}
		break;
	}
	case Cutin::ORICHALCOS: {
		if(beat(0, 0.f)) PlayFx("seal");
		if(beat(1, 800.f)) { ScreenFlash(0.6f, 500.f, { 255, 120, 255, 160 }); Shake(8.f, 500.f); SparkBurst(c, 70, cut.color, 700.f, 12.f, 0.f, 1000.f); }
		break;
	}
	case Cutin::ACE: {
		if(beat(0, 0.f)) PlayFx("ace_whoosh");
		if(beat(1, 330.f)) { PlayFx("impact_heavy"); ScreenFlash(0.45f, 300.f); Shake(10.f, 420.f); SparkBurst(c, 50, cut.color, 700.f, 12.f, 0.f, 800.f); }
		if(beat(2, cut.dur - 200.f)) {
			pulses.push_back({ Pulse::RING, false, cut.fieldPos, {}, 0.f, 520.f, 260.f, cut.color, 0 });
			bursts.push_back({ cut.fieldPos, 24, 420.f, 10.f, 300.f, 700.f, cut.color });
		}
		break;
	}
	default: break;
	}
	// embers rising during the god scenes
	if(cut.kind == Cutin::OBELISK || cut.kind == Cutin::SLIFER || cut.kind == Cutin::RA || cut.kind == Cutin::EXODIA_WIN) {
		const float from = cut.kind == Cutin::EXODIA_WIN ? 2600.f : 760.f;
		if(t > from && t < cut.dur - 350.f) {
			const auto ss = drv->getScreenSize();
			while(cut.emberAcc > 16.f) {
				cut.emberAcc -= 16.f;
				sparks.push_back({ { frand(0.f, (float)ss.Width), (float)ss.Height + 10.f },
								   { frand(-20.f, 20.f) * s, -frand(120.f, 320.f) * s },
								   0.f, frand(900.f, 1800.f), frand(5.f, 12.f) * s, -40.f * s, cut.color });
			}
		}
	}
}
} // namespace

void Update(uint32_t delta_ms) {
	if(!drv) return;
	const float dt = (float)delta_ms;
	for(auto& p : pulses) p.t += dt;
	pulses.erase(std::remove_if(pulses.begin(), pulses.end(), [](const Pulse& p) { return p.t >= p.dur; }), pulses.end());
	for(auto& sp : sparks) {
		sp.life += dt;
		sp.v.Y += sp.gravity * dt / 1000.f;
		sp.v *= std::pow(0.985f, dt / 16.f);
		sp.p += sp.v * (dt / 1000.f);
	}
	sparks.erase(std::remove_if(sparks.begin(), sparks.end(), [](const Spark& s) { return s.life >= s.max; }), sparks.end());
	if(sparks.size() > 1500) sparks.erase(sparks.begin(), sparks.begin() + (sparks.size() - 1500));
	if(shakePx > 0.f) {
		shakeT += dt;
		const float k = 1.f - clamp01(shakeT / shakeDur);
		const float amp = shakePx * k * k;
		shakeNow = core::vector2df(frand(-1.f, 1.f) * amp, frand(-1.f, 1.f) * amp);
		if(shakeT >= shakeDur) { shakePx = 0.f; shakeNow = core::vector2df(0.f, 0.f); }
	}
	flashA = std::max(0.f, flashA - flashDecay * dt);
	tintA = std::max(0.f, tintA - tintDecay * dt);
	if(cut.kind != Cutin::NONE) {
		cut.t += dt;
		cut.emberAcc += dt;
		CutinBeats();
		if(cut.t >= cut.dur)
			cut = CutinState{};
	}
}

core::vector3df CameraOffset() {
	// pixels -> field units, roughly (the field is ~9 units across ~1000 px)
	return { shakeNow.X * 0.006f, -shakeNow.Y * 0.006f, 0.f };
}

namespace {
void DrawPulses(scene::ISceneCollisionManager* coll) {
	const float s = S();
	for(auto& p : pulses) {
		core::vector2df at = p.pos2;
		if(!p.screen) {
			const auto sc = coll->getScreenCoordinatesFrom3DPosition(p.pos);
			if(sc.X < -1000) continue;
			at = core::vector2df((float)sc.X, (float)sc.Y);
		}
		const float k = p.t / p.dur;
		const float sz = p.screen ? p.size : p.size * s;
		switch(p.kind) {
		case Pulse::RING: {
			const float r = sz * outCubic(k);
			Quad(tRing, at, r, r, 0.f, addc(1.f - k, p.color), true);
			break;
		}
		case Pulse::DUST: {
			const float r = sz * (0.6f + 0.6f * outCubic(k));
			Quad(tGlow, at, r * 1.6f, r * 0.7f, 0.f, col(0.45f * (1.f - k), p.color), false);
			break;
		}
		case Pulse::FLASH: {
			const float r = sz * (0.5f + 0.7f * outCubic(k));
			Quad(tGlow, at, r, r, 0.f, addc((1.f - k) * 1.2f, p.color), true);
			break;
		}
		case Pulse::STAMP: {
			const float t = p.t;
			const float pop = t < 220.f ? 2.4f - 1.4f * outBack(t / 220.f) : 1.f;
			const float a = t < 520.f ? 1.f : 1.f - (t - 520.f) / (p.dur - 520.f);
			const float g = 110.f * s * pop;
			Quad(tGlow, at, g, g, 0.f, addc(a, p.color), true);
			Quad(tRing, at, g * 0.78f, g * 0.78f, t * 0.004f, addc(a, { 255, 255, 255, 255 }), true);
			if(fNum) {
				const core::ustring txt(epro::format(L"{}", p.num));
				const auto d = fNum->getDimensionustring(txt);
				const s32 dw = (s32)d.Width, dh = (s32)d.Height;
				const s32 x = (s32)at.X - dw / 2, y = (s32)at.Y - dh / 2;
				const auto shadow = col(a, { 255, 0, 0, 0 });
				fNum->drawustring(txt, core::rect<s32>(x + 3, y + 3, x + dw + 3, y + dh + 3), shadow);
				fNum->drawustring(txt, core::rect<s32>(x, y, x + dw, y + dh), col(a, { 255, 255, 255, 255 }));
			}
			break;
		}
		}
	}
}

void SpawnBursts(scene::ISceneCollisionManager* coll) {
	for(auto& b : bursts) {
		const auto sc = coll->getScreenCoordinatesFrom3DPosition(b.pos);
		if(sc.X < -1000) continue;
		SparkBurst({ (float)sc.X, (float)sc.Y }, b.n, b.color, b.speed, b.size, b.gravity, b.life);
	}
	bursts.clear();
}

void DrawSparks() {
	for(auto& sp : sparks) {
		const float k = sp.life / sp.max;
		const float a = k < 0.15f ? k / 0.15f : 1.f - (k - 0.15f) / 0.85f;
		Quad(tGlow, sp.p + shakeNow, sp.size, sp.size, 0.f, addc(a, sp.color), true);
	}
}

void Banner(float t, float in, float out, const std::wstring& text, video::SColor c, float yFrac) {
	if(!fBanner || t < in || t > out) return;
	const auto ss = drv->getScreenSize();
	const float s = S();
	const float slide = outQuint((t - in) / 260.f);
	const float fade = 1.f - smooth(out - 260.f, out, t);
	const s32 h = (s32)(64.f * s), y = (s32)(ss.Height * yFrac) - h / 2;
	const s32 right = (s32)(ss.Width * slide);
	const video::SColor dark = col(0.82f * fade, { 255, 0, 0, 0 }), clear = col(0.f, { 255, 0, 0, 0 });
	drv->draw2DRectangle(core::rect<s32>(0, y, right, y + h), dark, clear, dark, clear);
	drv->draw2DRectangle(core::rect<s32>(0, y - (s32)(2 * s), right, y), col(fade, c), col(0.f, c), col(fade, c), col(0.f, c));
	drv->draw2DRectangle(core::rect<s32>(0, y + h, right, y + h + (s32)(2 * s)), col(fade, c), col(0.f, c), col(fade, c), col(0.f, c));
	const core::ustring txt(text);
	const auto d = fBanner->getDimensionustring(txt);
	const s32 dw = (s32)d.Width, dh = (s32)d.Height;
	const s32 tx = (s32)(-(float)dw + (40.f * s + (float)dw) * slide), ty = y + (h - dh) / 2;
	fBanner->drawustring(txt, core::rect<s32>(tx + 3, ty + 3, tx + 3 + dw, ty + 3 + dh), col(fade, { 255, 0, 0, 0 }));
	fBanner->drawustring(txt, core::rect<s32>(tx, ty, tx + dw, ty + dh), col(fade, { 255, 255, 255, 255 }));
}

void DrawGodCutin() {
	const float t = cut.t, D = cut.dur, s = S();
	const auto ss = drv->getScreenSize();
	const auto c = Center() + shakeNow;
	const float out = smooth(D - 520.f, D, t);
	// darken the field, tinted at the edges
	const float dim = smooth(0.f, 320.f, t) * (1.f - out);
	drv->draw2DRectangle(col(0.86f * dim, { 255, 4, 4, 10 }), core::rect<s32>(0, 0, (s32)ss.Width, (s32)ss.Height));
	Quad(tGlow, c, ss.Width * 1.5f, ss.Height * 1.9f, 0.f, addc(0.55f * dim, cut.color), true);
	// rays
	const float rays = smooth(200.f, 760.f, t) * (1.f - out);
	const float big = (float)std::max(ss.Width, ss.Height) * 1.6f;
	Quad(tRays, c, big, big, t * 0.00022f, addc(0.85f * rays, cut.color), true);
	Quad(tRays, c, big * 0.8f, big * 0.8f, -t * 0.00035f + 0.4f, addc(0.5f * rays, { 255, 255, 255, 255 }), true);
	if(cut.kind == Cutin::RA)
		Quad(tGlow, c, 720.f * s, 720.f * s, 0.f, addc(rays, { 255, 255, 220, 120 }), true);
	// artwork: drops in from large, settles, slowly pushes in, then blows out
	if(t > 330.f) {
		auto* art = Art(cut.code);
		if(art) {
			const float in = outQuint((t - 330.f) / 430.f);
			float scale = 2.1f - 1.1f * in + 0.07f * clamp01((t - 760.f) / (D - 1280.f)) + 0.45f * out;
			const float a = smooth(330.f, 520.f, t) * (1.f - out);
			const float h = 360.f * s * scale, w = h;
			Quad(tGlow, c, w * 1.55f, h * 1.55f, 0.f, addc(a * 0.9f, cut.color), true);
			Quad(art, ArtBox(art), c, w, h, 0.f, col(a, { 255, 255, 255, 255 }), false);
			// edge light
			Quad(tRing, c, w * 1.42f, h * 1.42f, t * 0.0006f, addc(a * 0.6f, cut.color), true);
		}
	}
	Banner(t, 900.f, D - 380.f, cut.name, cut.color, 0.83f);
}

void DrawExodia() {
	const float t = cut.t, D = cut.dur, s = S();
	const auto ss = drv->getScreenSize();
	const auto c = Center() + shakeNow;
	const float out = smooth(D - 520.f, D, t);
	const float dim = smooth(0.f, 300.f, t) * (1.f - out);
	drv->draw2DRectangle(col(0.9f * dim, { 255, 6, 4, 2 }), core::rect<s32>(0, 0, (s32)ss.Width, (s32)ss.Height));
	const float glow = smooth(2150.f, 2600.f, t) * (1.f - out);
	const float big = (float)std::max(ss.Width, ss.Height) * 1.7f;
	Quad(tGlow, c, ss.Width * 1.4f, ss.Height * 1.8f, 0.f, addc(0.3f * dim + 0.5f * glow, cut.color), true);
	Quad(tRays, c, big, big, t * 0.0002f, addc(0.9f * glow, cut.color), true);
	// the five pieces: right leg, left leg, right arm, left arm, head (as the player sees them)
	const core::vector2df at[5] = { { -0.16f, 0.26f }, { 0.16f, 0.26f }, { -0.36f, 0.02f }, { 0.36f, 0.02f }, { 0.f, -0.06f } };
	const float H = (float)ss.Height;
	for(int i = 0; i < 5; ++i) {
		const float t0 = 300.f + i * 340.f;
		if(t < t0) continue;
		auto* art = Art(EXODIA_PIECES[i]);
		if(!art) continue;
		const float in = outQuint((t - t0) / 260.f);
		const float converge = smooth(2150.f, 2600.f, t);
		const float scale = (1.7f - 0.7f * in) * (i == 4 ? 1.25f : 1.f) * (1.f + 0.08f * converge) + 0.4f * out;
		const float a = smooth(t0, t0 + 120.f, t) * (1.f - out);
		const core::vector2df p = c + core::vector2df(at[i].X * H, at[i].Y * H) * (1.f - 0.18f * converge);
		const float h = 210.f * s * scale, w = h * 0.686f;
		Quad(tGlow, p, w * 1.6f, h * 1.4f, 0.f, addc(a * (0.5f + 0.5f * glow), cut.color), true);
		const auto sz = art->getOriginalSize();
		Quad(art, core::rect<s32>(0, 0, sz.Width, sz.Height), p, w, h, 0.f, col(a, { 255, 255, 255, 255 }), false);
		if(t - t0 < 40.f && t - t0 >= 0.f)
			Ring2D(p, 380.f * s, 450.f, cut.color);
	}
	Banner(t, 2700.f, D - 380.f, cut.name, cut.color, 0.86f);
}

void DrawOrichalcos() {
	const float t = cut.t, D = cut.dur, s = S();
	const auto ss = drv->getScreenSize();
	const auto c = Center() + shakeNow;
	const float out = smooth(D - 500.f, D, t);
	const float dim = smooth(0.f, 300.f, t) * (1.f - out);
	drv->draw2DRectangle(col(0.7f * dim, { 255, 0, 10, 4 }), core::rect<s32>(0, 0, (s32)ss.Width, (s32)ss.Height));
	const float grow = outQuint(t / 900.f);
	const float sz = (float)ss.Height * (0.25f + 0.85f * grow) * (1.f + 0.3f * out);
	Quad(tGlow, c, sz * 1.5f, sz * 1.5f, 0.f, addc(0.6f * dim, cut.color), true);
	Quad(tSeal, c, sz, sz, -0.9f + 0.9f * grow + t * 0.00008f, addc(1.f - out, cut.color), true);
	Quad(tSeal, c, sz * 1.02f, sz * 1.02f, -0.9f + 0.9f * grow + t * 0.00008f, addc(0.5f * (1.f - out), { 255, 255, 255, 255 }), true);
	Banner(t, 700.f, D - 300.f, cut.name, cut.color, 0.86f);
}

void DrawAce() {
	const float t = cut.t, D = cut.dur, s = S();
	const auto ss = drv->getScreenSize();
	const auto c = Center() + shakeNow;
	const float out = smooth(D - 380.f, D, t);
	const float dim = smooth(0.f, 200.f, t) * (1.f - out);
	drv->draw2DRectangle(col(0.55f * dim, { 255, 0, 0, 0 }), core::rect<s32>(0, 0, (s32)ss.Width, (s32)ss.Height));
	// a band of light across the middle
	const float bandH = 300.f * s;
	Quad(tStreak, c, ss.Width * 2.2f, bandH, 0.f, addc(0.7f * dim, cut.color), true);
	auto* art = Art(cut.code);
	if(art) {
		const float in = outQuint(t / 330.f);
		const float x = c.X + (1.f - in) * ss.Width * 0.7f - out * ss.Width * 0.9f;
		const float h = 270.f * s * (1.f + 0.05f * clamp01(t / D)), w = h;
		const float a = smooth(0.f, 120.f, t) * (1.f - out);
		// motion streaks behind it while it flies in
		for(int i = 1; i <= 3; ++i)
			Quad(tStreak, { x + i * 90.f * s * (1.f - in), c.Y }, w * 2.f, h * 0.5f, 0.f, addc(a * (1.f - in) * 0.6f, cut.color), true);
		Quad(tGlow, { x, c.Y }, w * 1.5f, h * 1.5f, 0.f, addc(a * 0.8f, cut.color), true);
		Quad(art, ArtBox(art), { x, c.Y }, w, h, 0.f, col(a, { 255, 255, 255, 255 }), false);
	}
	Banner(t, 300.f, D - 160.f, cut.name, cut.color, 0.82f);
}
} // namespace

void Draw(scene::ISceneManager* smgr) {
	if(!drv || !smgr) return;
	auto* coll = smgr->getSceneCollisionManager();
	SpawnBursts(coll);
	DrawPulses(coll);
	switch(cut.kind) {
	case Cutin::OBELISK: case Cutin::SLIFER: case Cutin::RA: DrawGodCutin(); break;
	case Cutin::EXODIA_WIN: DrawExodia(); break;
	case Cutin::ORICHALCOS: DrawOrichalcos(); break;
	case Cutin::ACE: DrawAce(); break;
	default: break;
	}
	DrawSparks();
	const auto ss = drv->getScreenSize();
	if(tintA > 0.f) {
		// red vignette when you take damage
		const auto edge = col(tintA, tintCol), mid = col(0.f, tintCol);
		const s32 bw = (s32)ss.Width / 5, bh = (s32)ss.Height / 5, W = (s32)ss.Width, H = (s32)ss.Height;
		drv->draw2DRectangle(core::rect<s32>(0, 0, bw, H), edge, mid, edge, mid);
		drv->draw2DRectangle(core::rect<s32>(W - bw, 0, W, H), mid, edge, mid, edge);
		drv->draw2DRectangle(core::rect<s32>(0, 0, W, bh), edge, edge, mid, mid);
		drv->draw2DRectangle(core::rect<s32>(0, H - bh, W, H), mid, mid, edge, edge);
	}
	if(flashA > 0.f)
		drv->draw2DRectangle(col(flashA, flashCol), core::rect<s32>(0, 0, (s32)ss.Width, (s32)ss.Height));
}

}
