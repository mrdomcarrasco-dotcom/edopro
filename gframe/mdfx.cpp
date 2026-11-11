// Progression "duel polish" layer. See mdfx.h. Beat timings follow yugioh-progression docs/MD-STUDY.md.
#include "mdfx.h"
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <map>
#include <random>
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
Fonts F;
video::ITexture* tGlow = nullptr;    // soft radial glow
video::ITexture* tRing = nullptr;    // thin ring
video::ITexture* tRays = nullptr;    // radial spikes
video::ITexture* tStreak = nullptr;  // horizontal light streak
video::ITexture* tSpark = nullptr;   // 4-point sparkle
video::ITexture* tStar = nullptr;    // 5-point level star
video::ITexture* tShard = nullptr;   // angular shard
video::ITexture* tSeal = nullptr;    // circle + five-point star
video::ITexture* tLink = nullptr;    // chain link (an oval ring)
bool enabled = true;
std::map<uint32_t, Cutin> listed;    // mdfx.conf
std::map<uint32_t, video::ITexture*> artCache;
std::mt19937 rng{ 20261110u };

float frand(float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); }
float hash(int i) { float x = std::sin(i * 12.9898f + 78.233f) * 43758.5453f; return x - std::floor(x); }
float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }
float smooth(float a, float b, float x) { float t = clamp01((x - a) / (b - a)); return t * t * (3.f - 2.f * t); }
float outCubic(float t) { t = clamp01(t); float u = 1.f - t; return 1.f - u * u * u; }
float outQuint(float t) { t = clamp01(t); float u = 1.f - t; return 1.f - u * u * u * u * u; }
float window(float t, float a, float b, float fin, float fout) { return smooth(a, a + fin, t) * (1.f - smooth(b - fout, b, t)); }
video::SColor col(float a, const video::SColor& c) {
	return video::SColor((u32)(clamp01(a) * 255.f), c.getRed(), c.getGreen(), c.getBlue());
}
// additive textures: rgb carries the intensity, so fade with the vertex rgb
video::SColor addc(float a, const video::SColor& c) {
	a = clamp01(a);
	return video::SColor(255, (u32)(c.getRed() * a), (u32)(c.getGreen() * a), (u32)(c.getBlue() * a));
}
video::SColor lerpc(const video::SColor& a, const video::SColor& b, float k) {
	k = clamp01(k);
	return video::SColor(255, (u32)(a.getRed() + (b.getRed() - (float)a.getRed()) * k), (u32)(a.getGreen() + (b.getGreen() - (float)a.getGreen()) * k),
						 (u32)(a.getBlue() + (b.getBlue() - (float)a.getBlue()) * k));
}

template<typename Fn>
video::ITexture* MakeTex(const char* name, u32 w, u32 h, Fn f) {
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
float StarDist(float u, float v, int points, float inner, float outer, float rot) {
	const float r = std::sqrt(u * u + v * v);
	float a = std::atan2(v, u) - rot;
	const float seg = 2.f * PI / points;
	a = std::fmod(std::fmod(a, seg) + seg, seg);
	const float k = std::fabs(a - seg / 2.f) / (seg / 2.f); // 1 at tips, 0 between
	return r - (inner + (outer - inner) * std::pow(k, 1.6f));
}

void BuildTextures() {
	tGlow = MakeTex("mdfx_glow", 128, 128, [](float u, float v) {
		const float a = 1.f - std::sqrt(u * u + v * v);
		return a <= 0.f ? 0.f : a * a;
	});
	tRing = MakeTex("mdfx_ring", 256, 256, [](float u, float v) {
		const float d = (std::sqrt(u * u + v * v) - 0.86f) / 0.06f;
		return std::exp(-d * d);
	});
	tRays = MakeTex("mdfx_rays", 256, 256, [](float u, float v) {
		const float r = std::sqrt(u * u + v * v);
		if(r > 1.f) return 0.f;
		const float ang = std::atan2(v, u);
		float ray = 0.5f + 0.5f * std::cos(ang * 12.f) * (0.75f + 0.25f * std::cos(ang * 5.f + 1.3f));
		ray = std::pow(clamp01(ray), 10.f);
		return ray * (1.f - r) * 1.3f + std::pow(1.f - r, 6.f) * 0.5f;
	});
	tStreak = MakeTex("mdfx_streak", 256, 32, [](float u, float v) {
		const float along = 1.f - std::fabs(u);
		return std::exp(-(v * v) / 0.05f) * along * along;
	});
	tSpark = MakeTex("mdfx_spark", 64, 64, [](float u, float v) {
		const float a = std::exp(-std::fabs(u) * 18.f) * (1.f - std::fabs(v));
		const float b = std::exp(-std::fabs(v) * 18.f) * (1.f - std::fabs(u));
		const float c = std::max(0.f, 1.f - std::sqrt(u * u + v * v) * 3.f);
		return a + b + c * c;
	});
	tStar = MakeTex("mdfx_star", 64, 64, [](float u, float v) {
		const float d = StarDist(u, v, 5, 0.38f, 0.92f, -PI / 2.f);
		return clamp01(0.5f - d * 30.f) + 0.35f * std::exp(-std::max(d, 0.f) * 12.f);
	});
	tShard = MakeTex("mdfx_shard", 64, 64, [](float u, float v) {
		const float w = (1.f - (u + 1.f) / 2.f) * 0.7f;
		return (std::fabs(v) < w && u > -0.95f && u < 0.95f) ? 1.f : 0.f;
	});
	tSeal = MakeTex("mdfx_seal", 512, 512, [](float u, float v) {
		const float r = std::sqrt(u * u + v * v);
		float d = std::min(std::fabs(r - 0.92f), std::fabs(r - 0.84f));
		float px[5], py[5];
		for(int i = 0; i < 5; ++i) {
			const float a = -PI / 2.f + i * 2.f * PI / 5.f;
			px[i] = std::cos(a) * 0.84f;
			py[i] = std::sin(a) * 0.84f;
		}
		for(int i = 0; i < 5; ++i)
			d = std::min(d, SegDist(u, v, px[i], py[i], px[(i + 2) % 5], py[(i + 2) % 5]));
		return std::exp(-(d * d) / 0.00018f) + std::exp(-(d * d) / 0.004f) * 0.45f;
	});
	tLink = MakeTex("mdfx_link", 64, 32, [](float u, float v) {
		const float d = (std::sqrt(u * u + v * v) - 0.72f) / 0.16f;
		return std::exp(-d * d);
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
	const float tw = (float)tsz.Width, th = (float)tsz.Height;
	const core::vector2df uv[4] = {
		{ src.UpperLeftCorner.X / tw, src.UpperLeftCorner.Y / th },
		{ src.LowerRightCorner.X / tw, src.UpperLeftCorner.Y / th },
		{ src.UpperLeftCorner.X / tw, src.LowerRightCorner.Y / th },
		{ src.LowerRightCorner.X / tw, src.LowerRightCorner.Y / th } };
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
// a glowing line segment (lightning, tendrils)
void Line(core::vector2df a, core::vector2df b, float width, video::SColor c) {
	const auto d = b - a;
	const float len = (float)d.getLength();
	if(len < 0.5f) return;
	Quad(tStreak, (a + b) * 0.5f, len * 1.15f, width, std::atan2(d.Y, d.X), c, true);
}
void FillRect(s32 x0, s32 y0, s32 x1, s32 y1, video::SColor c) { drv->draw2DRectangle(c, core::rect<s32>(x0, y0, x1, y1)); }

video::ITexture* LoadFile(const epro::path_string& file) {
	if(!Utils::FileExists(file)) return nullptr;
	return drv->getTexture({ file.data(), static_cast<u32>(file.size()) });
}
uint32_t BaseCode(uint32_t code) {
	const auto* data = gDataManager->GetCardData(code);
	return (data && data->alias && data->alias != code) ? data->alias : code;
}
// high-resolution pictures the Progression app installs for the scene cards (tools/mdfx_art.py)
video::ITexture* HiRes(uint32_t code, bool full) {
	const auto dir = full ? EPRO_TEXT("./pics/mdfx/full/") : EPRO_TEXT("./pics/mdfx/");
	if(auto* t = LoadFile(epro::format(EPRO_TEXT("{}{}.jpg"), dir, code))) return t;
	const auto base = BaseCode(code);
	return base != code ? LoadFile(epro::format(EPRO_TEXT("{}{}.jpg"), dir, base)) : nullptr;
}
video::ITexture* Art(uint32_t code) {
	auto it = artCache.find(code);
	if(it != artCache.end())
		return it->second;
	video::ITexture* tex = HiRes(code, true);
	if(tex) { artCache[code] = tex; return tex; }
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
core::rect<s32> Full(video::ITexture* tex) { const auto s = tex->getOriginalSize(); return core::rect<s32>(0, 0, s.Width, s.Height); }
// the illustration box of a full card picture
core::rect<s32> ArtBox(video::ITexture* tex) {
	const auto s = tex->getOriginalSize();
	return core::rect<s32>((s32)(s.Width * 0.123f), (s32)(s.Height * 0.185f), (s32)(s.Width * 0.877f), (s32)(s.Height * 0.703f));
}
// the illustration: a high-resolution art crop if installed, else cut out of the card picture
std::map<uint32_t, std::pair<video::ITexture*, core::rect<s32>>> illoCache;
std::pair<video::ITexture*, core::rect<s32>> Illustration(uint32_t code) {
	auto it = illoCache.find(code);
	if(it != illoCache.end()) return it->second;
	std::pair<video::ITexture*, core::rect<s32>> r{ nullptr, {} };
	if(auto* crop = HiRes(code, false)) r = { crop, Full(crop) };
	else if(auto* card = Art(code)) r = { card, ArtBox(card) };
	illoCache[code] = r;
	return r;
}

void PlayFx(const char* name) {
	if(gSoundManager)
		gSoundManager->PlayFx(name);
}

float S() { return drv->getScreenSize().Height / 640.f; }
float SW() { return (float)drv->getScreenSize().Width; }
float SH() { return (float)drv->getScreenSize().Height; }
core::vector2df Center() { return { SW() / 2.f, SH() / 2.f }; }

core::dimension2d<u32> TextSize(gui::CGUITTFont* f, const std::wstring& s) {
	return f ? f->getDimensionustring(core::ustring(s)) : core::dimension2d<u32>(0, 0);
}
// text with a dark outline
void Text(gui::CGUITTFont* f, const std::wstring& s, float x, float y, video::SColor c, float outlineA, int outline = 2, const core::rect<s32>* clip = nullptr) {
	if(!f || s.empty()) return;
	const core::ustring u(s);
	const auto d = f->getDimensionustring(u);
	const s32 X = (s32)x, Y = (s32)y, W = (s32)d.Width, H = (s32)d.Height;
	if(outlineA > 0.f && outline > 0) {
		const auto oc = col(outlineA, { 255, 0, 0, 0 });
		for(int dx = -outline; dx <= outline; dx += outline)
			for(int dy = -outline; dy <= outline; dy += outline)
				if(dx || dy)
					f->drawustring(u, core::rect<s32>(X + dx, Y + dy, X + dx + W, Y + dy + H), oc, false, false, clip);
	}
	f->drawustring(u, core::rect<s32>(X, Y, X + W, Y + H), c, false, false, clip);
}

// --- state -----------------------------------------------------------------------------------
struct Spark {
	core::vector2df p, v;
	float life, max, size, gravity, rot, spin;
	video::SColor color;
	uint8_t kind; // 0 glow, 1 sparkle, 2 shard (alpha blended), 3 dust (alpha blended)
};
std::vector<Spark> sparks;

struct Fx {
	enum Kind : uint8_t { ACT_SPELL, ACT_MONSTER, SUMMON_FLASH, DAMAGE_RING, DESTROY, RESOLVE_GLOW } kind;
	core::vector3df pos;
	float t, dur;
	bool spawned;
};
std::vector<Fx> fxs;

float shakePx = 0.f, shakeT = 0.f, shakeDur = 1.f;
core::vector2df shakeNow;
float dmgFrame = 0.f; // crimson frame, ms left
float dimT = -1.f;    // activation dim

struct Overlay {
	enum Kind : uint8_t { NONE, CUTIN, EXODIA, SEAL, CHAIN, RESOLVE, BANNER } kind = NONE;
	float t = 0.f, dur = 0.f;
	uint32_t code = 0;
	Cutin cut = Cutin::NONE;
	bool opp = false, first = false, mine = false, turn = false;
	int resolving = 0, beat = 0;
	std::vector<uint32_t> codes;
	std::wstring name, stats, text;
	int stars = 0;
	video::SColor tint, plate;
	float acc = 0.f;
	std::vector<std::pair<core::vector2df, core::vector2df>> bolts;
} ov;

video::SColor AttributeTint(uint32_t code) {
	const auto* d = gDataManager->GetCardData(code);
	const uint32_t a = d ? d->attribute : 0;
	if(a & 0x01) return { 255, 230, 170, 90 };  // EARTH
	if(a & 0x02) return { 255, 90, 170, 255 };  // WATER
	if(a & 0x04) return { 255, 255, 120, 60 };  // FIRE
	if(a & 0x08) return { 255, 110, 240, 140 }; // WIND
	if(a & 0x10) return { 255, 255, 220, 110 }; // LIGHT
	if(a & 0x20) return { 255, 190, 110, 255 }; // DARK
	return { 255, 255, 210, 90 };               // DIVINE
}

std::wstring StatText(int v) { return v == -2 ? std::wstring(L"?") : epro::format(L"{}", v); }

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

int Frames(float ms) { return (int)std::ceil(ms * 60.f / 1000.f) + 2; }

void Burst(core::vector2df at, int n, video::SColor c, float speed, float size, float life, uint8_t kind = 0, float gravity = 0.f) {
	const float s = S();
	for(int i = 0; i < n; ++i) {
		const float a = frand(0.f, 2.f * PI), sp = frand(0.25f, 1.f) * speed * s;
		sparks.push_back({ at, core::vector2df(std::cos(a) * sp, std::sin(a) * sp), 0.f, life * frand(0.6f, 1.f), size * s * frand(0.5f, 1.f),
						   gravity * s, frand(0.f, 6.28f), frand(-8.f, 8.f), c, kind });
	}
}

} // namespace

// --- public API --------------------------------------------------------------------------------
void Init(video::IVideoDriver* driver, const Fonts& fonts) {
	drv = driver;
	F = fonts;
	BuildTextures();
	LoadConf();
}
bool Enabled() { return enabled && drv; }

float EaseMove(float t) { return enabled ? outQuint(t) : t; }
float MoveLift(float distance) { return (!enabled || distance < 0.6f) ? 0.f : std::min(0.5f, distance * 0.08f); }
float MoveTimeScale() { return enabled ? 1.3f : 1.f; }
int LpRollFrames() { return enabled ? 60 : 10; }
void Pop(float& from, float& t, float& dur, float scale, float ms) {
	if(!Enabled()) return;
	from = scale;
	t = 0.f;
	dur = ms;
}
float PopScale(float from, float t, float dur) {
	if(dur <= 0.f || t >= dur) return 1.f;
	return from + (1.f - from) * outCubic(t / dur);
}

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

int StartCutin(Cutin kind, uint32_t code, bool opponent) {
	if(!Enabled() || kind == Cutin::NONE) return 0;
	ov = Overlay{};
	ov.code = code;
	ov.cut = kind;
	ov.opp = opponent;
	{ const auto nm = gDataManager->GetName(code); ov.name.assign(nm.data(), nm.size()); }
	const auto paren = ov.name.find(L" (");
	if(paren != std::wstring::npos) ov.name.erase(paren);
	if(kind == Cutin::ORICHALCOS) {
		ov.kind = Overlay::SEAL;
		ov.dur = 2000.f;
		ov.tint = { 255, 60, 255, 120 };
		PlayFx("seal");
		return Frames(ov.dur);
	}
	ov.kind = Overlay::CUTIN;
	ov.dur = 1450.f;
	if(const auto* d = gDataManager->GetCardData(code)) {
		ov.stars = (int)(d->level & 0xff);
		ov.stats = epro::format(L"ATK {}    DEF {}", StatText(d->attack), StatText(d->defense));
	}
	switch(kind) {
	case Cutin::OBELISK: ov.tint = { 255, 110, 170, 255 }; ov.plate = { 255, 20, 40, 110 }; break;
	case Cutin::SLIFER: ov.tint = { 255, 255, 110, 90 }; ov.plate = { 255, 120, 14, 24 }; break;
	case Cutin::RA: ov.tint = { 255, 255, 214, 100 }; ov.plate = { 255, 20, 40, 110 }; break;
	default: ov.tint = AttributeTint(code); ov.plate = { 255, 20, 40, 110 }; break;
	}
	PlayFx("cutin");
	return Frames(ov.dur);
}

int ExodiaWin() {
	if(!Enabled()) return 0;
	ov = Overlay{};
	ov.kind = Overlay::EXODIA;
	ov.dur = 10500.f;
	ov.tint = { 255, 255, 170, 60 };
	PlayFx("exodia");
	return Frames(ov.dur);
}

void Activate(const core::vector3df& pos, bool monster) {
	if(!Enabled()) return;
	fxs.push_back({ monster ? Fx::ACT_MONSTER : Fx::ACT_SPELL, pos, 0.f, monster ? 780.f : 950.f, false });
	if(!monster) dimT = 0.f;
}

int ChainDisplay(const std::vector<uint32_t>& codes) {
	if(!Enabled() || codes.size() < 2) return 0;
	ov = Overlay{};
	ov.kind = Overlay::CHAIN;
	ov.dur = 1100.f;
	ov.codes = codes;
	ov.text = L"CHAIN";
	PlayFx(epro::format("chain{}", std::min<size_t>(codes.size(), 6)).data());
	return Frames(ov.dur);
}

int ResolveDisplay(const std::vector<uint32_t>& codes, int resolving, bool first) {
	if(!Enabled() || codes.size() < 2) return 0;
	ov = Overlay{};
	ov.kind = Overlay::RESOLVE;
	ov.first = first;
	ov.dur = first ? 1400.f : 650.f;
	ov.codes = codes;
	ov.resolving = resolving;
	ov.text = L"RESOLVE";
	PlayFx(first ? "resolve" : "resolve_link");
	return Frames(ov.dur);
}

void ResolveGlow(const core::vector3df& pos) {
	if(!Enabled()) return;
	fxs.push_back({ Fx::RESOLVE_GLOW, pos, 0.f, 450.f, false });
	PlayFx("resolve_link");
}

void SummonFlash(const core::vector3df& pos, bool special) {
	if(!Enabled() || !special) return;
	fxs.push_back({ Fx::SUMMON_FLASH, pos, 0.f, 500.f, false });
}

void Damage(int amount, bool self, const core::vector3df& at) {
	if(!Enabled() || amount <= 0) return;
	if(self) dmgFrame = 1200.f;
	else fxs.push_back({ Fx::DAMAGE_RING, at, 0.f, 600.f, false });
	PlayFx("lproll");
}

int Destroy(const core::vector3df& pos) {
	if(!Enabled()) return 0;
	fxs.push_back({ Fx::DESTROY, pos, 0.f, 700.f, false });
	return Frames(300.f);
}

int PhaseBanner(const std::wstring& text, bool myTurn, bool turnChange) {
	if(!Enabled()) return 0;
	if(ov.kind != Overlay::NONE && ov.kind != Overlay::BANNER) return 0;
	ov = Overlay{};
	ov.kind = Overlay::BANNER;
	ov.dur = 950.f;
	ov.text = text;
	for(auto& ch : ov.text) ch = (wchar_t)std::towupper(ch);
	ov.mine = myTurn;
	ov.turn = turnChange;
	return Frames(ov.dur) - 4;
}

void Clear() {
	sparks.clear();
	fxs.clear();
	ov = Overlay{};
	shakePx = 0.f;
	dmgFrame = 0.f;
	dimT = -1.f;
}

// --- per frame -------------------------------------------------------------------------------
void Update(uint32_t delta_ms) {
	if(!drv) return;
	const float dt = (float)delta_ms;
	for(auto& f : fxs) f.t += dt;
	fxs.erase(std::remove_if(fxs.begin(), fxs.end(), [](const Fx& f) { return f.t >= f.dur; }), fxs.end());
	for(auto& sp : sparks) {
		sp.life += dt;
		sp.v.Y += sp.gravity * dt / 1000.f;
		sp.v *= std::pow(0.986f, dt / 16.f);
		sp.p += sp.v * (dt / 1000.f);
		sp.rot += sp.spin * dt / 1000.f;
	}
	sparks.erase(std::remove_if(sparks.begin(), sparks.end(), [](const Spark& s) { return s.life >= s.max; }), sparks.end());
	if(sparks.size() > 1800) sparks.erase(sparks.begin(), sparks.begin() + (sparks.size() - 1800));
	if(shakePx > 0.f) {
		shakeT += dt;
		const float k = 1.f - clamp01(shakeT / shakeDur);
		shakeNow = core::vector2df(frand(-1.f, 1.f), frand(-1.f, 1.f)) * shakePx * k * k;
		if(shakeT >= shakeDur) { shakePx = 0.f; shakeNow = core::vector2df(0.f, 0.f); }
	}
	dmgFrame = std::max(0.f, dmgFrame - dt);
	if(dimT >= 0.f) { dimT += dt; if(dimT > 950.f) dimT = -1.f; }
	if(ov.kind != Overlay::NONE) {
		ov.t += dt;
		ov.acc += dt;
		if(ov.t >= ov.dur) ov = Overlay{};
	}
}

core::vector3df CameraOffset() {
	return { shakeNow.X * 0.006f, -shakeNow.Y * 0.006f, 0.f };
}

namespace {

// ---------------------------------------------------------------- card-attached effects
void DrawFx(scene::ISceneCollisionManager* coll) {
	const float s = S();
	for(auto& f : fxs) {
		const auto sc = coll->getScreenCoordinatesFrom3DPosition(f.pos);
		if(sc.X < -1000) continue;
		const core::vector2df at((float)sc.X, (float)sc.Y);
		const float t = f.t;
		switch(f.kind) {
		case Fx::ACT_SPELL: {
			// cyan-white bloom on the card, peaking ~300 ms
			const float a = smooth(0.f, 300.f, t) * (1.f - smooth(f.dur - 180.f, f.dur, t));
			const float pulse = 0.85f + 0.15f * std::sin(t * 0.018f);
			Quad(tGlow, at, 210.f * s, 250.f * s, 0.f, addc(a * pulse, { 255, 120, 230, 255 }), true);
			Quad(tGlow, at, 110.f * s, 140.f * s, 0.f, addc(a * 0.8f, { 255, 255, 255, 255 }), true);
			if(t < 380.f) {
				const float r = (80.f + 160.f * outCubic(t / 380.f)) * s;
				Quad(tRing, at, r, r, 0.f, addc(1.f - t / 380.f, { 255, 160, 240, 255 }), true);
			}
			break;
		}
		case Fx::ACT_MONSTER: {
			// gold disc + ring + white spikes: grow 150, hold 300, fade 300
			const float g = outCubic(t / 150.f);
			const float a = 1.f - smooth(450.f, 780.f, t);
			const float r = 190.f * s * g;
			Quad(tGlow, at, r * 1.3f, r * 1.3f, 0.f, addc(a * 0.9f, { 255, 255, 200, 90 }), true);
			Quad(tRays, at, r * 1.6f, r * 1.6f, t * 0.0012f, addc(a, { 255, 255, 255, 230 }), true);
			Quad(tRing, at, r, r, 0.f, addc(a, { 255, 255, 220, 120 }), true);
			if(!f.spawned) { Burst(at, 14, { 255, 255, 220, 120 }, 260.f, 12.f, 600.f, 1); f.spawned = true; }
			break;
		}
		case Fx::SUMMON_FLASH: {
			// white silhouette ~100 ms, then cyan, then back to the art over ~400 ms
			const float a = t < 100.f ? 1.f : 1.f - smooth(100.f, 500.f, t);
			const video::SColor c = lerpc({ 255, 255, 255, 255 }, { 255, 90, 220, 255 }, smooth(60.f, 200.f, t));
			Quad(tGlow, at, 200.f * s, 250.f * s, 0.f, addc(a * 1.3f, c), true);
			break;
		}
		case Fx::DAMAGE_RING: {
			const float k = outCubic(t / 330.f);
			Quad(tRing, at, 260.f * s * k, 260.f * s * k, 0.f, addc(1.f - smooth(200.f, 600.f, t), { 255, 255, 210, 90 }), true);
			if(!f.spawned) { Burst(at, 18, { 255, 255, 220, 120 }, 300.f, 12.f, 600.f, 1); f.spawned = true; }
			break;
		}
		case Fx::DESTROY: {
			// 0-60 flash, 60-300 shards + cyan streaks, 300-700 dust
			if(t < 120.f) Quad(tGlow, at, 230.f * s, 290.f * s, 0.f, addc(1.f - t / 120.f, { 255, 200, 250, 255 }), true);
			if(!f.spawned && t >= 60.f) {
				Burst(at, 16, { 255, 30, 34, 44 }, 520.f, 26.f, 520.f, 2, 300.f);
				Burst(at, 10, { 255, 120, 230, 255 }, 700.f, 10.f, 380.f, 0);
				for(int i = 0; i < 6; ++i)
					sparks.push_back({ at + core::vector2df(frand(-30.f, 30.f), frand(-30.f, 30.f)) * s, core::vector2df(frand(-40.f, 40.f), frand(-50.f, -10.f)) * s,
									   -240.f, 640.f, frand(90.f, 150.f) * s, 0.f, 0.f, 0.f, { 255, 150, 150, 160 }, 3 });
				f.spawned = true;
			}
			break;
		}
		case Fx::RESOLVE_GLOW: {
			const float a = 1.f - smooth(100.f, 450.f, t);
			Quad(tGlow, at, 230.f * s, 280.f * s, 0.f, addc(a, { 255, 230, 245, 255 }), true);
			break;
		}
		}
	}
}

void DrawSparks() {
	for(auto& sp : sparks) {
		if(sp.life < 0.f) continue;
		const float k = sp.life / sp.max;
		const float a = k < 0.12f ? k / 0.12f : 1.f - (k - 0.12f) / 0.88f;
		switch(sp.kind) {
		case 0: Quad(tGlow, sp.p, sp.size, sp.size, 0.f, addc(a, sp.color), true); break;
		case 1: Quad(tSpark, sp.p, sp.size * 2.f, sp.size * 2.f, sp.rot * 0.2f, addc(a * (0.7f + 0.3f * std::sin(sp.life * 0.03f)), sp.color), true); break;
		case 2: Quad(tShard, sp.p, sp.size, sp.size * 0.45f, sp.rot, col(a, sp.color), false); break;
		case 3: Quad(tGlow, sp.p, sp.size * (1.f + k), sp.size * (1.f + k) * 0.7f, 0.f, col(a * 0.35f, sp.color), false); break;
		}
	}
}

// ---------------------------------------------------------------- letters that fly in and assemble
void AssembleWord(const std::wstring& word, float t, float tin, float tout, float y, video::SColor glow) {
	if(!F.big || t < 0.f) return;
	const auto full = TextSize(F.big, word);
	float x = SW() / 2.f - full.Width / 2.f;
	const float s = S();
	for(size_t i = 0; i < word.size(); ++i) {
		const std::wstring ch(1, word[i]);
		const auto d = TextSize(F.big, ch);
		const float start = i * 28.f;
		const float p = outCubic((t - start) / std::max(60.f, tin - start));
		const float out = smooth(tout, tout + 160.f, t);
		const float ox = (hash((int)i * 3 + 1) - 0.5f) * 0.6f * SW(), oy = (hash((int)i * 7 + 2) - 0.5f) * 0.5f * SH();
		const float px = x + ox * (1.f - p) + (hash((int)i + 9) - 0.5f) * 80.f * s * out;
		const float py = y + oy * (1.f - p) - 30.f * s * out;
		const float a = p * (1.f - out);
		if(a > 0.f) {
			Quad(tGlow, { px + d.Width / 2.f, py + d.Height / 2.f }, d.Width * 2.2f, d.Height * 1.6f, 0.f, addc(a * 0.55f, glow), true);
			Text(F.big, ch, px, py, col(a, { 255, 255, 255, 255 }), a, 3);
		}
		x += d.Width;
	}
}

// big chain cards in a zig-zag joined by a chain
void ChainStack(const std::vector<uint32_t>& codes, int upto, float a, float appear, int flashLink, float flash) {
	const int n = std::min<int>(upto, (int)codes.size());
	if(n <= 0 || a <= 0.f) return;
	const int first = std::max(0, n - 4);
	const int shown = n - first;
	const float s = S();
	const auto c = Center();
	std::vector<core::vector2df> at;
	for(int i = 0; i < shown; ++i) {
		const float k = shown == 1 ? 0.f : (float)i / (shown - 1) - 0.5f;
		at.push_back(c + core::vector2df(k * 0.36f * SW(), ((i % 2) ? 0.07f : -0.07f) * SH() + 30.f * s));
	}
	for(int i = 0; i + 1 < shown; ++i) {
		const auto p0 = at[i], p1 = at[i + 1];
		const auto d = p1 - p0;
		const float len = (float)d.getLength(), ang = std::atan2(d.Y, d.X);
		const int links = std::max(3, (int)(len / (24.f * s)));
		for(int l = 0; l < links; ++l)
			Quad(tLink, p0 + d * ((l + 0.5f) / links), 30.f * s, (l % 2 ? 8.f : 16.f) * s, ang, col(a * 0.95f, { 255, 190, 200, 215 }), false);
	}
	for(int i = 0; i < shown; ++i) {
		const int link = first + i + 1;
		auto* art = Art(codes[first + i]);
		if(!art) continue;
		const float pk = outCubic((appear - i * 60.f) / 220.f);
		if(pk <= 0.f) continue;
		const float h = 210.f * s * (0.85f + 0.15f * pk) * (1.f + 0.06f * i), w = h * 0.686f;
		const float rot = (i % 2 ? 0.26f : -0.17f);
		const float aa = a * pk;
		const bool lit = link == flashLink;
		Quad(tGlow, at[i], w * 1.5f, h * 1.3f, rot, addc(aa * (lit ? 0.9f + flash : 0.35f), lit ? video::SColor(255, 255, 255, 255) : video::SColor(255, 90, 200, 255)), true);
		Quad(art, Full(art), at[i], w, h, rot, col(aa, { 255, 255, 255, 255 }), false);
		if(lit && flash > 0.f) Quad(art, Full(art), at[i], w, h, rot, addc(flash * 0.8f, { 255, 255, 255, 255 }), true);
		if(F.num) {
			const std::wstring num = epro::format(L"{}", link);
			const auto d = TextSize(F.num, num);
			const core::vector2df corner = at[i] + core::vector2df((i % 2 ? 0.42f : -0.42f) * w, (i % 2 ? 0.36f : -0.42f) * h);
			Quad(tGlow, corner, d.Width * 3.f, d.Height * 2.2f, 0.f, addc(aa, { 255, 100, 220, 255 }), true);
			Text(F.num, num, corner.X - d.Width / 2.f, corner.Y - d.Height / 2.f, col(aa, { 255, 235, 252, 255 }), aa, 3);
		}
	}
}

// ---------------------------------------------------------------- 1.4 s ace / god summon scene
void DrawCutin() {
	const float t = ov.t, D = ov.dur, s = S();
	const float W = SW(), H = SH();
	const auto c = Center();
	const float out = smooth(D - 150.f, D, t);  // 100-150 ms cross-dissolve
	const float live = 1.f - out;
	// the board stays visible, darkened ~70% and tinted
	FillRect(0, 0, (s32)W, (s32)H, col(0.72f * live, { 255, 6, 8, 16 }));
	Quad(tGlow, c, W * 1.3f, H * 1.6f, 0.f, addc(0.35f * live, ov.tint), true);
	// radial speed lines converging on the centre, 400-1300
	const float lines = window(t, 380.f, D - 100.f, 120.f, 200.f);
	for(int i = 0; i < 28; ++i) {
		const float ang = hash(i) * 2.f * PI;
		const float r0 = (0.42f + 0.25f * hash(i + 40)) * W, len = (0.18f + 0.2f * hash(i + 80)) * W;
		const float drift = std::fmod(t * 0.0006f + hash(i + 120), 1.f);
		const float r = r0 - drift * 0.12f * W;
		const core::vector2df dir(std::cos(ang), std::sin(ang));
		Quad(tStreak, c + dir * (r + len * 0.5f), len, (6.f + 8.f * hash(i + 3)) * s, ang, col(lines * 0.55f, { 255, 0, 0, 0 }), false);
	}
	// bokeh orbs drifting at the left and right edges
	for(int i = 0; i < 10; ++i) {
		const float side = i % 2 ? 1.f : -1.f;
		const float x = c.X + side * (0.36f + 0.1f * hash(i + 7)) * W;
		const float y = H * (1.1f - std::fmod(hash(i + 11) + t * 0.00008f * (1.f + hash(i)), 1.2f));
		const float r = (40.f + 70.f * hash(i + 5)) * s;
		Quad(tGlow, { x, y }, r, r, 0.f, addc(lines * 0.45f, { 255, 80, 170, 255 }), true);
	}
	// the illustration: ~70% of screen height, pushing in 0.85 -> 1.0, slow parallax drift
	const auto illo = Illustration(ov.code);
	auto* art = illo.first;
	if(art) {
		const float push = 0.85f + 0.15f * outCubic(t / (D - 150.f));
		const float h = 0.70f * H * push, w = h;
		const core::vector2df p = c + core::vector2df(t * 0.012f * s, -0.04f * H);
		Quad(tGlow, p, w * 1.5f, h * 1.5f, 0.f, addc(0.6f * live, ov.tint), true);
		Quad(art, illo.second, p, w, h, 0.f, col(live, { 255, 255, 255, 255 }), false);
		// tendrils of energy whipping in from the edges, 100-300 ms
		if(t > 100.f && t < 420.f) {
			const float k = clamp01((t - 100.f) / 200.f);
			for(int i = 0; i < 6; ++i) {
				const float a0 = hash(i + 200) * 2.f * PI;
				core::vector2df prev = p + core::vector2df(std::cos(a0), std::sin(a0)) * (0.75f * W);
				for(int seg = 1; seg <= 10; ++seg) {
					const float u = seg / 10.f;
					if(u > k) break;
					const float rr = (0.75f - 0.55f * u) * W;
					const float aa = a0 + 1.4f * u * (i % 2 ? 1.f : -1.f);
					const core::vector2df q = p + core::vector2df(std::cos(aa), std::sin(aa)) * rr;
					Line(prev, q, 7.f * s, addc((1.f - smooth(300.f, 420.f, t)) * 0.7f, { 255, 220, 210, 120 }));
					prev = q;
				}
			}
		}
		// lightning crawling over the art at ~300-400 and ~1000-1060 ms
		const bool bolt = (t > 300.f && t < 420.f) || (t > 1000.f && t < 1070.f);
		if(bolt) {
			if(ov.acc > 33.f || ov.bolts.empty()) {
				ov.acc = 0.f;
				ov.bolts.clear();
				for(int b = 0; b < 3; ++b) {
					core::vector2df q = p + core::vector2df(frand(-0.5f, 0.5f) * w, frand(-0.5f, 0.5f) * h);
					const float dir = frand(0.f, 2.f * PI);
					for(int seg = 0; seg < 9; ++seg) {
						const float da = dir + frand(-0.9f, 0.9f);
						const core::vector2df n = q + core::vector2df(std::cos(da), std::sin(da)) * (frand(18.f, 42.f) * s);
						ov.bolts.push_back({ q, n });
						q = n;
					}
				}
			}
			for(auto& b : ov.bolts) {
				Line(b.first, b.second, 9.f * s, addc(0.9f, { 255, 150, 230, 255 }));
				Line(b.first, b.second, 3.f * s, addc(1.f, { 255, 255, 255, 255 }));
			}
		}
		// gold sparkle burst around the silhouette at ~300 ms, twinkling after
		if(ov.beat == 0 && t >= 300.f) {
			ov.beat = 1;
			for(int i = 0; i < 40; ++i) {
				const float a = frand(0.f, 2.f * PI);
				const core::vector2df q = p + core::vector2df(std::cos(a) * w * frand(0.35f, 0.62f), std::sin(a) * h * frand(0.35f, 0.62f));
				sparks.push_back({ q, core::vector2df(frand(-15.f, 15.f), frand(-25.f, 5.f)) * s, -frand(0.f, 400.f), frand(700.f, 1100.f),
								   frand(9.f, 18.f) * s, 0.f, 0.f, 0.f, { 255, 255, 214, 110 }, 1 });
			}
		}
	}
	// two-tier name plate across the lower ~15%, sliding in from the left
	const float slide = outQuint(t / 220.f);
	const float plateA = live;
	const float textA = 1.f - smooth(D - 200.f, D - 150.f, t);
	const s32 y0 = (s32)(0.79f * H), y1 = (s32)(0.875f * H), y2 = (s32)(0.94f * H);
	const s32 right = (s32)(W * 0.78f * slide);
	const video::SColor band = col(0.80f * plateA, { 255, 6, 10, 22 }), bandEnd = col(0.f, { 255, 6, 10, 22 });
	drv->draw2DRectangle(core::rect<s32>(0, y0, right, y1), band, bandEnd, band, bandEnd);
	const video::SColor rule = col(plateA, { 255, 220, 230, 255 }), ruleEnd = col(0.f, { 255, 220, 230, 255 });
	drv->draw2DRectangle(core::rect<s32>(0, y0 - (s32)(2 * s), right, y0), rule, ruleEnd, rule, ruleEnd);
	const s32 inner = (s32)(W * 0.52f * slide);
	const video::SColor pl = col(0.95f * plateA, ov.plate), plEnd = col(0.f, ov.plate);
	drv->draw2DRectangle(core::rect<s32>(0, y1, inner, y2), pl, plEnd, pl, plEnd);
	const float textX = -0.6f * W * (1.f - slide) + 34.f * s;
	if(F.plate) {
		const auto d = TextSize(F.plate, ov.name);
		Text(F.plate, ov.name, textX, (y0 + y1) / 2.f - d.Height / 2.f, col(textA, { 255, 255, 255, 255 }), textA * 0.9f, 2);
	}
	if(F.statline && !ov.stats.empty()) {
		const auto d = TextSize(F.statline, ov.stats);
		Text(F.statline, ov.stats, textX + 6.f * s, (y1 + y2) / 2.f - d.Height / 2.f, col(textA, { 255, 236, 240, 255 }), textA * 0.7f, 1);
	}
	// level stars pop in left to right, ~30 ms apart, from ~600 ms (white dot, then gold)
	for(int i = 0; i < ov.stars && i < 13; ++i) {
		const float st = 600.f + i * 30.f;
		if(t < st) break;
		const float k = clamp01((t - st) / 90.f);
		const float sz = (22.f + 10.f * (1.f - k)) * s;
		const core::vector2df p(textX + 14.f * s + i * 27.f * s, y0 - 18.f * s);
		Quad(tGlow, p, sz * 2.2f, sz * 2.2f, 0.f, addc(textA * (1.f - k) * 0.9f, { 255, 255, 255, 255 }), true);
		Quad(tStar, p, sz, sz, 0.f, col(textA, lerpc({ 255, 255, 255, 255 }, { 255, 255, 196, 60 }, k)), false);
	}
}

// ---------------------------------------------------------------- Seal of Orichalcos (anime-only: our own scene)
void DrawSeal() {
	const float t = ov.t, D = ov.dur;
	const float W = SW(), H = SH();
	const auto c = Center();
	const float out = smooth(D - 350.f, D, t);
	const float dim = smooth(0.f, 250.f, t) * (1.f - out);
	FillRect(0, 0, (s32)W, (s32)H, col(0.6f * dim, { 255, 0, 10, 4 }));
	const float grow = outQuint(t / 800.f);
	const float sz = H * (0.25f + 0.8f * grow) * (1.f + 0.25f * out);
	Quad(tGlow, c, sz * 1.5f, sz * 1.5f, 0.f, addc(0.55f * dim, ov.tint), true);
	const float rot = -0.9f + 0.9f * grow + t * 0.00008f;
	Quad(tSeal, c, sz, sz, rot, addc(1.f - out, ov.tint), true);
	Quad(tSeal, c, sz * 1.02f, sz * 1.02f, rot, addc(0.5f * (1.f - out), { 255, 255, 255, 255 }), true);
	if(ov.beat == 0 && t > 780.f) { ov.beat = 1; Burst(c, 60, ov.tint, 650.f, 14.f, 900.f); }
	AssembleWord(ov.name, t - 500.f, 400.f, D - 700.f, H * 0.80f, ov.tint);
}

// ---------------------------------------------------------------- Exodia win (~10.5 s)
void DrawExodia() {
	const float t = ov.t, s = S();
	const float W = SW(), H = SH();
	const auto c = Center();
	const float dim = smooth(0.f, 400.f, t) * (1.f - smooth(9400.f, 9600.f, t));
	FillRect(0, 0, (s32)W, (s32)H, col(0.55f * dim, { 255, 4, 2, 8 }));
	// 600-2900: head at the top centre, the limbs fly in from the edges into a star around it
	static const uint32_t piece[5] = { 33396948, 70903634, 7902349, 8124921, 44519536 }; // head, R arm, L arm, R leg, L leg
	static const float px[5] = { 0.f, -0.25f, 0.25f, -0.16f, 0.16f }, py[5] = { -0.17f, -0.04f, -0.04f, 0.2f, 0.2f };
	static const float tilt[5] = { 0.f, -0.24f, 0.24f, -0.2f, 0.2f };
	static const float fromx[5] = { 0.f, -0.8f, 0.8f, -0.3f, 0.3f }, fromy[5] = { 0.f, -0.1f, -0.1f, 0.8f, 0.8f };
	const float scatter = smooth(2750.f, 3050.f, t);
	const float aura = window(t, 1500.f, 3050.f, 600.f, 250.f);
	if(aura > 0.f) {
		const float pulse = 0.85f + 0.15f * std::sin(t * 0.008f);
		const core::vector2df ac = c + core::vector2df(0.f, -0.02f * H);
		Quad(tGlow, ac, 0.95f * H * pulse, 0.95f * H * pulse, 0.f, addc(aura * 0.9f, { 255, 160, 70, 255 }), true);
		Quad(tGlow, ac, 0.55f * H, 0.55f * H, 0.f, addc(aura * 0.6f, { 255, 220, 140, 255 }), true);
	}
	for(int i = 0; i < 5; ++i) {
		const float t0 = i == 0 ? 600.f : 750.f + (i - 1) * 40.f;
		if(t < t0 || scatter >= 1.f) continue;
		auto* art = Art(piece[i]);
		if(!art) continue;
		const float k = i == 0 ? smooth(600.f, 750.f, t) : outCubic((t - t0) / 600.f);
		const core::vector2df home = c + core::vector2df(px[i] * H * 1.15f, py[i] * H);
		core::vector2df p = i == 0 ? home : home + core::vector2df(fromx[i] * W, fromy[i] * H) * (1.f - k);
		p += core::vector2df((hash(i) - 0.5f) * 0.3f * W, -0.6f * H) * (scatter * scatter);
		const float a = (i == 0 ? k : 0.6f + 0.4f * k) * (1.f - scatter);
		const float h = 0.27f * H, w = h * 0.686f;
		Quad(art, Full(art), p, w, h, tilt[i] * k + scatter * (hash(i + 3) - 0.5f), col(a, { 255, 255, 255, 255 }), false);
	}
	// 2750-3700 fire rises from the bottom and fills the screen; it keeps burning behind the figure until ~9400
	const float fire = smooth(2750.f, 3050.f, t) * (1.f - smooth(9300.f, 9500.f, t));
	if(fire > 0.f) {
		const float level = smooth(2750.f, 3200.f, t);
		const float full = window(t, 3000.f, 3900.f, 150.f, 400.f);
		const s32 top = (s32)(H * (1.f - level));
		const video::SColor f0 = col(0.f, { 255, 255, 120, 20 }), f1 = col(fire * 0.8f, { 255, 255, 110, 20 });
		drv->draw2DRectangle(core::rect<s32>(0, top, (s32)W, (s32)H), f0, f0, f1, f1);
		FillRect(0, 0, (s32)W, (s32)H, col(full * 0.85f, { 255, 255, 150, 40 }));
		while(ov.acc > 12.f) {
			ov.acc -= 12.f;
			for(int k = 0; k < 4; ++k) {
				const bool ash = frand(0.f, 1.f) < 0.12f;
				sparks.push_back({ core::vector2df(frand(0.f, W), H + 30.f * s), core::vector2df(frand(-30.f, 30.f), -frand(260.f, 620.f)) * s, 0.f, frand(900.f, 1700.f),
								   (ash ? frand(5.f, 9.f) : frand(60.f, 150.f)) * s, -60.f * s, 0.f, 0.f,
								   ash ? video::SColor(255, 20, 10, 10) : video::SColor(255, 255, (u32)frand(90.f, 180.f), 30), (uint8_t)(ash ? 2 : 0) });
			}
		}
	} else {
		ov.acc = 0.f;
	}
	// 3700-5750 a dark figure emerges in the fire; 5750 reveal slash; hold and push in
	const auto headIllo = Illustration(33396948);
	auto* head = headIllo.first;
	if(head && t > 3700.f && t < 9600.f) {
		const float appear = smooth(3700.f, 5000.f, t);
		const float reveal = smooth(5750.f, 5900.f, t);
		const float push = 1.f + 0.05f * clamp01((t - 5750.f) / 3500.f);
		const float h = 0.82f * H * push, w = h;
		const core::vector2df p = c + core::vector2df(0.f, -0.02f * H);
		const video::SColor shade = lerpc({ 255, 20, 6, 4 }, { 255, 255, 255, 255 }, reveal);
		Quad(head, headIllo.second, p, w, h, 0.f, col(appear, shade), false);
		if(t > 5750.f && t < 6050.f) {
			const float k = (t - 5750.f) / 300.f;
			Quad(tStreak, p + core::vector2df((k - 0.5f) * 0.9f * W, (k - 0.5f) * 0.5f * H), W * 0.9f, 60.f * s, 0.5f, addc(1.f - k, { 255, 255, 220, 140 }), true);
			FillRect(0, 0, (s32)W, (s32)H, col((1.f - k) * 0.6f, { 255, 255, 240, 200 }));
		}
		// 6600-9200 an energy orb gathers between the hands, wind arcs orbiting
		const float charge = clamp01((t - 6600.f) / 2600.f);
		if(t > 6600.f) {
			const core::vector2df orb = p + core::vector2df(0.f, 0.12f * h);
			const float r = (30.f + 220.f * charge * charge) * s;
			Quad(tGlow, orb, r * 2.4f, r * 2.4f, 0.f, addc(0.9f, { 255, 255, 230, 140 }), true);
			Quad(tGlow, orb, r, r, 0.f, addc(1.f, { 255, 255, 255, 255 }), true);
			for(int i = 0; i < 4; ++i)
				Quad(tRing, p, h * (0.7f + 0.12f * i), h * (0.32f + 0.05f * i), t * 0.004f * (i % 2 ? 1.f : -1.3f) + i, addc(0.45f * charge, { 255, 240, 245, 255 }), true);
		}
	}
	// 9200-9400 flare + gold shockwave rings, 9400-10000 white-out, then back to the field
	if(t > 9200.f && t < 9900.f) {
		for(int i = 0; i < 3; ++i) {
			const float k = clamp01((t - 9200.f - i * 80.f) / 500.f);
			const float r = W * 1.6f * outCubic(k);
			Quad(tRing, c, r, r, 0.f, addc(1.f - k, { 255, 255, 214, 100 }), true);
		}
	}
	const float white = smooth(9300.f, 9600.f, t) * (1.f - smooth(9950.f, 10400.f, t));
	if(white > 0.f) FillRect(0, 0, (s32)W, (s32)H, col(white, { 255, 255, 255, 255 }));
}

// ---------------------------------------------------------------- phase / turn bands (~0.9 s)
void DrawBanner() {
	if(!F.big) return;
	const float t = ov.t, D = ov.dur, s = S();
	const float W = SW(), H = SH();
	const float bandA = window(t, 0.f, D, 90.f, 150.f);
	const s32 h = (s32)(0.11f * H * (ov.turn ? 1.15f : 1.f));
	const s32 y = (s32)(0.5f * H) - h / 2 - (s32)(ov.turn ? smooth(D - 120.f, D, t) * 20.f * s : 0.f);
	const video::SColor a = ov.mine ? video::SColor(255, 40, 70, 200) : video::SColor(255, 190, 20, 40);
	const video::SColor b = ov.mine ? video::SColor(255, 110, 40, 190) : video::SColor(255, 120, 10, 30);
	const float sat = ov.turn ? 0.85f : 0.62f;
	drv->draw2DRectangle(core::rect<s32>(0, y, (s32)W, y + h), col(bandA * sat, a), col(bandA * sat, b), col(bandA * sat, a), col(bandA * sat, b));
	FillRect(0, y, (s32)W, y + (s32)(2 * s), col(bandA * 0.8f, { 255, 255, 255, 255 }));
	FillRect(0, y + h - (s32)(2 * s), (s32)W, y + h, col(bandA * 0.5f, { 255, 255, 255, 255 }));
	if(ov.turn && t < 160.f) {
		const float k = t / 160.f;
		Quad(tStreak, { W * (k - 0.2f), y + h / 2.f }, W * 0.6f, h * 0.9f, 0.f, addc(1.f, { 255, 255, 255, 255 }), true);
	}
	if(ov.turn) Quad(tStreak, { W * 0.86f, (float)y + h * 0.35f }, W * 0.3f, h * 0.5f, -0.05f, addc(bandA * 0.6f, { 255, 255, 255, 255 }), true);
	// text revealed by a left-to-right wipe (0-200), wiped out right part first (800-950)
	const auto d = TextSize(F.big, ov.text);
	const float tx = W / 2.f - d.Width / 2.f, ty = y + h / 2.f - d.Height / 2.f;
	const float wipeIn = clamp01((t - (ov.turn ? 100.f : 0.f)) / 200.f);
	const float wipeOut = clamp01((t - (D - 150.f)) / 120.f);
	const s32 cx0 = (s32)(tx - 10.f * s), cx1 = (s32)(tx + (d.Width + 20.f * s) * wipeIn * (1.f - wipeOut));
	const core::rect<s32> clip(cx0, y, std::max(cx0, cx1), y + h);
	Text(F.big, ov.text, tx + 4.f * s, ty + 3.f * s, col(bandA * 0.6f, { 255, 0, 0, 0 }), 0.f, 0, &clip);
	Text(F.big, ov.text, tx, ty, col(bandA, { 255, 255, 255, 255 }), bandA * 0.7f, 2, &clip);
}

} // namespace

void Draw(scene::ISceneManager* smgr) {
	if(!drv || !smgr) return;
	auto* coll = smgr->getSceneCollisionManager();
	const float W = SW(), H = SH();
	// activation dims the field ~35% with a cool tint (under the card glow)
	if(dimT >= 0.f) FillRect(0, 0, (s32)W, (s32)H, col(0.33f * window(dimT, 0.f, 950.f, 100.f, 150.f), { 255, 20, 30, 52 }));
	if(ov.kind == Overlay::CHAIN || ov.kind == Overlay::RESOLVE)
		FillRect(0, 0, (s32)W, (s32)H, col(0.42f * window(ov.t, 0.f, ov.dur, 100.f, 200.f), { 255, 10, 22, 48 }));
	DrawFx(coll);
	switch(ov.kind) {
	case Overlay::CUTIN: DrawCutin(); break;
	case Overlay::SEAL: DrawSeal(); break;
	case Overlay::EXODIA: DrawExodia(); break;
	case Overlay::CHAIN: {
		const float t = ov.t;
		ChainStack(ov.codes, (int)ov.codes.size(), 1.f - smooth(950.f, 1100.f, t), t - 100.f, -1, 0.f);
		AssembleWord(ov.text, t, 300.f, 850.f, H * 0.12f, { 255, 90, 200, 255 });
		break;
	}
	case Overlay::RESOLVE: {
		const float t = ov.t;
		if(ov.first) {
			AssembleWord(ov.text, t, 400.f, 650.f, H * 0.12f, { 255, 120, 220, 255 });
			const float flash = t > 850.f ? std::max(0.f, 1.f - std::fabs(t - 900.f) / 160.f) : 0.f;
			ChainStack(ov.codes, ov.resolving, window(t, 700.f, ov.dur, 100.f, 100.f), (t - 700.f) * 3.f, ov.resolving, flash);
		} else {
			const float flash = std::max(0.f, 1.f - std::fabs(t - 220.f) / 160.f);
			ChainStack(ov.codes, ov.resolving, window(t, 0.f, ov.dur, 100.f, 120.f), t * 3.f, ov.resolving, flash);
		}
		break;
	}
	case Overlay::BANNER: DrawBanner(); break;
	default: break;
	}
	DrawSparks();
	if(dmgFrame > 0.f) {
		// thick crimson frame round the screen when you take damage
		const float a = dmgFrame > 300.f ? 1.f : dmgFrame / 300.f;
		const s32 b = (s32)(0.05f * W), w = (s32)W, h = (s32)H;
		const auto edge = col(0.85f * a, { 255, 200, 10, 30 }), in = col(0.f, { 255, 200, 10, 30 });
		drv->draw2DRectangle(core::rect<s32>(0, 0, b, h), edge, in, edge, in);
		drv->draw2DRectangle(core::rect<s32>(w - b, 0, w, h), in, edge, in, edge);
		drv->draw2DRectangle(core::rect<s32>(0, 0, w, b), edge, edge, in, in);
		drv->draw2DRectangle(core::rect<s32>(0, h - b, w, h), in, in, edge, edge);
		const float pink = clamp01((dmgFrame - 700.f) / 500.f);
		if(pink > 0.f) Quad(tGlow, { W / 2.f, H * 1.05f }, W * 1.4f, H * 0.9f, 0.f, addc(pink * 0.6f, { 255, 255, 150, 180 }), true);
	}
}

}
