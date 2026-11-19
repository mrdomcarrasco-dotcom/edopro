// Progression "duel polish" layer. See mdfx.h. Beat timings follow yugioh-progression docs/MD-STUDY.md.
#include "mdfx.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
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
video::ITexture* tVig = nullptr;     // screen vignette: clear centre, dark edges (alpha)
video::ITexture* tArrow = nullptr;   // Link arrow chevron, pointing right
video::ITexture* tFeather = nullptr; // square frame: clear inside, opaque toward the edges (feathers a square crop)
bool enabled = true;
bool mdLayout = true;                // mdfx.conf "layout md|classic"
bool layoutActive = false;
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
	tVig = MakeTex("mdfx_vig", 256, 256, [](float u, float v) {
		return smooth(0.62f, 1.42f, std::sqrt(u * u * 0.85f + v * v));
	});
	tFeather = MakeTex("mdfx_feather", 128, 128, [](float u, float v) {
		const float e = std::max(std::fabs(u), std::fabs(v));
		return std::max(smooth(0.48f, 0.98f, e), smooth(0.72f, 1.18f, std::sqrt(u * u + v * v)));
	});
	tArrow = MakeTex("mdfx_arrow", 64, 64, [](float u, float v) {
		// a filled chevron: the triangle u in [-0.6, 0.75], |v| < (0.75 - u) * 0.8, with a soft edge
		const float edge = (0.75f - u) * 0.8f - std::fabs(v);
		return u < -0.6f ? 0.f : clamp01(edge * 24.f);
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
// an unrotated quad cut to the horizontal band [top, bot] (the cut-in shutter), texture coords cropped to match
void BandQuad(video::ITexture* tex, const core::rect<s32>& src, core::vector2df c, float w, float h, float top, float bot,
			  video::SColor color, bool additive) {
	if(!tex || h <= 0.f) return;
	const float y0 = c.Y - h / 2.f, y1 = c.Y + h / 2.f;
	const float a = std::max(y0, top), b = std::min(y1, bot);
	if(b <= a) return;
	const float sh = (float)src.getHeight();
	const s32 sy0 = src.UpperLeftCorner.Y + (s32)std::floor((a - y0) / h * sh);
	const s32 sy1 = src.UpperLeftCorner.Y + (s32)std::ceil((b - y0) / h * sh);
	Quad(tex, core::rect<s32>(src.UpperLeftCorner.X, sy0, src.LowerRightCorner.X, std::max(sy0 + 1, sy1)), { c.X, (a + b) / 2.f }, w, b - a, 0.f, color, additive);
}
void BandQuad(video::ITexture* tex, core::vector2df c, float w, float h, float top, float bot, video::SColor color, bool additive) {
	if(!tex) return;
	const auto t = tex->getOriginalSize();
	BandQuad(tex, core::rect<s32>(0, 0, t.Width, t.Height), c, w, h, top, bot, color, additive);
}
// rgb multiplied by k (alpha 255): a darker / dimmer copy of a picture through the vertex colour
video::SColor gain(float k) {
	const u32 v = (u32)(clamp01(k) * 255.f);
	return video::SColor(255, v, v, v);
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
// a private copy of a card picture under an "mdfx:" name. drv->getTexture(file) would hand back the SAME texture the
// image manager loads from that path, and the image manager removes it on a cache reset, leaving our cached pointer
// dead (chain stack SIGSEGV in STextureCache::set, 2026-11-14). The driver never shares a texture with this name.
video::ITexture* OwnTexture(const epro::path_string& file) {
	const epro::path_string name = EPRO_TEXT("mdfx:") + file;
	if(auto* t = drv->findTexture({ name.data(), static_cast<u32>(name.size()) })) return t;
	auto* img = drv->createImageFromFile({ file.data(), static_cast<u32>(file.size()) });
	if(!img) return nullptr;
	auto* t = drv->addTexture({ name.data(), static_cast<u32>(name.size()) }, img);
	img->drop();
	return t;
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
				tex = OwnTexture(file);
				if(tex) break;
			}
		}
		if(tex) break;
	}
	if(!tex) {
		const auto* data = gDataManager->GetCardData(code);
		if(data && data->alias && data->alias != code) {
			tex = Art(data->alias);
			const auto own = artCache.find(data->alias);
			if(own == artCache.end() || own->second != tex)
				return tex;   // the alias's picture is the image manager's: don't keep it either
		}
	}
	// EDOPRO's own card picture: owned by the image manager, which frees it whenever it resets its cache, so it is
	// never kept here (a kept pointer crashed the chain stack once the picture was gone). Asked for again every frame.
	if(!tex)
		return mainGame->imageManager.GetTextureCard(code, ygo::imgType::ART);
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
	// only cache textures mdfx loaded itself (see Art): an image-manager picture can be freed at any time
	const auto own = artCache.find(code);
	if(r.first && HiRes(code, false) == r.first || (own != artCache.end() && own->second == r.first))
		illoCache[code] = r;
	return r;
}

// authored title art (tools/mdfx_skin.py -> <EDOPRO>/pics/mdfx/ui/<key>.png): banner words, CHAIN/RESOLVE,
// chain numerals n1..n6, the chain link and the banner band. Missing files fall back to text.
std::map<std::string, video::ITexture*> uiCache;
video::ITexture* Ui(const std::string& key) {
	auto it = uiCache.find(key);
	if(it != uiCache.end()) return it->second;
	auto* t = LoadFile(epro::format(EPRO_TEXT("./pics/mdfx/ui/{}.png"), Utils::ToPathString(key)));
	uiCache[key] = t;
	return t;
}
std::string KeyOf(const std::wstring& text) {
	std::string k;
	for(wchar_t ch : text) {
		if(ch == L' ') k += '_';
		else if((ch >= L'a' && ch <= L'z') || (ch >= L'0' && ch <= L'9')) k += (char)ch;
		else if(ch >= L'A' && ch <= L'Z') k += (char)(ch - L'A' + L'a');
	}
	return k;
}
// a title image centred at (cx, cy), height h, alpha a; glint = 0..1 position of a light sweep (<0 none)
void Title(video::ITexture* tex, float cx, float cy, float h, float a, float scale, float glint) {
	const auto ts = tex->getOriginalSize();
	const float w = h * ts.Width / (float)ts.Height;
	Quad(tex, { cx, cy }, w * scale, h * scale, 0.f, col(a, { 255, 255, 255, 255 }), false);
	if(glint >= 0.f && glint <= 1.f) {
		const float gx = cx - w * 0.6f + glint * w * 1.2f;
		Quad(tStreak, { gx, cy }, h * 1.4f, h * 1.2f, 1.2f, addc(a * 0.55f * std::sin(glint * PI), { 255, 255, 250, 225 }), true);
	}
}

bool PlayFx(const char* name) {
	return gSoundManager && gSoundManager->PlayFx(name);
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
float clockMs = 0.f;  // free-running, for HUD pulses
struct PendingNumber { std::wstring text; int player = 0, frames = 0; bool valid = false; } pendingNum; // LpNumber -> Draw
bool DrawLpNumber(const std::wstring& text, int player, int framesLeft);

struct Overlay {
	enum Kind : uint8_t { NONE, CUTIN, EXODIA, SEAL, CHAIN, RESOLVE, BANNER, RESULT, METHOD } kind = NONE;
	float t = 0.f, dur = 0.f;
	uint32_t code = 0;
	Cutin cut = Cutin::NONE;
	bool opp = false, first = false, mine = false, turn = false;
	int resolving = 0, beat = 0;
	int video = 0;          // frames of an authored scene (pics/mdfx/scene/<code>/0001.jpg...), 0 = procedural
	uint32_t videoCode = 0;
	float plateStart = 0.f; // when the name plate slides in
	std::vector<uint32_t> codes;
	std::wstring name, stats, text;
	int stars = 0;
	video::SColor tint, plate;
	float acc = 0.f;
	std::vector<std::pair<core::vector2df, core::vector2df>> bolts;
	// the summoned card's zone: the cut-in shutter closes onto its row, the method card flies into it
	bool hasZone = false;
	core::vector3df zone;
	core::vector2df zoneScreen;
	int method = 0;         // Method: summon-method presentation / cut-in label
	uint32_t markers = 0;   // Link arrows
} ov;

// how a monster arrived: a summon-method presentation plays for these (Master Duel animates every one)
enum Method : int { M_NONE, M_FUSION, M_SYNCHRO, M_XYZ, M_LINK, M_RITUAL, M_PENDULUM };
const wchar_t* MethodName(int m) {
	switch(m) {
	case M_FUSION: return L"FUSION SUMMON";
	case M_SYNCHRO: return L"SYNCHRO SUMMON";
	case M_XYZ: return L"XYZ SUMMON";
	case M_LINK: return L"LINK SUMMON";
	case M_RITUAL: return L"RITUAL SUMMON";
	case M_PENDULUM: return L"PENDULUM SUMMON";
	default: return L"";
	}
}
// palette per method: a = main light, b = accent
void MethodColors(int m, video::SColor& a, video::SColor& b) {
	switch(m) {
	case M_FUSION: a = { 255, 168, 96, 255 }; b = { 255, 255, 150, 70 }; break;
	case M_SYNCHRO: a = { 255, 205, 240, 255 }; b = { 255, 120, 255, 175 }; break;
	case M_XYZ: a = { 255, 255, 205, 110 }; b = { 255, 120, 110, 220 }; break;
	case M_LINK: a = { 255, 80, 175, 255 }; b = { 255, 255, 100, 60 }; break;
	case M_RITUAL: a = { 255, 140, 205, 255 }; b = { 255, 230, 245, 255 }; break;
	case M_PENDULUM: a = { 255, 70, 230, 205 }; b = { 255, 235, 90, 205 }; break;
	default: a = { 255, 255, 255, 255 }; b = a; break;
	}
}
// materials and origins noted from MSG_MOVE, consumed by the next summon
struct PendingSummon {
	std::vector<uint32_t> mats;
	uint32_t reason = 0;
	float time = -1e9f;
	std::map<uint32_t, uint32_t> from; // card code -> the location it came from into a Monster Zone
	float lastPendulum = -1e9f;
} pend;

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
		if(a == "layout") { mdLayout = (b != "classic" && b != "off" && b != "0"); continue; }
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
bool MdLayout() { return Enabled() && mdLayout; }
bool LayoutActive() { return layoutActive; }
void SetLayoutActive(bool on) { layoutActive = on; }
// EDOPRO's frustum is -0.90..0.45 x -0.42..0.42 (the board pushed right of a 300 px info column, empty sky above the
// opponent's hand). The board spans about -0.48..0.45 x -0.41..0.27 in view space (camera at 4.2/8/7.8): centre on
// it and fill the screen like Master Duel, ~1.2x larger, with a margin for the side controls.
void LayoutFrustum(float& left, float& right, float& bottom, float& top) {
	bottom = -0.415f; top = 0.285f;
	const float cx = -0.02f, half = (top - bottom) * 1.6f / 2.f;
	left = cx - half; right = cx + half;
}

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

namespace {
// authored summon scenes: a 24 fps JPEG sequence per card, rendered from our own keyframe + image-to-video
constexpr float SCENE_FPS = 24.f;
constexpr float SCENE_SPEED = 1.15f; // the takes play a touch faster: livelier and shorter
std::map<uint32_t, int> sceneCount;
epro::path_string ScenePath(uint32_t code, int frame) {
	return epro::format(EPRO_TEXT("./pics/mdfx/scene/{}/{:04d}.jpg"), code, frame);
}
int SceneFrames(uint32_t code) {
	auto it = sceneCount.find(code);
	if(it != sceneCount.end()) return it->second;
	int n = 0;
	while(n < 600 && Utils::FileExists(ScenePath(code, n + 1))) ++n;
	sceneCount[code] = n;
	return n;
}
video::ITexture* sceneTex = nullptr;
int sceneTexFrame = -1;
uint32_t sceneTexCode = 0;
void DropSceneTex() {
	if(sceneTex) drv->removeTexture(sceneTex);
	sceneTex = nullptr;
	sceneTexFrame = -1;
	sceneTexCode = 0;
}
// The image-to-video takes run hot: Ra / Blue-Eyes / Exodia average 105-150 luma with gold highlights clipped at 255,
// shown full screen straight after a dark board (Dominic: "Obelisk hurt my eyes"). Every frame of a scene goes through
// the same curve, so nothing pumps: an exposure that brings the scene's brightest sampled frame to ~78 mean, then a
// soft shoulder that rolls highlights off toward ~208 instead of clipping to white.
std::map<uint32_t, std::vector<u8>> sceneLut;
float MeanLuma(video::IImage* img) {
	const auto d = img->getDimension();
	double sum = 0.0;
	int n = 0;
	for(u32 y = 4; y < d.Height; y += 9)
		for(u32 x = 4; x < d.Width; x += 9) {
			const auto c = img->getPixel(x, y);
			sum += 0.2126 * c.getRed() + 0.7152 * c.getGreen() + 0.0722 * c.getBlue();
			++n;
		}
	return n ? (float)(sum / n) : 0.f;
}
const std::vector<u8>& SceneLut(uint32_t code, int frames) {
	auto it = sceneLut.find(code);
	if(it != sceneLut.end()) return it->second;
	float mean = 0.f;
	for(int f : { 1, std::max(1, frames / 2), std::max(1, frames - 4) }) {
		const auto file = ScenePath(code, f);
		if(auto* img = drv->createImageFromFile({ file.data(), static_cast<u32>(file.size()) })) {
			mean = std::max(mean, MeanLuma(img));
			img->drop();
		}
	}
	const float exposure = mean > 1.f ? std::min(1.f, std::max(0.56f, 78.f / mean)) : 0.9f;
	std::vector<u8> lut(256);
	constexpr float knee = 0.56f, white = 0.82f;
	for(int i = 0; i < 256; ++i) {
		float y = i / 255.f * exposure;
		if(y > knee) y = knee + (white - knee) * (1.f - std::exp(-(y - knee) / (white - knee)));
		lut[i] = (u8)std::lround(clamp01(y) * 255.f);
	}
	return sceneLut[code] = std::move(lut);
}
void ToneMap(video::IImage* img, const std::vector<u8>& lut) {
	const auto fmt = img->getColorFormat();
	if(fmt != video::ECF_R8G8B8 && fmt != video::ECF_A8R8G8B8) return;
	const u32 bpp = img->getBytesPerPixel(), pitch = img->getPitch();
	const auto d = img->getDimension();
	auto* data = static_cast<u8*>(img->lock());
	if(!data) return;
	for(u32 y = 0; y < d.Height; ++y) {
		u8* row = data + y * pitch;
		for(u32 x = 0; x < d.Width; ++x) {
			u8* p = row + x * bpp;
			p[0] = lut[p[0]]; p[1] = lut[p[1]]; p[2] = lut[p[2]]; // ARGB in memory is B,G,R,A: the alpha byte is left alone
		}
	}
	img->unlock();
}
// make frame n of a scene the current scene texture (tone-mapped); streams one texture at a time
video::ITexture* SceneTexture(uint32_t code, int frame) {
	if(sceneTex && sceneTexCode == code && sceneTexFrame == frame) return sceneTex;
	const auto file = ScenePath(code, frame);
	auto* img = Utils::FileExists(file) ? drv->createImageFromFile({ file.data(), static_cast<u32>(file.size()) }) : nullptr;
	if(!img) return sceneTex;
	ToneMap(img, SceneLut(code, SceneFrames(code)));
	const auto name = epro::format(EPRO_TEXT("mdfx:scene:{}:{}"), code, frame);
	auto* next = drv->addTexture({ name.data(), static_cast<u32>(name.size()) }, img);
	img->drop();
	if(next) {
		if(sceneTex && sceneTex != next) drv->removeTexture(sceneTex);
		sceneTex = next;
		sceneTexFrame = frame;
		sceneTexCode = code;
	}
	return sceneTex;
}
// draw frame n of an authored scene full screen (cover), alpha a
void SceneFrame(uint32_t code, int frame, float a) {
	SceneTexture(code, frame);
	if(!sceneTex || a <= 0.f) return;
	const auto ts = sceneTex->getOriginalSize();
	const float W = SW(), H = SH();
	const float k = std::max(W / ts.Width, H / ts.Height);
	Quad(sceneTex, Center(), ts.Width * k, ts.Height * k, 0.f, col(a, { 255, 255, 255, 255 }), false);
}

// the summon method of a monster now arriving in a Monster Zone (MSG_SPSUMMONING), from what MSG_MOVE noted
int DetectMethod(uint32_t code, bool consume) {
	const auto* d = gDataManager->GetCardData(code);
	if(!d) return M_NONE;
	uint32_t from = 0;
	if(auto it = pend.from.find(code); it != pend.from.end()) from = it->second;
	const bool fresh = clockMs - pend.time < 4000.f && clockMs >= pend.time;
	int m = M_NONE;
	if(from & LOCATION_EXTRA) {
		if(d->type & TYPE_LINK) m = M_LINK;
		else if(d->type & TYPE_XYZ) m = M_XYZ;
		else if(d->type & TYPE_SYNCHRO) m = M_SYNCHRO;
		else if(d->type & TYPE_FUSION) m = M_FUSION;
		else if(d->type & TYPE_PENDULUM) m = M_PENDULUM; // a face-up Pendulum from the Extra Deck
	}
	if(m == M_NONE && (d->type & TYPE_RITUAL) && fresh && (pend.reason & REASON_RITUAL)) m = M_RITUAL;
	if(m == M_NONE && (d->type & TYPE_PENDULUM) && (from & LOCATION_HAND) && !(fresh && !pend.mats.empty())) m = M_PENDULUM;
	if(consume) pend.from.erase(code);
	return m;
}
} // namespace

void NoteMove(uint32_t code, uint32_t fromLoc, uint32_t toLoc, uint32_t reason) {
	if(!Enabled()) return;
	constexpr uint32_t methods = REASON_FUSION | REASON_SYNCHRO | REASON_RITUAL | REASON_XYZ | REASON_LINK;
	const bool material = ((reason & REASON_MATERIAL) && (reason & methods)) || ((toLoc & LOCATION_OVERLAY) && !(fromLoc & LOCATION_OVERLAY));
	if(material) {
		if(clockMs - pend.time > 4000.f || clockMs < pend.time) { pend.mats.clear(); pend.reason = 0; }
		if(code && pend.mats.size() < 6) pend.mats.push_back(code);
		pend.reason |= reason;
		pend.time = clockMs;
	}
	if((toLoc & LOCATION_MZONE) && !(toLoc & LOCATION_OVERLAY) && !(fromLoc & LOCATION_MZONE)) {
		if(pend.from.size() > 24) pend.from.clear();
		pend.from[code] = fromLoc;
	}
}

int StartCutin(Cutin kind, uint32_t code, bool opponent, const core::vector3df* zone, bool special) {
	if(!Enabled() || kind == Cutin::NONE) return 0;
	const int method = special ? DetectMethod(code, true) : M_NONE;
	ov = Overlay{};
	ov.code = code;
	ov.cut = kind;
	ov.opp = opponent;
	ov.method = method;
	if(zone) { ov.hasZone = true; ov.zone = *zone; }
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
	// procedural scene: 0-420 the shutter opens, MD's 1.4 s of beats, then it closes onto the zone
	ov.dur = 1850.f;
	ov.plateStart = 380.f;
	{
		uint32_t sc = code;
		int frames = SceneFrames(sc);
		if(!frames) { sc = BaseCode(code); if(sc != code) frames = SceneFrames(sc); }
		if(frames > 0) {
			// authored scene: about 2.7 s of the take (was the whole 5 s clip), the plate early instead of at the end
			ov.video = frames;
			ov.videoCode = sc;
			ov.dur = std::min(2700.f, frames * 1000.f / (SCENE_FPS * SCENE_SPEED));
			ov.plateStart = 560.f;
		}
	}
	if(const auto* d = gDataManager->GetCardData(code)) {
		ov.stars = (d->type & TYPE_LINK) ? 0 : (int)(d->level & 0xff);
		ov.stats = (d->type & TYPE_LINK) ? epro::format(L"ATK {}    LINK-{}", StatText(d->attack), d->level & 0xff)
										  : epro::format(L"ATK {}    DEF {}", StatText(d->attack), StatText(d->defense));
	}
	switch(kind) {
	case Cutin::OBELISK: ov.tint = { 255, 110, 170, 255 }; ov.plate = { 255, 20, 40, 110 }; break;
	case Cutin::SLIFER: ov.tint = { 255, 255, 110, 90 }; ov.plate = { 255, 120, 14, 24 }; break;
	case Cutin::RA: ov.tint = { 255, 255, 214, 100 }; ov.plate = { 255, 20, 40, 110 }; break;
	default: ov.tint = AttributeTint(code); ov.plate = { 255, 20, 40, 110 }; break;
	}
	{
		const bool god = kind == Cutin::OBELISK || kind == Cutin::SLIFER || kind == Cutin::RA;
		if(!(ov.video > 0 && PlayFx(god ? "god" : "ace")))
			PlayFx("cutin");
	}
	return Frames(ov.dur);
}

int SummonMethod(uint32_t code, const core::vector3df& zone, bool opponent) {
	if(!Enabled()) return 0;
	const int m = DetectMethod(code, true);
	if(m == M_NONE) return 0;
	// a Pendulum Summon brings several monsters at once: the presentation plays for the first one only
	if(m == M_PENDULUM) {
		const bool batch = clockMs - pend.lastPendulum < 2500.f && clockMs >= pend.lastPendulum;
		pend.lastPendulum = clockMs;
		if(batch) return 0;
	}
	ov = Overlay{};
	ov.kind = Overlay::METHOD;
	ov.method = m;
	ov.code = code;
	ov.opp = opponent;
	ov.hasZone = true;
	ov.zone = zone;
	ov.dur = 1550.f;
	if(clockMs - pend.time < 4000.f && clockMs >= pend.time) ov.codes = pend.mats;
	pend.mats.clear();
	pend.reason = 0;
	if(const auto* d = gDataManager->GetCardData(code)) {
		ov.stars = (int)(d->level & 0xff);
		ov.markers = d->link_marker;
	}
	{ const auto nm = gDataManager->GetName(code); ov.name.assign(nm.data(), nm.size()); }
	static const char* sfx[] = { "", "summon_fusion", "summon_synchro", "summon_xyz", "summon_link", "summon_ritual", "summon_pendulum" };
	PlayFx(sfx[m]); // duelclient already played the special-summon sound
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

int Result(int outcome, const std::wstring& reason) {
	if(!Enabled() || outcome < 1 || outcome > 2) return 0;
	// duel thread: check the file only, the texture loads on the main thread when drawn
	if(!Utils::FileExists(epro::format(EPRO_TEXT("./pics/mdfx/ui/{}.png"), outcome == 1 ? EPRO_TEXT("victory") : EPRO_TEXT("defeat")))) return 0;
	ov = Overlay{};
	ov.kind = Overlay::RESULT;
	ov.dur = 2900.f;
	ov.mine = outcome == 1;
	ov.text = reason;
	PlayFx(ov.mine ? "victory" : "defeat");
	return Frames(ov.dur);
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
	clockMs += dt;
	if(clockMs > 1e7f) clockMs = 0.f;
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
			// MD's white-then-cyan special summon flash, as a soft bloom: 70 ms ramp to a capped peak, cyan, gone by 500 ms
			const float a = smooth(0.f, 70.f, t) * (1.f - smooth(110.f, 500.f, t));
			const video::SColor c = lerpc({ 255, 236, 244, 255 }, { 255, 90, 200, 255 }, smooth(60.f, 220.f, t));
			Quad(tGlow, at, 190.f * s, 240.f * s, 0.f, addc(a * 0.62f, c), true);
			Quad(tRing, at, (120.f + 120.f * outCubic(t / 500.f)) * s, (150.f + 150.f * outCubic(t / 500.f)) * s, 0.f, addc(a * 0.35f, c), true);
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
	if(t < 0.f) return;
	if(auto* img = Ui(KeyOf(word))) {
		// title art: punches in from 1.6x with a flash, settles, a glint sweeps across, then lifts and fades
		const float p = outCubic(t / 220.f);
		const float out = smooth(tout, tout + 180.f, t);
		const float a = clamp01(t / 90.f) * (1.f - out);
		const float h = 0.17f * SH();
		const float scale = (1.6f - 0.6f * p) * (1.f + 0.12f * out);
		const float cy = y + h * 0.35f - 20.f * S() * out;
		const float cx = SW() * 0.59f;
		Quad(tGlow, { cx, cy }, h * 6.f, h * 2.2f, 0.f, addc(a * (0.45f + 0.5f * (1.f - p)), glow), true);
		Title(img, cx, cy, h, a, scale, (t - 260.f) / 420.f);
		return;
	}
	if(!F.big) return;
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
	const auto c = Center() + core::vector2df(0.09f * SW(), 0.f); // the board sits right of the card-info panel
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
		auto* linkArt = Ui("link");
		for(int l = 0; l < links; ++l) {
			if(linkArt)
				Quad(linkArt, p0 + d * ((l + 0.5f) / links), 34.f * s, (l % 2 ? 20.f : 34.f) * s, ang + (l % 2 ? 0.f : 1.57f), col(a, { 255, 255, 255, 255 }), false);
			else
				Quad(tLink, p0 + d * ((l + 0.5f) / links), 30.f * s, (l % 2 ? 8.f : 16.f) * s, ang, col(a * 0.95f, { 255, 190, 200, 215 }), false);
		}
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
		if(auto* digit = link <= 6 ? Ui(epro::format("n{}", link)) : nullptr) {
			const core::vector2df corner = at[i] + core::vector2df((i % 2 ? 0.46f : -0.46f) * w, (i % 2 ? 0.34f : -0.40f) * h);
			const float dh = 0.46f * h * (1.f + 0.4f * (1.f - pk));
			Quad(digit, corner, dh, dh, 0.f, col(aa, { 255, 255, 255, 255 }), false);
		} else if(F.num) {
			const std::wstring num = epro::format(L"{}", link);
			const auto d = TextSize(F.num, num);
			const core::vector2df corner = at[i] + core::vector2df((i % 2 ? 0.42f : -0.42f) * w, (i % 2 ? 0.36f : -0.42f) * h);
			Quad(tGlow, corner, d.Width * 3.f, d.Height * 2.2f, 0.f, addc(aa, { 255, 100, 220, 255 }), true);
			Text(F.num, num, corner.X - d.Width / 2.f, corner.Y - d.Height / 2.f, col(aa, { 255, 235, 252, 255 }), aa, 3);
		}
	}
}

// ---------------------------------------------------------------- ace / god summon scene
// The cut-in is a shutter inside the duel, not a video dropped over it: the board dims, the scene opens from a thin
// band across the middle (exposure rising from dark, never a white flash), holds with a slow push-in under cinematic
// edge shading, and closes back down onto the summoned card's row, where the card then lands and settles.
struct Shutter { float top, bot, cy, open, close, dim; };
Shutter CutinShutter(float t, float D) {
	const float H = SH();
	Shutter sh;
	sh.open = outQuint(clamp01((t - 30.f) / 430.f));
	sh.close = smooth(D - 440.f, D - 30.f, t);
	const float zy = ov.hasZone ? ov.zoneScreen.Y : H * 0.5f;
	sh.cy = H * 0.5f + (zy - H * 0.5f) * sh.close * sh.close;
	const float half = H * 0.5f * (0.05f + 0.95f * sh.open) * (1.f - sh.close);
	sh.top = sh.cy - half;
	sh.bot = sh.cy + half;
	sh.dim = smooth(0.f, 240.f, t) * (1.f - smooth(D - 260.f, D, t));
	return sh;
}
void ShutterEdges(const Shutter& sh, video::SColor tint) {
	const float W = SW(), H = SH(), s = S();
	if(sh.bot - sh.top < 1.f) return;
	// dark gradient inside both edges (cinematic bars that travel with the shutter) and a vignette
	const float lb = std::min(0.13f * H, (sh.bot - sh.top) * 0.35f);
	const auto edge = col(0.88f, { 255, 0, 0, 0 }), clear = col(0.f, { 255, 0, 0, 0 });
	drv->draw2DRectangle(core::rect<s32>(0, (s32)sh.top, (s32)W, (s32)(sh.top + lb)), edge, edge, clear, clear);
	drv->draw2DRectangle(core::rect<s32>(0, (s32)(sh.bot - lb), (s32)W, (s32)sh.bot), clear, clear, edge, edge);
	BandQuad(tVig, Center(), W * 1.02f, H * 1.02f, sh.top, sh.bot, col(0.75f, { 255, 0, 0, 0 }), false);
	// thin gold blades on the shutter edges while it moves (hidden off-screen while it is fully open)
	const float blade = 1.f - 0.85f * sh.open * (1.f - sh.close);
	if(blade > 0.02f) {
		const s32 bw = std::max<s32>(1, (s32)(1.5f * s));
		FillRect(0, (s32)sh.top - bw, (s32)W, (s32)sh.top + bw, col(0.75f * blade, { 255, 236, 206, 140 }));
		FillRect(0, (s32)sh.bot - bw, (s32)W, (s32)sh.bot + bw, col(0.75f * blade, { 255, 236, 206, 140 }));
		Quad(tStreak, { W * 0.5f, sh.top }, W * 1.1f, 14.f * s, 0.f, addc(0.22f * blade, tint), true);
		Quad(tStreak, { W * 0.5f, sh.bot }, W * 1.1f, 14.f * s, 0.f, addc(0.22f * blade, tint), true);
	}
}
void DrawCutin() {
	const float t = ov.t, D = ov.dur, s = S();
	const float W = SW(), H = SH();
	const auto c = Center();
	const Shutter sh = CutinShutter(t, D);
	const float live = sh.open * (1.f - sh.close);
	// the board stays, dimmed and cooled
	FillRect(0, 0, (s32)W, (s32)H, col(0.6f * sh.dim, { 255, 4, 6, 14 }));
	if(ov.video > 0) {
		// authored scene, tone-mapped (SceneLut), exposure rising from dark as the shutter opens
		const int frame = std::min(ov.video, 1 + (int)(t * SCENE_FPS * SCENE_SPEED / 1000.f));
		auto* tex = SceneTexture(ov.videoCode, frame);
		if(tex) {
			const auto ts = tex->getOriginalSize();
			const float cover = std::max(W / ts.Width, H / ts.Height);
			const float k = cover * (1.07f - 0.07f * outCubic(t / 900.f)) * (1.f + 0.025f * t / D);
			const float exposure = (0.3f + 0.7f * smooth(0.f, 480.f, t)) * (1.f - 0.55f * sh.close) * 0.94f;
			BandQuad(tex, c, ts.Width * k, ts.Height * k, sh.top, sh.bot, gain(exposure), false);
		}
	} else {
		// procedural: the illustration itself, enlarged and softened, is the backdrop (a tinted field, never black)
		const auto illo = Illustration(ov.code);
		if(auto* art = illo.first) {
			const float bg = 1.25f * W;
			for(int i = 0; i < 6; ++i) {
				const float ang = i * PI / 3.f, r = 14.f * s;
				BandQuad(art, illo.second, c + core::vector2df(std::cos(ang) * r, std::sin(ang) * r), bg, bg, sh.top, sh.bot,
						 col(0.3f, { 255, 120, 120, 132 }), false);
			}
		}
		BandQuad(tGlow, c, W * 1.3f, H * 1.6f, sh.top, sh.bot, col(0.22f * live, ov.tint), false);
		// beats run 200 ms later than MD's (the shutter needs the first 420 ms)
		const float bt = t - 200.f;
		// radial speed lines converging on the centre
		const float lines = window(bt, 380.f, D - 600.f, 160.f, 260.f) * live;
		for(int i = 0; i < 28; ++i) {
			const float ang = hash(i) * 2.f * PI;
			const float r0 = (0.42f + 0.25f * hash(i + 40)) * W, len = (0.18f + 0.2f * hash(i + 80)) * W;
			const float drift = std::fmod(t * 0.0006f + hash(i + 120), 1.f);
			const float r = r0 - drift * 0.12f * W;
			const core::vector2df dir(std::cos(ang), std::sin(ang));
			const core::vector2df at = c + dir * (r + len * 0.5f);
			if(at.Y < sh.top || at.Y > sh.bot) continue;
			Quad(tStreak, at, len, (5.f + 7.f * hash(i + 3)) * s, ang, col(lines * 0.5f, { 255, 0, 0, 0 }), false);
		}
		// bokeh orbs drifting at the left and right edges
		for(int i = 0; i < 10; ++i) {
			const float side = i % 2 ? 1.f : -1.f;
			const float x = c.X + side * (0.36f + 0.1f * hash(i + 7)) * W;
			const float y = H * (1.1f - std::fmod(hash(i + 11) + t * 0.00008f * (1.f + hash(i)), 1.2f));
			if(y < sh.top || y > sh.bot) continue;
			const float r = (40.f + 70.f * hash(i + 5)) * s;
			Quad(tGlow, { x, y }, r, r, 0.f, addc(lines * 0.22f, ov.tint), true);
		}
		// the illustration: ~70% of screen height, pushing in 0.88 -> 1.0, slow parallax drift
		const auto illo2 = Illustration(ov.code);
		if(auto* art = illo2.first) {
			const float push = 0.88f + 0.12f * outCubic(t / (D - 300.f));
			const float h = 0.70f * H * push, w = h;
			const core::vector2df p = c + core::vector2df(t * 0.010f * s, -0.04f * H);
			BandQuad(tGlow, p, w * 1.45f, h * 1.45f, sh.top, sh.bot, addc(0.2f * live, ov.tint), true);
			// a soft dark rim so the square crop sits in the scene instead of floating on it
			BandQuad(tGlow, p, w * 1.25f, h * 1.25f, sh.top, sh.bot, col(0.55f, { 255, 0, 0, 0 }), false);
			BandQuad(art, illo2.second, p, w, h, sh.top, sh.bot, gain(0.92f * (0.3f + 0.7f * smooth(0.f, 480.f, t))), false);
			BandQuad(tFeather, p, w * 1.01f, h * 1.01f, sh.top, sh.bot, col(1.f, { 255, 8, 10, 18 }), false);
			// tendrils of energy whipping in from the edges
			if(bt > 100.f && bt < 420.f) {
				const float k = clamp01((bt - 100.f) / 200.f);
				for(int i = 0; i < 6; ++i) {
					const float a0 = hash(i + 200) * 2.f * PI;
					core::vector2df prev = p + core::vector2df(std::cos(a0), std::sin(a0)) * (0.75f * W);
					for(int seg = 1; seg <= 10; ++seg) {
						const float u = seg / 10.f;
						if(u > k) break;
						const float rr = (0.75f - 0.55f * u) * W;
						const float aa = a0 + 1.4f * u * (i % 2 ? 1.f : -1.f);
						const core::vector2df q = p + core::vector2df(std::cos(aa), std::sin(aa)) * rr;
						if(q.Y > sh.top && q.Y < sh.bot)
							Line(prev, q, 6.f * s, addc((1.f - smooth(300.f, 420.f, bt)) * 0.38f, { 255, 220, 210, 120 }));
						prev = q;
					}
				}
			}
			// lightning crawling over the art: short, thin, half the old intensity
			const bool bolt = (bt > 300.f && bt < 400.f) || (bt > 1000.f && bt < 1060.f);
			if(bolt && live > 0.9f) {
				if(ov.acc > 40.f || ov.bolts.empty()) {
					ov.acc = 0.f;
					ov.bolts.clear();
					for(int b = 0; b < 2; ++b) {
						core::vector2df q = p + core::vector2df(frand(-0.45f, 0.45f) * w, frand(-0.45f, 0.45f) * h);
						const float dir = frand(0.f, 2.f * PI);
						for(int seg = 0; seg < 8; ++seg) {
							const float da = dir + frand(-0.9f, 0.9f);
							const core::vector2df n = q + core::vector2df(std::cos(da), std::sin(da)) * (frand(16.f, 36.f) * s);
							ov.bolts.push_back({ q, n });
							q = n;
						}
					}
				}
				for(auto& b : ov.bolts) {
					Line(b.first, b.second, 7.f * s, addc(0.4f, { 255, 150, 220, 255 }));
					Line(b.first, b.second, 2.f * s, addc(0.55f, { 255, 235, 245, 255 }));
				}
			}
			// gold sparkle burst around the silhouette, twinkling after
			if(ov.beat == 0 && bt >= 300.f) {
				ov.beat = 1;
				for(int i = 0; i < 34; ++i) {
					const float a = frand(0.f, 2.f * PI);
					const core::vector2df q = p + core::vector2df(std::cos(a) * w * frand(0.35f, 0.6f), std::sin(a) * h * frand(0.35f, 0.6f));
					sparks.push_back({ q, core::vector2df(frand(-15.f, 15.f), frand(-25.f, 5.f)) * s, -frand(0.f, 400.f), frand(700.f, 1000.f),
									   frand(8.f, 15.f) * s, 0.f, 0.f, 0.f, { 255, 200, 168, 86 }, 1 });
				}
			}
		}
	}
	ShutterEdges(sh, ov.tint);
	// two-tier name plate across the lower ~15%, sliding in from the left, out before the shutter closes
	if(t < ov.plateStart) return;
	const float tp = t - ov.plateStart;
	const float slide = outQuint(tp / 260.f) * (1.f - smooth(D - 560.f, D - 380.f, t));
	const float plateA = clamp01(tp / 120.f) * (1.f - smooth(D - 560.f, D - 380.f, t));
	const float textA = plateA;
	if(plateA <= 0.f) return;
	const s32 y0 = (s32)(0.77f * H), y1 = (s32)(0.855f * H), y2 = (s32)(0.915f * H);
	const s32 right = (s32)(W * 0.78f * slide);
	const video::SColor band = col(0.80f * plateA, { 255, 6, 10, 22 }), bandEnd = col(0.f, { 255, 6, 10, 22 });
	drv->draw2DRectangle(core::rect<s32>(0, y0, right, y1), band, bandEnd, band, bandEnd);
	const video::SColor rule = col(plateA * 0.85f, { 255, 220, 210, 170 }), ruleEnd = col(0.f, { 255, 220, 210, 170 });
	drv->draw2DRectangle(core::rect<s32>(0, y0 - (s32)(2 * s), right, y0), rule, ruleEnd, rule, ruleEnd);
	const s32 inner = (s32)(W * 0.52f * slide);
	const video::SColor pl = col(0.92f * plateA, ov.plate), plEnd = col(0.f, ov.plate);
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
	// level stars pop in left to right, ~30 ms apart (white dot, then gold); the summon method tag after them
	float tagX = textX + 4.f * s;
	for(int i = 0; i < ov.stars && i < 13; ++i) {
		const float st = 220.f + i * 30.f;
		const core::vector2df p(textX + 14.f * s + i * 27.f * s, y0 - 18.f * s);
		tagX = p.X + 22.f * s;
		if(tp < st) continue;
		const float k = clamp01((tp - st) / 90.f);
		const float sz = (22.f + 8.f * (1.f - k)) * s;
		Quad(tGlow, p, sz * 2.f, sz * 2.f, 0.f, addc(textA * (1.f - k) * 0.5f, { 255, 255, 255, 255 }), true);
		Quad(tStar, p, sz, sz, 0.f, col(textA, lerpc({ 255, 255, 255, 255 }, { 255, 255, 196, 60 }, k)), false);
	}
	if(ov.method != M_NONE && F.tag) {
		video::SColor a, b;
		MethodColors(ov.method, a, b);
		const std::wstring tag = MethodName(ov.method);
		const auto d = TextSize(F.tag, tag);
		Text(F.tag, tag, tagX + 8.f * s, y0 - 18.f * s - d.Height / 2.f, col(textA * smooth(200.f, 400.f, tp), a), textA * 0.9f, 1);
	}
}

// ---------------------------------------------------------------- summon method (Fusion / Synchro / Xyz / Link / Ritual / Pendulum)
// ~1.55 s: the board dims, the materials gather into the method's figure (vortex, rings, galaxy, link arrows, ritual
// circle, pendulum scales), the new monster's card rises out of it with its method title, then flies into its zone.
// Additive light is capped low throughout: the figure reads as light in the dark, never as a flash.
float outBack(float t) { t = clamp01(t); const float c1 = 1.5f, c3 = c1 + 1.f; const float u = t - 1.f; return 1.f + c3 * u * u * u + c1 * u * u; }
void DrawMethod() {
	const float t = ov.t, D = ov.dur, s = S();
	const float W = SW(), H = SH();
	const int m = ov.method;
	video::SColor ca, cb;
	MethodColors(m, ca, cb);
	const core::vector2df c = Center() + core::vector2df(0.07f * W, -0.03f * H); // the board sits right of the info panel
	const float dim = smooth(0.f, 220.f, t) * (1.f - smooth(D - 300.f, D, t));
	FillRect(0, 0, (s32)W, (s32)H, col(0.70f * dim, { 255, 4, 6, 14 }));
	Quad(tVig, Center(), W * 1.05f, H * 1.05f, 0.f, col(0.7f * dim, { 255, 0, 0, 0 }), false);
	const float fig = smooth(120.f, 420.f, t) * (1.f - smooth(D - 420.f, D - 120.f, t)); // the method figure
	const float R = 0.34f * H;
	// coloured fog round the figure: alpha-blended, so it tints the dark instead of adding light
	Quad(tGlow, c, R * 4.2f, R * 3.0f, 0.f, col(0.2f * fig, ca), false);
	const float spin = t * 0.0011f;
	// 1. the figure
	switch(m) {
	case M_FUSION: {
		// a two-colour vortex: three spiral arms of soft light round a dark core
		Quad(tGlow, c, R * 1.3f, R * 1.3f, 0.f, col(0.65f * fig, { 255, 10, 4, 20 }), false);
		for(int arm = 0; arm < 3; ++arm)
			for(int i = 0; i < 22; ++i) {
				const float u = i / 21.f;
				const float ang = arm * 2.f * PI / 3.f + spin * 2.4f + u * 3.6f;
				const float r = R * (0.12f + 0.9f * u);
				const core::vector2df q = c + core::vector2df(std::cos(ang) * r, std::sin(ang) * r * 0.82f);
				const float sz = (30.f + 50.f * u) * s;
				Quad(tGlow, q, sz, sz, 0.f, addc(fig * 0.5f * (1.f - 0.5f * u), lerpc(cb, ca, u)), true);
				if(i % 3 == 0) Quad(tSpark, q, sz * 0.5f, sz * 0.5f, ang, addc(fig * 0.45f, { 255, 255, 236, 210 }), true);
			}
		Quad(tRing, c, R * 1.15f, R * 0.94f, spin, addc(0.45f * fig, ca), true);
		Quad(tRing, c, R * 0.7f, R * 0.58f, -spin * 1.6f, addc(0.35f * fig, cb), true);
		break;
	}
	case M_SYNCHRO: {
		// a column of light with three tilted rings sliding down it; the level's stars stream through
		Quad(tStreak, c, H * 1.2f, 0.16f * H, PI / 2.f, addc(0.32f * fig, cb), true);
		Quad(tStreak, c, H * 1.2f, 0.035f * H, PI / 2.f, addc(0.5f * fig, ca), true);
		for(int i = 0; i < 3; ++i) {
			const float k = std::fmod(t * 0.0007f + i / 3.f, 1.f);
			const float y = c.Y - 0.36f * H + k * 0.72f * H;
			const float a = fig * std::sin(k * PI);
			Quad(tRing, { c.X, y }, R * 1.6f, R * 0.40f, 0.f, addc(0.75f * a, ca), true);
			Quad(tRing, { c.X, y }, R * 1.64f, R * 0.43f, 0.f, addc(0.4f * a, cb), true);
			Quad(tGlow, { c.X, y }, R * 1.7f, R * 0.5f, 0.f, addc(0.12f * a, cb), true);
		}
		const int n = std::max(3, std::min(12, ov.stars));
		for(int i = 0; i < n; ++i) {
			const float k = std::fmod(t * 0.0009f + hash(i + 31), 1.f);
			const core::vector2df q(c.X + (hash(i + 5) - 0.5f) * 0.10f * H, c.Y - 0.42f * H + k * 0.84f * H);
			const float a = fig * std::sin(k * PI);
			Quad(tStar, q, 24.f * s, 24.f * s, 0.f, col(a, { 255, 255, 226, 120 }), false);
			Quad(tGlow, q, 48.f * s, 48.f * s, 0.f, addc(0.4f * a, cb), true);
		}
		break;
	}
	case M_XYZ: {
		// a galaxy: a dark core, a gold disc of orbiting motes, two thin orbits
		Quad(tGlow, c, R * 1.6f, R * 1.6f, 0.f, col(0.8f * fig, { 255, 2, 2, 10 }), false);
		Quad(tRays, c, R * 2.5f, R * 2.5f, -spin * 0.5f, addc(0.16f * fig, ca), true);
		for(int i = 0; i < 46; ++i) {
			const float ang = hash(i) * 2.f * PI + spin * (1.6f + hash(i + 9));
			const float r = R * (0.35f + 0.75f * hash(i + 17));
			const core::vector2df q = c + core::vector2df(std::cos(ang) * r, std::sin(ang) * r * 0.36f);
			const float sz = (14.f + 24.f * hash(i + 23)) * s;
			Quad(tGlow, q, sz, sz, 0.f, addc(fig * 0.8f, i % 4 ? ca : cb), true);
			if(i % 5 == 0) Quad(tSpark, q, sz * 0.8f, sz * 0.8f, 0.f, addc(fig * 0.5f, { 255, 255, 240, 200 }), true);
		}
		Quad(tRing, c, R * 1.9f, R * 0.66f, 0.f, addc(0.45f * fig, ca), true);
		Quad(tRing, c, R * 1.4f, R * 0.48f, 0.f, addc(0.35f * fig, cb), true);
		break;
	}
	case M_LINK: {
		// the Link arrows round the card: dark sockets, the monster's own arrows light up one by one
		static const int bit[8] = { 0x80, 0x100, 0x20, 0x4, 0x2, 0x1, 0x8, 0x40 }; // N NE E SE S SW W NW
		int lit = 0;
		for(int i = 0; i < 8; ++i) {
			const float ang = -PI / 2.f + i * PI / 4.f;
			const core::vector2df dir(std::cos(ang), std::sin(ang));
			const core::vector2df q = c + dir * (R * 0.92f);
			const bool on = (ov.markers & bit[i]) != 0;
			const float lightT = 380.f + lit * 90.f;
			const float k = on ? smooth(lightT, lightT + 140.f, t) : 0.f;
			if(on) ++lit;
			Quad(tArrow, q, 60.f * s, 60.f * s, ang, col(0.9f * fig, { 255, 14, 20, 34 }), false);
			Quad(tArrow, q, 48.f * s, 48.f * s, ang, col(0.8f * fig, { 255, 52, 72, 104 }), false);
			if(k > 0.f) {
				Quad(tArrow, q, 48.f * s, 48.f * s, ang, col(fig * k, cb), false);
				Quad(tGlow, q, 120.f * s, 120.f * s, 0.f, addc(0.5f * fig * k, cb), true);
				// circuit line from the arrow into the centre
				Line(c + dir * (R * 0.2f), q - dir * (26.f * s), 7.f * s, addc(0.55f * fig * k, ca));
			}
		}
		// the circuit frame: a square on its corner
		for(int i = 0; i < 4; ++i) {
			const float a0 = i * PI / 2.f + spin * 0.3f, a1 = a0 + PI / 2.f;
			Line(c + core::vector2df(std::cos(a0), std::sin(a0)) * (R * 0.62f), c + core::vector2df(std::cos(a1), std::sin(a1)) * (R * 0.62f),
				 8.f * s, addc(0.6f * fig, ca));
			Line(c + core::vector2df(std::cos(a0), std::sin(a0)) * (R * 0.70f), c + core::vector2df(std::cos(a1), std::sin(a1)) * (R * 0.70f),
				 4.f * s, addc(0.35f * fig, ca));
		}
		Quad(tGlow, c, R * 1.1f, R * 1.1f, 0.f, addc(0.2f * fig, ca), true);
		break;
	}
	case M_RITUAL: {
		// a ritual circle on the floor: two rings and rotating ticks, light rising from it
		const core::vector2df fl = c + core::vector2df(0.f, 0.17f * H);
		Quad(tRing, fl, R * 2.1f, R * 0.7f, 0.f, addc(0.7f * fig, ca), true);
		Quad(tRing, fl, R * 1.7f, R * 0.56f, 0.f, addc(0.5f * fig, cb), true);
		Quad(tGlow, fl, R * 2.2f, R * 0.8f, 0.f, addc(0.15f * fig, ca), true);
		for(int i = 0; i < 24; ++i) {
			const float ang = i * 2.f * PI / 24.f + spin * 0.6f;
			const core::vector2df dir(std::cos(ang), std::sin(ang) * 0.333f);
			Line(fl + dir * (R * 0.86f), fl + dir * (R * 0.98f), 5.f * s, addc(0.55f * fig, ca));
		}
		Quad(tStreak, fl - core::vector2df(0.f, 0.25f * H), 0.55f * H, R * 1.4f, PI / 2.f, addc(0.28f * fig, ca), true);
		while(ov.acc > 30.f && fig > 0.3f) {
			ov.acc -= 30.f;
			const float a = frand(0.f, 2.f * PI);
			sparks.push_back({ fl + core::vector2df(std::cos(a) * R * 0.9f, std::sin(a) * R * 0.3f), core::vector2df(frand(-10.f, 10.f), -frand(80.f, 170.f)) * s,
							   0.f, frand(600.f, 1000.f), frand(5.f, 10.f) * s, 0.f, 0.f, 0.f, cb, 1 });
		}
		break;
	}
	case M_PENDULUM: {
		// the two scales as pillars of light and a pendulum swinging between them, leaving an arc
		for(int side = -1; side <= 1; side += 2) {
			const core::vector2df pl = c + core::vector2df(side * 0.34f * H, 0.f);
			Quad(tStreak, pl, H * 1.1f, 0.16f * H, PI / 2.f, addc(0.3f * fig, side < 0 ? ca : cb), true);
			Quad(tStreak, pl, H * 1.1f, 0.04f * H, PI / 2.f, addc(0.6f * fig, side < 0 ? ca : cb), true);
		}
		const core::vector2df pivot = c - core::vector2df(0.f, 0.36f * H);
		const float swing = std::sin(t * 0.0062f) * 0.75f;
		for(int i = 0; i < 26; ++i) {
			const float u = i / 25.f;
			const float ang = PI / 2.f + (u - 0.5f) * 1.5f;
			const core::vector2df q = pivot + core::vector2df(std::cos(ang), std::sin(ang)) * (0.56f * H);
			Quad(tGlow, q, 30.f * s, 30.f * s, 0.f, addc(0.5f * fig, lerpc(ca, cb, u)), true);
		}
		const core::vector2df bob = pivot + core::vector2df(std::cos(PI / 2.f + swing), std::sin(PI / 2.f + swing)) * (0.56f * H);
		Line(pivot, bob, 4.f * s, addc(0.5f * fig, { 255, 230, 230, 240 }));
		Quad(tGlow, bob, 110.f * s, 110.f * s, 0.f, addc(0.6f * fig, lerpc(ca, cb, 0.5f + swing * 0.6f)), true);
		Quad(tSpark, bob, 60.f * s, 60.f * s, t * 0.002f, addc(0.5f * fig, { 255, 255, 250, 240 }), true);
		break;
	}
	default: break;
	}
	// 2. the materials gather into the figure (their card art, spiralling in and shrinking away)
	const int nm = (int)ov.codes.size();
	for(int i = 0; i < nm; ++i) {
		auto* art = Art(ov.codes[i]);
		if(!art) continue;
		const float t0 = 60.f + i * 50.f;
		const float k = outCubic((t - t0 - 160.f) / 520.f);
		const float a = smooth(t0, t0 + 160.f, t) * (1.f - smooth(0.7f, 1.f, k));
		if(a <= 0.f) continue;
		const float base = -PI / 2.f + (i + 0.5f) * 2.f * PI / std::max(1, nm) + (nm == 1 ? PI : 0.f);
		const float twist = m == M_FUSION ? 2.4f : m == M_XYZ ? 3.2f : m == M_SYNCHRO ? 0.f : 0.9f;
		const float ang = base + twist * k;
		const float r = (0.40f * H) * (1.f - k);
		core::vector2df q = c + core::vector2df(std::cos(ang) * r, std::sin(ang) * r * 0.8f);
		if(m == M_SYNCHRO) q = core::vector2df(c.X + std::cos(base) * r, c.Y - 0.36f * H * k + std::sin(base) * r * 0.5f);
		const float h = 0.20f * H * (1.f - 0.7f * k), w = h * 0.686f;
		Quad(tGlow, q, w * 1.8f, h * 1.5f, 0.f, addc(0.25f * a, ca), true);
		Quad(art, Full(art), q, w, h, (ang - base) * 0.3f, col(a, lerpc({ 255, 255, 255, 255 }, ca, k)), false);
	}
	// 3. the new monster's card rises out of the figure, then flies into its zone
	auto* card = Art(ov.code);
	if(card) {
		const float tin = 620.f, tout = D - 400.f;
		const float k = outBack((t - tin) / 360.f);
		const float fly = smooth(tout, D - 60.f, t);
		const float a = smooth(tin, tin + 160.f, t) * (1.f - smooth(0.75f, 1.f, fly));
		if(a > 0.f) {
			core::vector2df to = ov.zoneScreen;
			if(to.X < -900.f || !ov.hasZone) to = c;
			const float fe = fly * fly;
			const core::vector2df p = c + (to - c) * fe;
			const float h = 0.48f * H * (0.6f + 0.4f * k) * (1.f - 0.8f * fe), w = h * 0.686f;
			Quad(tGlow, p, w * 2.1f, h * 1.7f, 0.f, addc(0.32f * a, ca), true);
			Quad(tGlow, p, w * 1.25f, h * 1.12f, 0.f, col(0.5f * a, { 255, 0, 0, 0 }), false);
			Quad(card, Full(card), p, w, h, 0.f, col(a, { 255, 255, 255, 255 }), false);
			// an energised rim in the method's colour
			const float rim = a * (0.45f + 0.15f * std::sin(t * 0.012f));
			const core::vector2df q0 = p + core::vector2df(-w / 2.f, -h / 2.f), q1 = p + core::vector2df(w / 2.f, -h / 2.f);
			const core::vector2df q2 = p + core::vector2df(w / 2.f, h / 2.f), q3 = p + core::vector2df(-w / 2.f, h / 2.f);
			Line(q0, q1, 10.f * s, addc(rim, ca)); Line(q1, q2, 10.f * s, addc(rim, ca));
			Line(q2, q3, 10.f * s, addc(rim, ca)); Line(q3, q0, 10.f * s, addc(rim, ca));
			// a single soft sheen across the face as it arrives (no white-out)
			const float sheen = clamp01((t - tin - 120.f) / 380.f);
			if(sheen > 0.f && sheen < 1.f)
				Quad(tStreak, p + core::vector2df((sheen - 0.5f) * w * 1.3f, 0.f), h * 0.9f, w * 0.5f, 1.25f, addc(0.3f * a * std::sin(sheen * PI), { 255, 255, 250, 235 }), true);
			if(ov.beat == 0 && t >= tin + 120.f) {
				ov.beat = 1;
				Burst(p, 26, lerpc(ca, { 255, 255, 255, 255 }, 0.3f), 300.f, 11.f, 700.f, 1);
			}
		}
		// 4. the method title under the card
		const float ta = smooth(tin + 120.f, tin + 300.f, t) * (1.f - smooth(tout - 150.f, tout + 60.f, t));
		if(ta > 0.f) {
			const std::wstring title = MethodName(m);
			const float ty = c.Y + 0.31f * H;
			if(auto* img = Ui(KeyOf(title))) {
				const float p = outCubic((t - tin - 120.f) / 260.f);
				Quad(tGlow, { c.X, ty }, 0.9f * H, 0.16f * H, 0.f, addc(0.22f * ta, ca), true);
				Title(img, c.X, ty, 0.085f * H, ta, 1.25f - 0.25f * p, (t - tin - 380.f) / 420.f);
			} else if(F.plate) {
				const auto d = TextSize(F.plate, title);
				Quad(tGlow, { c.X, ty }, d.Width * 1.8f, d.Height * 2.4f, 0.f, addc(0.25f * ta, ca), true);
				Text(F.plate, title, c.X - d.Width / 2.f, ty - d.Height / 2.f, col(ta, { 255, 255, 246, 225 }), ta, 2);
			}
		}
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
		const video::SColor f0 = col(0.f, { 255, 255, 120, 20 }), f1 = col(fire * 0.5f, { 255, 210, 78, 14 });
		drv->draw2DRectangle(core::rect<s32>(0, top, (s32)W, (s32)H), f0, f0, f1, f1);
		FillRect(0, 0, (s32)W, (s32)H, col(full * 0.34f, { 255, 200, 92, 24 }));
		while(ov.acc > 12.f) {
			ov.acc -= 12.f;
			// additive flames stack fast: two per tick at ~half intensity keeps the fire orange instead of a white-gold sheet
			for(int k = 0; k < 2; ++k) {
				const bool ash = frand(0.f, 1.f) < 0.16f;
				sparks.push_back({ core::vector2df(frand(0.f, W), H + 30.f * s), core::vector2df(frand(-30.f, 30.f), -frand(260.f, 620.f)) * s, 0.f, frand(900.f, 1700.f),
								   (ash ? frand(5.f, 9.f) : frand(60.f, 150.f)) * s, -60.f * s, 0.f, 0.f,
								   ash ? video::SColor(255, 20, 10, 10) : video::SColor(255, 150, (u32)frand(46.f, 96.f), 14), (uint8_t)(ash ? 2 : 0) });
			}
		}
	} else {
		ov.acc = 0.f;
	}
	// authored scene (pics/mdfx/scene/33396948/): Exodia rises from the portal, 3800-9300, under the fire and flare
	const int exFrames = SceneFrames(33396948);
	if(exFrames > 0 && t > 3700.f && t < 9700.f) {
		const int frame = std::min(exFrames, 1 + (int)(std::max(0.f, t - 3900.f) * SCENE_FPS / 1000.f));
		SceneFrame(33396948, frame, smooth(3700.f, 4200.f, t) * (1.f - smooth(9350.f, 9650.f, t)));
	}
	// 3700-5750 a dark figure emerges in the fire; 5750 reveal slash; hold and push in
	const auto headIllo = Illustration(33396948);
	auto* head = exFrames > 0 ? nullptr : headIllo.first;
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
			FillRect(0, 0, (s32)W, (s32)H, col((1.f - k) * 0.28f, { 255, 255, 228, 180 }));
		}
		// 6600-9200 an energy orb gathers between the hands, wind arcs orbiting
		const float charge = clamp01((t - 6600.f) / 2600.f);
		if(t > 6600.f) {
			const core::vector2df orb = p + core::vector2df(0.f, 0.12f * h);
			const float r = (30.f + 220.f * charge * charge) * s;
			Quad(tGlow, orb, r * 2.4f, r * 2.4f, 0.f, addc(0.5f, { 255, 255, 220, 130 }), true);
			Quad(tGlow, orb, r, r, 0.f, addc(0.6f, { 255, 255, 246, 225 }), true);
			for(int i = 0; i < 4; ++i)
				Quad(tRing, p, h * (0.7f + 0.12f * i), h * (0.32f + 0.05f * i), t * 0.004f * (i % 2 ? 1.f : -1.3f) + i, addc(0.45f * charge, { 255, 240, 245, 255 }), true);
		}
	}
	// 9200-9400 flare + gold shockwave rings, 9400-10000 white-out, then back to the field
	if(t > 9200.f && t < 9900.f) {
		for(int i = 0; i < 3; ++i) {
			const float k = clamp01((t - 9200.f - i * 80.f) / 500.f);
			const float r = W * 1.6f * outCubic(k);
			Quad(tRing, c, r, r, 0.f, addc(0.6f * (1.f - k), { 255, 255, 214, 100 }), true);
		}
	}
	const float white = smooth(9300.f, 9600.f, t) * (1.f - smooth(9950.f, 10400.f, t));
	// MD whites out here; ours is a warm haze that stops well short of white
	if(white > 0.f) FillRect(0, 0, (s32)W, (s32)H, col(white * 0.38f, { 255, 214, 190, 150 }));
}

// ---------------------------------------------------------------- phase / turn bands (~0.9 s)
void DrawBanner() {
	const float t = ov.t, D = ov.dur, s = S();
	const float W = SW(), H = SH();
	if(auto* img = Ui(ov.turn ? std::string(ov.mine ? "your_turn" : "opponents_turn") : KeyOf(ov.text))) {
		// band sweeps open from the centre, the title slides in fast and eases, glint, then both slide out
		const float open = outQuint(t / 200.f), close = smooth(D - 170.f, D, t);
		const float a = open * (1.f - close);
		const float bandH = 0.15f * H * (ov.turn ? 1.2f : 1.f);
		const float cy = 0.5f * H;
		if(auto* band = Ui("band"))
			Quad(band, { W / 2.f, cy }, W * 1.05f * open, bandH, 0.f, col(a * 0.95f, { 255, 255, 255, 255 }), false);
		else
			FillRect(0, (s32)(cy - bandH / 2.f), (s32)W, (s32)(cy + bandH / 2.f), col(a * 0.7f, { 255, 8, 12, 20 }));
		const video::SColor glow = ov.turn ? (ov.mine ? video::SColor(255, 60, 200, 210) : video::SColor(255, 220, 40, 40))
										   : video::SColor(255, 200, 160, 70);
		Quad(tGlow, { W / 2.f, cy }, W * 0.9f, bandH * 1.8f, 0.f, addc(a * (ov.turn ? 0.6f : 0.35f), glow), true);
		const float slideIn = outQuint((t - 40.f) / 260.f), slideOut = smooth(D - 200.f, D, t);
		const float tx = W / 2.f + (1.f - slideIn) * -0.35f * W + slideOut * 0.25f * W;
		Title(img, tx, cy, bandH * 0.78f, clamp01((t - 40.f) / 120.f) * (1.f - slideOut), 1.f, (t - 300.f) / 380.f);
		if(ov.turn && t < 200.f)
			Quad(tStreak, { W * (t / 200.f), cy }, W * 0.5f, bandH, 0.f, addc(0.9f * (1.f - t / 200.f), { 255, 255, 255, 255 }), true);
		return;
	}
	if(!F.big) return;
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

namespace {
void DrawResult() {
	const float t = ov.t, D = ov.dur, s = S();
	const float W = SW(), H = SH();
	const auto c = Center();
	const float a = smooth(0.f, 300.f, t) * (1.f - smooth(D - 350.f, D, t));
	FillRect(0, 0, (s32)W, (s32)H, col(0.72f * a, { 255, 2, 3, 6 }));
	const video::SColor tint = ov.mine ? video::SColor(255, 255, 196, 90) : video::SColor(255, 200, 30, 30);
	Quad(tRays, c, H * 1.9f, H * 1.9f, t * 0.00012f, addc(a * (ov.mine ? 0.5f : 0.3f), tint), true);
	Quad(tGlow, c, W * 1.1f, H * 0.9f, 0.f, addc(a * 0.55f, tint), true);
	if(auto* img = Ui(ov.mine ? "victory" : "defeat")) {
		const float p = outQuint(t / 420.f);
		const float h = 0.30f * H;
		Title(img, c.X, c.Y - 0.04f * H, h, a, 1.35f - 0.35f * p, (t - 500.f) / 700.f);
	}
	if(!ov.text.empty() && F.statline) {
		const auto d = TextSize(F.statline, ov.text);
		Text(F.statline, ov.text, c.X - d.Width / 2.f, c.Y + 0.16f * H, col(a * smooth(400.f, 700.f, t), { 255, 240, 230, 205 }), a, 1);
	}
	if(ov.mine && ov.beat == 0 && t > 250.f) {
		ov.beat = 1;
		Burst(c, 90, { 255, 255, 214, 110 }, 520.f * s, 16.f * s, 1500.f, 1, 140.f * s);
	}
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
	if(sceneTex && !(ov.kind == Overlay::CUTIN && ov.video > 0) && ov.kind != Overlay::EXODIA) DropSceneTex();
	if(ov.hasZone) {
		const auto zs = coll->getScreenCoordinatesFrom3DPosition(ov.zone);
		ov.zoneScreen = core::vector2df((float)zs.X, (float)zs.Y);
	}
	switch(ov.kind) {
	case Overlay::CUTIN: DrawCutin(); break;
	case Overlay::METHOD: DrawMethod(); break;
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
	case Overlay::RESULT: DrawResult(); break;
	default: break;
	}
	DrawSparks();
	if(pendingNum.valid) {
		DrawLpNumber(pendingNum.text, pendingNum.player, pendingNum.frames);
		pendingNum.valid = false;
	}
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

// --- duel HUD -------------------------------------------------------------------------------------
namespace {
struct HudArt {
	bool tried = false;
	video::ITexture *plate = nullptr, *you = nullptr, *opp = nullptr, *turn = nullptr;
} hud;
float trail[2] = { 1.f, 1.f }, trailHold[2] = { 0.f, 0.f }, lastGauge[2] = { 1.f, 1.f }, hitFlash[2] = { 0.f, 0.f };
uint32_t lastClock = 0;

void LoadHud() {
	if(hud.tried) return;
	hud.tried = true;
	hud.plate = LoadFile(EPRO_TEXT("./pics/mdfx/hud/plate.png"));
	hud.you = LoadFile(EPRO_TEXT("./pics/mdfx/hud/avatar_you.png"));
	hud.opp = LoadFile(EPRO_TEXT("./pics/mdfx/hud/avatar_opp.png"));
	hud.turn = LoadFile(EPRO_TEXT("./pics/mdfx/hud/turn.png"));
}
gui::CGUITTFont* FitFont(float maxH) {
	gui::CGUITTFont* best = nullptr;
	u32 bestH = 0;
	for(auto* f : { F.big, F.lp, F.plate, F.num }) {
		if(!f) continue;
		const u32 h = TextSize(f, L"0123456789").Height;
		if(h <= maxH && h > bestH) { best = f; bestH = h; }
	}
	return best ? best : F.plate;
}
// one plate: x0,y0 top-left, w,h size; mirrored = opponent (ring on the right)
void Plate(float x0, float y0, float w, float h, bool mirrored, int lp, int startlp, const std::wstring& name,
		   bool active, int p, float dtMs) {
	const float pulse = 0.5f + 0.5f * std::sin(clockMs * 0.004f);
	const float cx = x0 + w / 2.f, cy = y0 + h / 2.f;
	// turn player: soft turquoise aura behind the plate
	if(active)
		Quad(tGlow, { cx, cy }, w * 1.25f, h * 2.1f, 0.f, addc(0.30f + 0.18f * pulse, { 255, 70, 220, 210 }), true);
	// portrait inside the ring (ring centre at 12.2% of the plate width)
	const float ringX = mirrored ? x0 + w * (1.f - 0.122f) : x0 + w * 0.122f;
	auto* avatar = p == 0 ? hud.you : hud.opp;
	Quad(avatar, { ringX, cy }, h * 0.86f, h * 0.86f, 0.f, video::SColor(255, 255, 255, 255), false);
	// the plate itself (mirrored by flipping the source rect)
	const auto ps = hud.plate->getOriginalSize();
	const core::rect<s32> src = mirrored ? core::rect<s32>(ps.Width, 0, 0, ps.Height) : core::rect<s32>(0, 0, ps.Width, ps.Height);
	const u8 dim = active ? 255 : 205;
	Quad(hud.plate, src, { cx, cy }, w, h, 0.f, video::SColor(255, dim, dim, dim), false);
	// bar region: x 25%..93% (mirrored 7%..75%), y 21%..87%
	const float bx0 = mirrored ? x0 + w * 0.07f : x0 + w * 0.255f;
	const float bx1 = mirrored ? x0 + w * 0.745f : x0 + w * 0.93f;
	const float by0 = y0 + h * 0.21f, by1 = y0 + h * 0.87f, bh = by1 - by0;
	// LP gauge with a lagging damage trail
	const float gauge = startlp > 0 ? clamp01((float)lp / (float)startlp) : 0.f;
	if(gauge < lastGauge[p] - 0.0001f) { trailHold[p] = 450.f; hitFlash[p] = 1.f; }
	if(gauge > trail[p]) trail[p] = gauge;
	lastGauge[p] = gauge;
	if(trailHold[p] > 0.f) trailHold[p] -= dtMs;
	else trail[p] = std::max(gauge, trail[p] - dtMs / 900.f);
	hitFlash[p] = std::max(0.f, hitFlash[p] - dtMs / 380.f);
	const float gy0 = by1 - bh * 0.24f, gy1 = by1 - bh * 0.10f;
	const float gx0 = bx0 + (bx1 - bx0) * 0.03f, gx1 = bx1 - (bx1 - bx0) * 0.06f, gw = gx1 - gx0;
	FillRect((s32)gx0, (s32)gy0, (s32)gx1, (s32)gy1, { 200, 4, 6, 8 });
	auto seg = [&](float a, float b, video::SColor c0, video::SColor c1) {
		// mirrored plates drain toward the ring side as well (right to left for you, left to right for them)
		const float l = mirrored ? gx1 - gw * b : gx0 + gw * a;
		const float r = mirrored ? gx1 - gw * a : gx0 + gw * b;
		if(r - l < 1.f) return;
		drv->draw2DRectangle(core::rect<s32>((s32)l, (s32)gy0, (s32)r, (s32)gy1), c0, c1, c0, c1);
	};
	if(trail[p] > gauge) seg(gauge, trail[p], { 230, 255, 240, 220 }, { 230, 255, 190, 150 });
	const video::SColor g0 = p == 0 ? video::SColor(255, 150, 104, 30) : video::SColor(255, 110, 12, 22);
	const video::SColor g1 = p == 0 ? video::SColor(255, 255, 220, 120) : video::SColor(255, 240, 70, 70);
	seg(0.f, gauge, mirrored ? g1 : g0, mirrored ? g0 : g1);
	// name on top, "LP" + number under it, both on the portrait side
	auto* nf = F.name ? F.name : F.statline;
	const float pad = (bx1 - bx0) * 0.05f;
	float rowY = by0 + bh * 0.05f;
	if(!name.empty()) {
		const auto nd = TextSize(nf, name);
		const float nmx = mirrored ? bx1 - pad - nd.Width : bx0 + pad;
		Text(nf, name, nmx, rowY, active ? video::SColor(255, 250, 240, 215) : video::SColor(255, 175, 165, 145), 0.85f, 1);
		rowY += nd.Height;
	}
	auto* f = FitFont((gy0 - rowY) * 1.02f);
	const std::wstring lps = std::to_wstring(std::max(0, lp));
	const auto d = TextSize(f, lps);
	const std::wstring tag = L"LP";
	const auto td = TextSize(nf, tag);
	const float ny = rowY + (gy0 - rowY - d.Height) * 0.5f;
	video::SColor nc = gauge < 0.25f ? video::SColor(255, 255, 140, 120) : video::SColor(255, 250, 244, 228);
	nc = lerpc(nc, video::SColor(255, 255, 70, 60), hitFlash[p]);
	const float gap = h * 0.06f;
	const float tagX = mirrored ? bx1 - pad - d.Width - gap - td.Width : bx0 + pad;
	const float numX = mirrored ? bx1 - pad - d.Width : bx0 + pad + td.Width + gap;
	Text(nf, tag, tagX, ny + d.Height - td.Height - 1.f, { 255, 232, 196, 106 }, 0.8f, 1);
	Text(f, lps, numX, ny, nc, 0.85f, 2);
}
} // namespace

bool DrawHud(const HudInfo& info) {
	if(!Enabled()) return false;
	LoadHud();
	if(!hud.plate || !hud.you || !hud.opp) return false;
	const uint32_t now = (uint32_t)clockMs;
	const float dtMs = lastClock ? (float)(now - lastClock) : 16.f;
	lastClock = now;
	const float W = SW(), H = SH(), sx = W / 1024.f, sy = H / 640.f;
	const float h = 62.f * sy;
	const auto ps = hud.plate->getOriginalSize();
	const float w = h * (float)ps.Width / (float)ps.Height;
	const float leftX = 322.f * sx, rightX = 1018.f * sx - w, y = 4.f * sy;
	if(layoutActive) {
		// Master Duel: you bottom-left, the opponent top-right, the turn count beside the opponent's plate
		const float myY = H - h - 6.f * sy;
		Plate(6.f * sx, myY, w, h, false, info.lp[0], info.startlp, info.name[0], info.myTurn, 0, dtMs);
		Plate(rightX, y, w, h, true, info.lp[1], info.startlp, info.name[1], !info.myTurn, 1, dtMs);
		if(hud.turn) {
			const float ms = h * 0.95f, mx = rightX - ms * 0.62f, my = y + h * 0.5f;
			Quad(hud.turn, { mx, my }, ms, ms, 0.f, video::SColor(255, 255, 255, 255), false);
			const std::wstring tn = std::to_wstring(info.turn);
			auto* f = FitFont(ms * 0.36f);
			const auto d = TextSize(f, tn);
			Text(f, tn, mx - d.Width / 2.f, my - d.Height / 2.f - 1.f, { 255, 250, 236, 200 }, 0.9f, 2);
		}
		return true;
	}
	Plate(leftX, y, w, h, false, info.lp[0], info.startlp, info.name[0], info.myTurn, 0, dtMs);
	Plate(rightX, y, w, h, true, info.lp[1], info.startlp, info.name[1], !info.myTurn, 1, dtMs);
	// turn medallion between the plates
	if(hud.turn) {
		const float mx = (leftX + w + rightX) / 2.f, my = y + h * 0.5f, ms = h * 1.08f;
		Quad(hud.turn, { mx, my }, ms, ms, 0.f, video::SColor(255, 255, 255, 255), false);
		const std::wstring tn = std::to_wstring(info.turn);
		auto* f = FitFont(ms * 0.36f);
		const auto d = TextSize(f, tn);
		Text(f, tn, mx - d.Width / 2.f, my - d.Height / 2.f - 1.f, { 255, 250, 236, 200 }, 0.9f, 2);
	}
	return true;
}

// --- Battle Royal HUD ---------------------------------------------------------------------------
// Bottom seat: the usual plate on the left. Every other seat: a compact plate (ring + bar + lotus cap
// cut from the same plate art) in a fixed order on the right, so a seat never moves around. A centre
// pill says whether the top half is LIVE or a frozen board being viewed.
namespace {
struct Toast { std::wstring text; int kind = 0; float t = -1.f; } toast;
const video::SColor kSeatAccent[4] = { { 255, 226, 186, 96 }, { 255, 214, 84, 84 }, { 255, 86, 196, 214 }, { 255, 168, 128, 236 } };
const video::SColor kTeamAccent[2] = { { 255, 232, 190, 92 }, { 255, 84, 196, 226 } };
video::SColor Accent(int team, int idx) { return team >= 0 ? kTeamAccent[team & 1] : kSeatAccent[idx & 3]; }
// the plate art in three slices: ring (left cap), bar (stretched), lotus tip (right cap)
void MiniPlate(float x0, float y0, float w, float h, video::SColor tint) {
	const auto ps = hud.plate->getOriginalSize();
	const float pw = (float)ps.Width, ph = (float)ps.Height;
	const float ringW = ph * 1.0f, capW = pw * 0.13f;      // source widths
	const float dRing = h * ringW / ph, dCap = h * capW / ph; // drawn widths
	const float mid = std::max(1.f, w - dRing - dCap);
	Quad(hud.plate, core::rect<s32>(0, 0, (s32)ringW, (s32)ph), { x0 + dRing / 2.f, y0 + h / 2.f }, dRing, h, 0.f, tint, false);
	Quad(hud.plate, core::rect<s32>((s32)ringW, 0, (s32)(pw - capW), (s32)ph), { x0 + dRing + mid / 2.f, y0 + h / 2.f }, mid + 1.f, h, 0.f, tint, false);
	Quad(hud.plate, core::rect<s32>((s32)(pw - capW), 0, (s32)pw, (s32)ph), { x0 + dRing + mid + dCap / 2.f, y0 + h / 2.f }, dCap, h, 0.f, tint, false);
}
void SmallPill(const core::recti& r, video::SColor edge, float a) {
	const s32 x0 = r.UpperLeftCorner.X, y0 = r.UpperLeftCorner.Y, x1 = r.LowerRightCorner.X, y1 = r.LowerRightCorner.Y;
	drv->draw2DRectangle(core::rect<s32>(x0 + 1, y0 + 1, x1 - 1, y1 - 1), col(a * 0.92f, { 255, 26, 30, 40 }), col(a * 0.92f, { 255, 26, 30, 40 }),
						 col(a * 0.92f, { 255, 10, 12, 17 }), col(a * 0.92f, { 255, 10, 12, 17 }));
	const auto e = col(a, edge), ed = col(a * 0.55f, edge);
	drv->draw2DLine({ x0 + 2, y0 }, { x1 - 2, y0 }, e);
	drv->draw2DLine({ x0 + 2, y1 - 1 }, { x1 - 2, y1 - 1 }, ed);
	drv->draw2DLine({ x0, y0 + 2 }, { x0, y1 - 2 }, ed);
	drv->draw2DLine({ x1 - 1, y0 + 2 }, { x1 - 1, y1 - 2 }, ed);
}
// tiny card / stack glyphs for the hand and deck counts
void CardGlyph(float x, float y, float h, int stack, video::SColor c) {
	const float w = h * 0.7f;
	for(int i = stack - 1; i >= 0; --i) {
		const s32 ox = (s32)(x + i * h * 0.16f), oy = (s32)(y - i * h * 0.16f);
		drv->draw2DRectangle(col(0.9f, { 255, 18, 20, 26 }), core::rect<s32>(ox, oy, ox + (s32)w, oy + (s32)h));
		drv->draw2DRectangleOutline(core::rect<s32>(ox, oy, ox + (s32)w, oy + (s32)h), c);
	}
}
void DrawToast(float W, float H, float sx, float sy, float dtMs) {
	if(toast.t < 0.f || toast.text.empty()) return;
	toast.t += std::min(dtMs, 100.f);
	const float D = 2200.f;
	if(toast.t > D) { toast.t = -1.f; return; }
	auto* f = F.plate ? F.plate : F.name;
	if(!f) return;
	const float a = smooth(0.f, 160.f, toast.t) * (1.f - smooth(D - 350.f, D, toast.t));
	const auto d = TextSize(f, toast.text);
	const float cx = 660.f * sx, cy = 248.f * sy + (1.f - outCubic(toast.t / 260.f)) * 8.f * sy;
	const float pw = d.Width + 48.f * sx, ph = d.Height + 12.f * sy;
	const video::SColor edge = toast.kind == 1 ? video::SColor(255, 236, 92, 80) : toast.kind == 2 ? video::SColor(255, 190, 170, 160) : video::SColor(255, 222, 186, 100);
	SmallPill(core::recti((s32)(cx - pw / 2.f), (s32)(cy - ph / 2.f), (s32)(cx + pw / 2.f), (s32)(cy + ph / 2.f)), edge, a);
	const video::SColor tc = toast.kind == 1 ? video::SColor(255, 255, 210, 200) : video::SColor(255, 250, 240, 220);
	Text(f, toast.text, cx - d.Width / 2.f, cy - d.Height / 2.f, col(a, tc), a * 0.8f, 1);
}
} // namespace

void SeatToast(const std::wstring& text, int kind) {
	toast.text = text;
	toast.kind = kind;
	toast.t = 0.f;
}

namespace {
// allies gold, opponents crimson (teams); a colour per seat in a free-for-all
const video::SColor kAlly(255, 232, 190, 92), kEnemy(255, 222, 78, 70), kTurnC(255, 92, 226, 212), kTargetC(255, 255, 96, 84);
video::SColor SeatColor(bool you, bool ally, int team, int idx) {
	if(you || ally) return kAlly;
	if(team >= 0) return kEnemy;
	return kSeatAccent[idx & 3];
}
// s cut to maxW with an ellipsis
std::wstring Fit(gui::CGUITTFont* f, const std::wstring& s, float maxW) {
	if(!f || TextSize(f, s).Width <= maxW) return s;
	std::wstring t = s;
	while(!t.empty() && TextSize(f, t + L"…").Width > maxW) t.pop_back();
	while(!t.empty() && t.back() == L' ') t.pop_back();
	return t.empty() ? t : t + L"…";
}
// a small rounded tag: filled (badge) or outlined
core::recti Tag(gui::CGUITTFont* f, const std::wstring& s, float x, float y, float h, video::SColor c, bool filled, float a, bool rightAlign = false) {
	const auto d = TextSize(f, s);
	const float w = d.Width + h * 0.9f;
	const float x0 = rightAlign ? x - w : x;
	const core::recti r((s32)x0, (s32)y, (s32)(x0 + w), (s32)(y + h));
	if(filled) {
		const video::SColor top = lerpc(c, { 255, 20, 20, 24 }, 0.35f), bot = lerpc(c, { 255, 10, 10, 12 }, 0.62f);
		drv->draw2DRectangle(core::rect<s32>(r.UpperLeftCorner.X + 1, r.UpperLeftCorner.Y, r.LowerRightCorner.X - 1, r.LowerRightCorner.Y), col(a, top), col(a, top), col(a, bot), col(a, bot));
		drv->draw2DLine({ r.UpperLeftCorner.X + 1, r.UpperLeftCorner.Y }, { r.LowerRightCorner.X - 1, r.UpperLeftCorner.Y }, col(a, lerpc(c, { 255, 255, 255, 255 }, 0.35f)));
		Text(f, s, x0 + (w - d.Width) / 2.f, y + (h - d.Height) / 2.f, col(a, { 255, 255, 250, 240 }), a * 0.7f, 1);
	} else {
		SmallPill(r, c, a);
		Text(f, s, x0 + (w - d.Width) / 2.f, y + (h - d.Height) / 2.f, col(a, lerpc(c, { 255, 255, 255, 255 }, 0.45f)), a * 0.7f, 1);
	}
	return r;
}
void Frame(const core::recti& r, video::SColor c) {
	const s32 x0 = r.UpperLeftCorner.X, y0 = r.UpperLeftCorner.Y, x1 = r.LowerRightCorner.X - 1, y1 = r.LowerRightCorner.Y - 1;
	drv->draw2DLine({ x0, y0 }, { x1, y0 }, c);
	drv->draw2DLine({ x0, y1 }, { x1, y1 }, c);
	drv->draw2DLine({ x0, y0 }, { x0, y1 }, c);
	drv->draw2DLine({ x1, y0 }, { x1, y1 }, c);
}
} // namespace

// Top bar: your plate on the left, then your teammates, the turn medallion, then the opponents, every seat
// in a fixed place. Each seat has one status tab under it (TURN / TARGET / ALLY / OUT); the TABLE button under
// the medallion opens the table with every board.
bool DrawSeatHud(const SeatHud& in, SeatHudRects& out) {
	out.chip.assign(in.chips.size(), {});
	out.target.assign(in.chips.size(), {});
	out.back = {};
	out.table = {};
	if(!drv) return false;
	LoadHud();
	const float W = SW(), H = SH(), sx = W / 1024.f, sy = H / 640.f;
	const float pulse = 0.5f + 0.5f * std::sin(clockMs * 0.004f);
	const bool art = Enabled() && hud.plate && hud.you && hud.opp;
	const uint32_t now = (uint32_t)clockMs;
	const float dtMs = lastClock ? (float)(now - lastClock) : 16.f;
	lastClock = now;
	auto* nameF = F.name ? F.name : F.statline;
	auto* tagF = F.tag ? F.tag : nameF;
	auto* smallF = F.plain ? F.plain : nameF;
	const float tabH = 13.f * sy;
	// your plate
	const float h = 56.f * sy, y = 5.f * sy, px = 310.f * sx;
	float plateW = 236.f * sx;
	if(art) {
		const auto ps = hud.plate->getOriginalSize();
		plateW = h * (float)ps.Width / (float)ps.Height;
		Plate(px, y, plateW, h, false, in.lp, in.startlp, in.name, in.bottomTurn, 0, dtMs);
	} else {
		SmallPill(core::recti((s32)px, (s32)y, (s32)(px + plateW), (s32)(y + h)), kAlly, 1.f);
		Text(nameF, in.name, px + 10.f * sx, y + 6.f * sy, { 255, 250, 240, 215 }, 0.8f, 1);
		Text(FitFont(h * 0.5f), std::to_wstring(std::max(0, in.lp)), px + 10.f * sx, y + h * 0.42f, { 255, 250, 244, 228 }, 0.85f, 2);
	}
	{
		const float tx = px + plateW * 0.255f;
		float x = tx;
		x = (float)Tag(tagF, L"YOU", x, y + h + 2.f * sy, tabH, kAlly, false, 0.9f).LowerRightCorner.X + 4.f * sx;
		if(in.bottomTurn) Tag(tagF, L"TURN", x, y + h + 2.f * sy, tabH, kTurnC, true, 0.75f + 0.25f * pulse);
	}
	// split the other seats: allies before the medallion, everyone else after it
	std::vector<size_t> allies, rivals;
	for(size_t i = 0; i < in.chips.size(); ++i)
		(in.chips[i].ally ? allies : rivals).push_back(i);
	const float gap = 6.f * sx, medS = h * 0.74f;
	const float x0 = px + plateW + gap, x1 = 1018.f * sx;
	const size_t n = in.chips.size();
	const float avail = x1 - x0 - medS - gap * 2.f - gap * (n ? n - 1 : 0);
	const float cw = n ? std::min(172.f * sx, avail / (float)n) : 0.f;
	const float ch = 48.f * sy, cy0 = y + (h - ch) * 0.5f;
	std::vector<float> chipX(n, 0.f);
	float cx = x0;
	for(size_t i : allies) { chipX[i] = cx; cx += cw + gap; }
	const float medX = cx + medS * 0.5f, medY = y + h * 0.5f;
	cx += medS + gap;
	// opponents are right-aligned so the gap sits between the two sides
	const float rivalsW = rivals.empty() ? 0.f : rivals.size() * cw + (rivals.size() - 1) * gap;
	cx = std::max(cx, x1 - rivalsW);
	for(size_t i : rivals) { chipX[i] = cx; cx += cw + gap; }
	// turn medallion + TABLE button
	if(art && hud.turn) {
		Quad(hud.turn, { medX, medY }, medS, medS, 0.f, video::SColor(255, 255, 255, 255), false);
	} else {
		SmallPill(core::recti((s32)(medX - medS / 2.f), (s32)(medY - medS / 2.f), (s32)(medX + medS / 2.f), (s32)(medY + medS / 2.f)), kAlly, 1.f);
	}
	{
		const std::wstring tn = std::to_wstring(in.turn);
		auto* f = FitFont(medS * 0.38f);
		const auto d = TextSize(f, tn);
		Text(f, tn, medX - d.Width / 2.f, medY - d.Height / 2.f - 1.f, { 255, 250, 236, 200 }, 0.9f, 2);
	}
	{
		const std::wstring bt = in.tableOpen ? L"CLOSE TABLE" : L"TABLE";
		const auto d = TextSize(tagF, bt);
		const float bw = d.Width + 18.f * sx, bx = medX - bw / 2.f, by = y + h + 2.f * sy;
		const core::recti r((s32)bx, (s32)by, (s32)(bx + bw), (s32)(by + tabH + 2.f * sy));
		out.table = r;
		const video::SColor e = in.tableHover || in.tableOpen ? video::SColor(255, 255, 232, 160) : kAlly;
		if(in.tableHover) Quad(tGlow, { medX, by + tabH * 0.5f }, bw * 1.5f, tabH * 3.f, 0.f, addc(0.35f, { 255, 255, 210, 120 }), true);
		SmallPill(r, e, 1.f);
		Text(tagF, bt, bx + (bw - d.Width) / 2.f, by + (tabH + 2.f * sy - d.Height) / 2.f, in.tableHover ? video::SColor(255, 255, 244, 210) : video::SColor(255, 240, 220, 170), 0.8f, 1);
	}
	// the seats
	for(size_t i = 0; i < n; ++i) {
		const auto& c = in.chips[i];
		const float x = chipX[i];
		const core::recti rect((s32)x, (s32)cy0, (s32)(x + cw), (s32)(cy0 + ch));
		out.chip[i] = rect;
		const video::SColor accent = SeatColor(c.isYou, c.ally, c.team, c.colorIndex);
		const float ringD = ch, ringCX = x + ringD / 2.f, ringCY = cy0 + ch / 2.f;
		if(c.alive && c.turn)
			Quad(tGlow, { x + cw / 2.f, ringCY }, cw * 1.2f, ch * 1.9f, 0.f, addc(0.24f + 0.14f * pulse, { 255, 70, 220, 210 }), true);
		else if(c.alive && c.target)
			Quad(tGlow, { x + cw / 2.f, ringCY }, cw * 1.15f, ch * 1.7f, 0.f, addc(0.18f + 0.08f * pulse, { 255, 230, 60, 50 }), true);
		if(c.canTarget && (in.targetHint || c.hovered))
			Quad(tGlow, { x + cw / 2.f, ringCY }, cw * 1.1f, ch * 1.6f, 0.f, addc((c.hovered ? 0.32f : 0.12f) + 0.16f * pulse, { 255, 255, 110, 80 }), true);
		const u8 lum = (u8)(c.alive ? (c.hovered ? 255 : 220) : 105);
		if(art) {
			Quad(c.isYou ? hud.you : hud.opp, { ringCX, ringCY }, ringD * 0.84f, ringD * 0.84f, 0.f,
				 video::SColor(255, (u8)(lum * 0.5f), (u8)(lum * 0.5f), (u8)(lum * 0.5f)), false);
			MiniPlate(x, cy0, cw, ch, video::SColor(255, lum, lum, lum));
		} else {
			SmallPill(rect, accent, 1.f);
		}
		if(!c.name.empty()) {
			const std::wstring ini(1, (wchar_t)std::towupper(c.name[0]));
			auto* bf = FitFont(ringD * 0.44f);
			const auto d = TextSize(bf, ini);
			Text(bf, ini, ringCX - d.Width / 2.f, ringCY - d.Height / 2.f, col(c.alive ? 1.f : 0.45f, accent), 0.9f, 2);
		}
		// text between the ring and the lotus cap: name, then LP + hand
		const float tx0 = x + ringD * 0.98f, tx1 = x + cw - ch * 0.34f;
		const core::recti clip((s32)tx0, (s32)cy0, (s32)tx1, (s32)(cy0 + ch));
		const float top = cy0 + ch * 0.17f;
		const std::wstring nm = Fit(nameF, c.name, tx1 - tx0);
		const auto nd = TextSize(nameF, nm);
		Text(nameF, nm, tx0, top, c.alive ? video::SColor(255, 250, 240, 215) : video::SColor(255, 150, 140, 130), 0.85f, 1, &clip);
		const float rowY = top + nd.Height, gaugeY0 = cy0 + ch * 0.79f, gaugeY1 = cy0 + ch * 0.85f;
		if(c.alive) {
			auto* lf = FitFont((gaugeY0 - rowY) * 1.08f);
			const std::wstring lps = std::to_wstring(std::max(0, c.lp));
			const auto ld = TextSize(lf, lps);
			const float ly = rowY + (gaugeY0 - rowY - ld.Height) * 0.5f;
			const video::SColor lc = c.lp * 4 < in.startlp ? video::SColor(255, 255, 140, 120) : video::SColor(255, 250, 244, 228);
			Text(lf, lps, tx0, ly, lc, 0.85f, 1, &clip);
			const float gh = (gaugeY0 - rowY) * 0.5f;
			const std::wstring hs = std::to_wstring(c.hand);
			const auto hd = TextSize(smallF, hs);
			const float hx = tx1 - hd.Width - gh * 0.7f - 3.f * sx;
			if(hx > tx0 + ld.Width + 4.f * sx) {
				const float gy = rowY + (gaugeY0 - rowY - gh) * 0.5f;
				CardGlyph(hx, gy, gh, 1, { 220, 200, 176, 120 });
				Text(smallF, hs, hx + gh * 0.7f + 3.f * sx, gy + (gh - hd.Height) * 0.5f, { 255, 225, 215, 195 }, 0.7f, 1);
			}
			const float g = in.startlp > 0 ? clamp01((float)c.lp / (float)in.startlp) : 0.f;
			FillRect((s32)tx0, (s32)gaugeY0, (s32)tx1, (s32)gaugeY1, { 200, 4, 6, 8 });
			const video::SColor g0 = lerpc(accent, { 255, 40, 30, 20 }, 0.55f);
			if(g > 0.f)
				drv->draw2DRectangle(core::rect<s32>((s32)tx0, (s32)gaugeY0, (s32)(tx0 + (tx1 - tx0) * g), (s32)gaugeY1), g0, accent, g0, accent);
		} else {
			FillRect((s32)tx0, (s32)gaugeY0, (s32)tx1, (s32)gaugeY1, { 160, 30, 12, 12 });
		}
		// status tab under the plate
		const float tabY = y + h + 2.f * sy;
		float tabX = tx0;
		if(!c.alive) {
			static const wchar_t* pl[]{ L"", L"1ST", L"2ND", L"3RD", L"4TH" };
			Tag(tagF, epro::format(L"OUT {}", c.placement >= 0 && c.placement < 5 ? pl[c.placement] : L""), tabX, tabY, tabH, { 255, 170, 110, 100 }, false, 0.85f);
		} else {
			if(c.ally)
				tabX = (float)Tag(tagF, L"ALLY", tabX, tabY, tabH, kAlly, false, 0.85f).LowerRightCorner.X + 4.f * sx;
			if(c.turn)
				tabX = (float)Tag(tagF, L"TURN", tabX, tabY, tabH, kTurnC, true, 0.75f + 0.25f * pulse).LowerRightCorner.X + 4.f * sx;
			if(c.target)
				tabX = (float)Tag(tagF, L"TARGET", tabX, tabY, tabH, kTargetC, true, 0.9f).LowerRightCorner.X + 4.f * sx;
			if(c.canTarget && (c.hovered || in.targetHint)) {
				const auto r = Tag(tagF, L"ATTACK ▸", tabX, tabY, tabH, kTargetC, c.targetHover || c.hovered, c.hovered ? 1.f : 0.6f + 0.4f * pulse);
				out.target[i] = rect; // the whole plate switches the target
				(void)r;
			}
		}
	}
	DrawToast(W, H, sx, sy, dtMs);
	return true;
}

// --- Battle Royal table ---------------------------------------------------------------------------------
core::recti TableArea() {
	if(!drv) return {};
	const float W = SW(), H = SH(), sx = W / 1024.f, sy = H / 640.f;
	return core::recti((s32)(304.f * sx), (s32)(78.f * sy), (s32)(W - 2.f * sx), (s32)(H - 3.f * sy));
}

namespace {
struct TableCtx {
	float sx, sy, u, a, pulse;
	gui::CGUITTFont *nameF, *tagF, *smallF;
	std::vector<TableHit>* hits;
	const TableView* in;
};
video::ITexture* Back(bool across) {
	auto* t = mainGame->imageManager.tCover[across ? 1 : 0];
	return t ? t : mainGame->imageManager.tCover[0];
}
// an empty zone: a dark recess with a faint edge and an optional label
void Slot(const TableCtx& k, float cx, float cy, float w, float h, video::SColor tint, const wchar_t* label) {
	const core::recti r((s32)(cx - w / 2.f), (s32)(cy - h / 2.f), (s32)(cx + w / 2.f), (s32)(cy + h / 2.f));
	drv->draw2DRectangle(r, col(k.a * 0.55f, { 255, 8, 10, 14 }), col(k.a * 0.55f, { 255, 8, 10, 14 }), col(k.a * 0.55f, { 255, 14, 17, 23 }), col(k.a * 0.55f, { 255, 14, 17, 23 }));
	Frame(r, col(k.a * 0.30f, tint));
	if(label && *label) {
		const auto d = TextSize(k.tagF, label);
		if(d.Width < w * 0.95f)
			Text(k.tagF, label, cx - d.Width / 2.f, cy - d.Height / 2.f, col(k.a * 0.38f, tint), 0.f, 0);
	}
}
// a count badge in a pile's corner
void Count(const TableCtx& k, float x, float y, uint32_t n, video::SColor c) {
	const std::wstring s = std::to_wstring(n);
	const auto d = TextSize(k.tagF, s);
	const float bh = d.Height + 2.f * k.sy, bw = std::max(bh, d.Width + 6.f * k.sx);
	const core::recti r((s32)(x - bw / 2.f), (s32)(y - bh / 2.f), (s32)(x + bw / 2.f), (s32)(y + bh / 2.f));
	drv->draw2DRectangle(col(k.a * 0.9f, { 255, 10, 11, 14 }), r);
	Frame(r, col(k.a * 0.8f, c));
	Text(k.tagF, s, x - d.Width / 2.f, y - d.Height / 2.f, col(k.a, { 255, 250, 240, 220 }), 0.f, 0);
}
// one card, centred; defence position lies sideways (scaled to stay inside its zone)
void CardAt(const TableCtx& k, const TableCard& c, float cx, float cy, float w, float h, bool across, bool own) {
	video::ITexture* tex = nullptr;
	const bool show = c.code && (!c.faceDown || own);
	if(show) tex = Art(c.code);
	if(!tex) tex = Back(across);
	const float rot = c.defense ? PI * 0.5f : 0.f, s = c.defense ? 0.78f : 1.f;
	// Xyz materials peek out behind it
	for(int m = std::min(c.materials, 3); m > 0; --m)
		Quad(Back(across), { cx + m * 2.5f * k.sx, cy - m * 2.5f * k.sy }, w * s, h * s, rot, col(k.a * 0.85f, { 255, 150, 140, 120 }), false);
	const u8 lum = (u8)(c.faceDown && own ? 150 : 255);
	Quad(tex, { cx, cy }, w * s, h * s, rot, col(k.a, { 255, lum, lum, lum }), false);
	if(c.faceDown && own) {
		const auto d = TextSize(k.tagF, L"SET");
		Text(k.tagF, L"SET", cx - d.Width / 2.f, cy - d.Height / 2.f, col(k.a, { 255, 240, 230, 210 }), k.a * 0.9f, 1);
	}
	if(c.materials > 0)
		Count(k, cx + w * 0.40f, cy - h * 0.42f, (uint32_t)c.materials, { 255, 236, 200, 120 });
	// ATK / DEF strip
	if(c.monster && !c.faceDown) {
		const std::wstring st = c.linkMon ? epro::format(L"{} L{}", StatText(c.atk), c.link) : epro::format(L"{}/{}", StatText(c.atk), StatText(c.def));
		const auto d = TextSize(k.smallF, st);
		const float sy0 = c.defense ? cy + h * s * 0.5f * 0.69f - d.Height : cy + h * 0.5f - d.Height - 2.f * k.sy;
		const float bw = std::max((float)d.Width + 6.f * k.sx, w * 0.9f);
		const core::recti r((s32)(cx - bw / 2.f), (s32)(sy0 - 1.f * k.sy), (s32)(cx + bw / 2.f), (s32)(sy0 + d.Height + 1.f * k.sy));
		drv->draw2DRectangle(col(k.a * 0.82f, { 255, 6, 7, 10 }), r);
		Text(k.smallF, st, cx - d.Width / 2.f, sy0, col(k.a, { 255, 255, 246, 226 }), 0.f, 0);
	}
}
void Hit(const TableCtx& k, TableHit::Kind kind, const core::recti& r, int seat, uint32_t loc = 0, uint32_t seq = 0, uint32_t code = 0) {
	TableHit h;
	h.kind = kind; h.rect = r; h.seat = seat; h.location = loc; h.sequence = seq; h.code = code;
	k.hits->push_back(h);
}
core::recti Box(float cx, float cy, float w, float h) {
	return core::recti((s32)(cx - w / 2.f), (s32)(cy - h / 2.f), (s32)(cx + w / 2.f), (s32)(cy + h / 2.f));
}

void DrawBoard(const TableCtx& k, const TableSeat& s, int idx, const core::recti& R) {
	const float x0 = (float)R.UpperLeftCorner.X, y0 = (float)R.UpperLeftCorner.Y, x1 = (float)R.LowerRightCorner.X, y1 = (float)R.LowerRightCorner.Y;
	const float bw = x1 - x0, bh = y1 - y0;
	const video::SColor accent = SeatColor(s.isYou, s.ally, s.team, s.colorIndex);
	const bool hov = k.in->hoverSeat == idx;
	Hit(k, TableHit::BOARD, R, idx);
	// glow behind the turn player / target / a seat just opened from its plate
	if(s.alive && s.turn)
		Quad(tGlow, { (x0 + x1) / 2.f, (y0 + y1) / 2.f }, bw * 1.18f, bh * 1.22f, 0.f, addc(k.a * (0.22f + 0.10f * k.pulse), { 255, 60, 210, 200 }), true);
	else if(s.alive && s.target)
		Quad(tGlow, { (x0 + x1) / 2.f, (y0 + y1) / 2.f }, bw * 1.16f, bh * 1.2f, 0.f, addc(k.a * (0.20f + 0.08f * k.pulse), { 255, 230, 50, 40 }), true);
	if(k.in->focusSeat == idx)
		Quad(tGlow, { (x0 + x1) / 2.f, (y0 + y1) / 2.f }, bw * 1.2f, bh * 1.25f, 0.f, addc(k.a * 0.25f, { 255, 255, 240, 200 }), true);
	// panel
	const video::SColor pTop = s.across ? video::SColor(255, 24, 14, 16) : video::SColor(255, 14, 20, 26), pBot(255, 7, 8, 11);
	drv->draw2DRectangle(R, col(k.a * 0.93f, s.across ? pTop : pBot), col(k.a * 0.93f, s.across ? pTop : pBot), col(k.a * 0.93f, s.across ? pBot : pTop), col(k.a * 0.93f, s.across ? pBot : pTop));
	const video::SColor edge = s.alive && s.turn ? kTurnC : s.alive && s.target ? kTargetC : accent;
	Frame(R, col(k.a * (hov ? 1.f : 0.75f), edge));
	Frame(core::recti(R.UpperLeftCorner + core::vector2di(2, 2), R.LowerRightCorner - core::vector2di(2, 2)), col(k.a * 0.22f, edge));
	// header (outer edge), hand strip next to it, zones toward the centre of the table
	const float hh = std::min(30.f * k.sy, bh * 0.15f), handH = std::min(32.f * k.sy, bh * 0.15f), pad = 5.f * k.u;
	const float hy0 = s.across ? y0 : y1 - hh, hy1 = hy0 + hh;
	const float handY0 = s.across ? hy1 + 2.f * k.sy : hy0 - 2.f * k.sy - handH;
	const float zy0 = s.across ? handY0 + handH + pad : y0 + pad, zy1 = s.across ? y1 - pad : handY0 - pad;
	// header strip
	{
		const video::SColor hb0 = lerpc(edge, { 255, 10, 10, 12 }, 0.78f), hb1(255, 10, 11, 14);
		drv->draw2DRectangle(core::rect<s32>((s32)x0 + 1, (s32)hy0 + (s.across ? 1 : 0), (s32)x1 - 1, (s32)hy1 - (s.across ? 0 : 1)), col(k.a, hb0), col(k.a, hb1), col(k.a, hb0), col(k.a, hb1));
		const float gy = s.across ? hy1 - 2.f * k.sy : hy0;
		const float g = k.in->startlp > 0 ? clamp01((float)s.lp / (float)k.in->startlp) : 0.f;
		FillRect((s32)x0 + 1, (s32)gy, (s32)x1 - 1, (s32)(gy + 2.f * k.sy), { (u32)(200 * k.a), 4, 5, 7 });
		if(s.alive && g > 0.f) {
			const video::SColor g0 = lerpc(accent, { 255, 40, 30, 20 }, 0.5f);
			drv->draw2DRectangle(core::rect<s32>((s32)x0 + 1, (s32)gy, (s32)(x0 + 1 + (bw - 2) * g), (s32)(gy + 2.f * k.sy)), col(k.a, g0), col(k.a, accent), col(k.a, g0), col(k.a, accent));
		}
		const float ringD = hh * 0.80f, rcx = x0 + pad + ringD / 2.f, rcy = (hy0 + hy1) / 2.f;
		if(hud.opp && hud.you)
			Quad(s.isYou ? hud.you : hud.opp, { rcx, rcy }, ringD, ringD, 0.f, col(k.a, { 255, 120, 120, 120 }), false);
		else
			Quad(tRing, { rcx, rcy }, ringD, ringD, 0.f, col(k.a, accent), false);
		if(!s.name.empty()) {
			const std::wstring ini(1, (wchar_t)std::towupper(s.name[0]));
			auto* bf = FitFont(ringD * 0.5f);
			const auto d = TextSize(bf, ini);
			Text(bf, ini, rcx - d.Width / 2.f, rcy - d.Height / 2.f, col(k.a, accent), 0.9f, 2);
		}
		// LP on the right
		auto* lf = FitFont(hh * 0.62f);
		const std::wstring lps = s.alive ? std::to_wstring(std::max(0, s.lp)) : L"0";
		const auto ld = TextSize(lf, lps);
		const float lx = x1 - pad - ld.Width;
		const video::SColor lc = !s.alive ? video::SColor(255, 150, 120, 110) : g < 0.25f ? video::SColor(255, 255, 140, 120) : video::SColor(255, 250, 244, 228);
		Text(lf, lps, lx, rcy - ld.Height / 2.f, col(k.a, lc), k.a * 0.85f, 2);
		const auto lpd = TextSize(k.tagF, L"LP");
		Text(k.tagF, L"LP", lx - lpd.Width - 3.f * k.sx, rcy + ld.Height / 2.f - lpd.Height - 1.f * k.sy, col(k.a, { 255, 232, 196, 106 }), 0.f, 0);
		// badges, right to left before the LP
		float bx = lx - lpd.Width - 9.f * k.sx;
		const float tH = std::min(13.f * k.sy, hh * 0.5f), tY = rcy - tH / 2.f;
		auto badge = [&](const std::wstring& t, video::SColor c, bool filled, float a) {
			const auto r = Tag(k.tagF, t, bx, tY, tH, c, filled, k.a * a, true);
			bx = (float)r.UpperLeftCorner.X - 4.f * k.sx;
			return r;
		};
		if(!s.alive) {
			static const wchar_t* pl[]{ L"", L"1ST", L"2ND", L"3RD", L"4TH" };
			badge(epro::format(L"OUT {}", s.placement >= 0 && s.placement < 5 ? pl[s.placement] : L""), { 255, 170, 110, 100 }, false, 0.9f);
		} else {
			if(s.canTarget) {
				const bool on = hov || k.in->hoverHit >= 0;
				const auto r = badge(L"ATTACK ▸", kTargetC, true, hov ? 1.f : 0.65f + 0.35f * k.pulse);
				Hit(k, TableHit::TARGET, core::recti(r.UpperLeftCorner - core::vector2di(2, 2), r.LowerRightCorner + core::vector2di(2, 2)), idx);
				(void)on;
			}
			if(s.target) badge(L"TARGET", kTargetC, true, 0.95f);
			if(s.turn) badge(L"TURN", kTurnC, true, 0.75f + 0.25f * k.pulse);
			if(s.isYou) badge(L"YOU", kAlly, false, 0.9f);
			else if(s.ally) badge(L"ALLY", kAlly, false, 0.9f);
		}
		// name + deck between the ring and the badges
		const float nx = rcx + ringD / 2.f + 6.f * k.sx, nmax = bx - nx - 4.f * k.sx;
		if(nmax > 10.f) {
			const std::wstring nm = Fit(k.nameF, s.name, nmax);
			const auto nd = TextSize(k.nameF, nm);
			const std::wstring dk = s.deckName.empty() ? L"" : Fit(k.smallF, s.deckName, nmax);
			const auto dd = TextSize(k.smallF, dk);
			const float total = nd.Height + (dk.empty() ? 0.f : dd.Height * 0.92f);
			const float ny = rcy - total / 2.f;
			Text(k.nameF, nm, nx, ny, col(k.a, s.alive ? video::SColor(255, 250, 240, 215) : video::SColor(255, 150, 140, 130)), k.a * 0.85f, 1);
			if(!dk.empty())
				Text(k.smallF, dk, nx, ny + nd.Height * 0.92f, col(k.a * 0.8f, { 255, 200, 186, 160 }), 0.f, 0);
		}
	}
	if(!s.known) {
		const std::wstring t = L"board not revealed yet";
		const auto d = TextSize(k.smallF, t);
		Text(k.smallF, t, (x0 + x1 - d.Width) / 2.f, (zy0 + zy1 - d.Height) / 2.f, col(k.a * 0.6f, { 255, 210, 200, 180 }), 0.f, 0);
		return;
	}
	// zones: 7 columns (pile, 5 zones, pile) x 3 rows (extra monster, monster, spell/trap)
	const float g = 4.f * k.u;
	const float zw = bw - 2.f * pad, zh = zy1 - zy0;
	const float cw = std::min((zw - 6.f * g) / 7.f, ((zh - 2.f * g) / 3.f) / 1.46f), chh = cw * 1.46f;
	const float gridW = 7.f * cw + 6.f * g, gridH = 3.f * chh + 2.f * g;
	const float gx0 = x0 + (bw - gridW) / 2.f, gy0 = zy0 + (zh - gridH) / 2.f;
	const bool own = s.isYou;
	auto cellC = [&](int col_, int row) {
		// row 0 = extra monster zones (centre side), 1 = monsters, 2 = spells/traps (outer side); across boards mirror
		const int c = s.across ? 6 - col_ : col_;
		const int r = s.across ? 2 - row : row;
		return core::vector2df(gx0 + c * (cw + g) + cw / 2.f, gy0 + r * (chh + g) + chh / 2.f);
	};
	const video::SColor mz(255, 200, 150, 90), st(255, 90, 170, 160), fz(255, 120, 200, 110), pile(255, 170, 160, 140);
	auto zone = [&](int c, int r, const TableCard& card, video::SColor tint, const wchar_t* label, uint32_t loc, uint32_t seq) {
		const auto p = cellC(c, r);
		Slot(k, p.X, p.Y, cw, chh, tint, card.present ? nullptr : label);
		if(card.present) {
			CardAt(k, card, p.X, p.Y, cw, chh, s.across, own);
			if(card.code && (!card.faceDown || own))
				Hit(k, TableHit::CARD, Box(p.X, p.Y, card.defense ? chh * 0.78f : cw, card.defense ? cw * 0.78f : chh), idx, loc, seq, card.code);
		}
	};
	// extra monster zones above monster zones 2 and 4 (seen from the owner's side)
	zone(2, 0, s.mz[5], mz, L"EX", LOCATION_MZONE, 5);
	zone(4, 0, s.mz[6], mz, L"EX", LOCATION_MZONE, 6);
	for(int i = 0; i < 5; ++i) {
		zone(1 + i, 1, s.mz[i], mz, nullptr, LOCATION_MZONE, (uint32_t)i);
		zone(1 + i, 2, s.sz[i], st, nullptr, LOCATION_SZONE, (uint32_t)i);
	}
	zone(0, 1, s.sz[5], fz, L"FIELD", LOCATION_SZONE, 5);
	// piles: banished (above the GY), GY, deck, extra deck
	auto pileAt = [&](int c, int r, const TableCard& top, uint32_t n, uint32_t loc, const wchar_t* label, bool back) {
		const auto p = cellC(c, r);
		Slot(k, p.X, p.Y, cw, chh, pile, n ? nullptr : label);
		if(n) {
			if(back) {
				for(int d = std::min<int>(3, (int)(n / 8)); d > 0; --d)
					Quad(Back(s.across), { p.X + d * 1.2f * k.sx, p.Y - d * 1.2f * k.sy }, cw, chh, 0.f, col(k.a * 0.8f, { 255, 160, 150, 140 }), false);
				Quad(Back(s.across), { p.X, p.Y }, cw, chh, 0.f, col(k.a, { 255, 255, 255, 255 }), false);
			} else if(top.code) {
				CardAt(k, top, p.X, p.Y, cw, chh, s.across, true);
			}
			Count(k, p.X, p.Y + chh * 0.5f - 6.f * k.sy, n, pile);
			if(loc)
				Hit(k, TableHit::PILE, Box(p.X, p.Y, cw, chh), idx, loc, 0, back ? 0 : top.code);
		}
	};
	pileAt(6, 0, s.banishTop, s.banished, LOCATION_REMOVED, L"BAN", false);
	pileAt(6, 1, s.graveTop, s.grave, LOCATION_GRAVE, L"GY", false);
	pileAt(6, 2, TableCard{}, s.deck, 0, L"DECK", true);
	pileAt(0, 2, TableCard{}, s.extra, LOCATION_EXTRA, L"EXTRA", true);
	if(s.extraUp) {
		const auto p = cellC(0, 2);
		const std::wstring t = epro::format(L"+{} up", s.extraUp);
		const auto d = TextSize(k.tagF, t);
		Text(k.tagF, t, p.X - d.Width / 2.f, p.Y - chh * 0.5f + 2.f * k.sy, col(k.a, { 255, 200, 230, 255 }), k.a * 0.9f, 1);
	}
	// hand
	{
		const size_t n = s.hand.size();
		const float hch = handH * 0.94f, hcw = hch / 1.46f;
		const std::wstring lbl = epro::format(L"HAND {}", n);
		const auto ld = TextSize(k.tagF, lbl);
		const float lblX = x0 + pad + 2.f * k.sx, midY = handY0 + handH / 2.f;
		Text(k.tagF, lbl, lblX, midY - ld.Height / 2.f, col(k.a * 0.75f, { 255, 220, 206, 170 }), 0.f, 0);
		if(n) {
			const float span = bw - 2.f * pad - ld.Width - 16.f * k.sx;
			const float step = std::min(hcw + 3.f * k.sx, n > 1 ? (span - hcw) / (float)(n - 1) : hcw);
			const float total = hcw + step * (float)(n - 1);
			float hx = x0 + pad + ld.Width + 12.f * k.sx + (span - total) / 2.f + hcw / 2.f;
			for(size_t i = 0; i < n; ++i) {
				const auto& c = s.hand[i];
				auto* tex = c.code ? Art(c.code) : nullptr;
				Quad(tex ? tex : Back(s.across), { hx, midY }, hcw, hch, 0.f, col(k.a, { 255, 255, 255, 255 }), false);
				if(c.code) {
					// overlapping cards: each one's hit area is its visible slice
					const float vis = i + 1 < n ? std::min(step, hcw) : hcw;
					Hit(k, TableHit::CARD, core::recti((s32)(hx - hcw / 2.f), (s32)(midY - hch / 2.f), (s32)(hx - hcw / 2.f + vis), (s32)(midY + hch / 2.f)), idx, LOCATION_HAND, (uint32_t)i, c.code);
				}
				hx += step;
			}
		}
	}
	if(!s.alive) {
		drv->draw2DRectangle(col(k.a * 0.55f, { 255, 0, 0, 0 }), core::recti((s32)x0 + 1, (s32)zy0, (s32)x1 - 1, (s32)zy1));
		static const wchar_t* pl[]{ L"", L"1ST", L"2ND", L"3RD", L"4TH" };
		const std::wstring t = epro::format(L"ELIMINATED · {}", s.placement >= 0 && s.placement < 5 ? pl[s.placement] : L"");
		auto* f = FitFont(std::min(28.f * k.sy, (zy1 - zy0) * 0.2f));
		const auto d = TextSize(f, t);
		Text(f, t, (x0 + x1 - d.Width) / 2.f, (zy0 + zy1 - d.Height) / 2.f, col(k.a, { 255, 230, 120, 100 }), k.a, 2);
	}
}
} // namespace

void DrawTable(const TableView& in, std::vector<TableHit>& hits) {
	hits.clear();
	if(!drv || in.seats.empty()) return;
	LoadHud();
	const auto A = TableArea();
	const float W = SW(), H = SH();
	TableCtx k;
	k.sx = W / 1024.f; k.sy = H / 640.f; k.u = std::min(k.sx, k.sy);
	k.a = outCubic(in.open);
	k.pulse = 0.5f + 0.5f * std::sin(clockMs * 0.004f);
	k.nameF = F.name ? F.name : F.statline;
	k.tagF = F.tag ? F.tag : k.nameF;
	k.smallF = F.plain ? F.plain : k.nameF;
	k.hits = &hits;
	k.in = &in;
	const float ax0 = (float)A.UpperLeftCorner.X, ay0 = (float)A.UpperLeftCorner.Y, ax1 = (float)A.LowerRightCorner.X, ay1 = (float)A.LowerRightCorner.Y;
	// backdrop: the field dims away behind a dark table
	drv->draw2DRectangle(A, col(k.a * 0.90f, { 255, 9, 12, 17 }), col(k.a * 0.90f, { 255, 9, 12, 17 }), col(k.a * 0.94f, { 255, 4, 5, 8 }), col(k.a * 0.94f, { 255, 4, 5, 8 }));
	Frame(A, col(k.a * 0.5f, kAlly));
	const float cxm = (ax0 + ax1) / 2.f;
	// title strip: name + hint on the left, close on the right
	const float titleH = 18.f * k.sy, pad = 7.f * k.u;
	{
		const float ty = ay0 + 4.f * k.sy;
		const std::wstring title = L"BATTLE ROYAL · TABLE";
		const auto td = TextSize(k.tagF, title);
		Text(k.tagF, title, ax0 + pad + 2.f * k.sx, ty + (titleH - td.Height) / 2.f, col(k.a, { 255, 240, 214, 140 }), k.a * 0.8f, 1);
		const std::wstring hint = L"hover a card to read it · click a pile to list it · Tab or right-click to close";
		const auto hd = TextSize(k.smallF, hint);
		Text(k.smallF, hint, ax0 + pad + td.Width + 14.f * k.sx, ty + (titleH - hd.Height) / 2.f, col(k.a * 0.55f, { 255, 220, 210, 190 }), 0.f, 0);
		const std::wstring ct = L"CLOSE  ✕";
		const auto cd = TextSize(k.tagF, ct);
		const float bw = cd.Width + 18.f * k.sx, bx = ax1 - pad - bw;
		const core::recti r((s32)bx, (s32)ty, (s32)(bx + bw), (s32)(ty + titleH));
		SmallPill(r, kAlly, k.a);
		Text(k.tagF, ct, bx + (bw - cd.Width) / 2.f, ty + (titleH - cd.Height) / 2.f, col(k.a, { 255, 240, 220, 170 }), 0.f, 0);
		Hit(k, TableHit::CLOSE, r, -1);
	}
	// rows: across the table on top, your side at the bottom, a centre line between them
	std::vector<int> farRow, nearRow;
	for(int i = 0; i < (int)in.seats.size(); ++i)
		(in.seats[i].across ? farRow : nearRow).push_back(i);
	const float top = ay0 + 4.f * k.sy + titleH + pad, bottom = ay1 - pad;
	const float midH = 22.f * k.sy;
	const float rowH = (bottom - top - midH - pad * 2.f) / 2.f;
	const float farY0 = top, nearY0 = top + rowH + pad + midH + pad;
	const float midY = top + rowH + pad + midH / 2.f;
	std::vector<core::recti> boardR(in.seats.size());
	auto layRow = [&](const std::vector<int>& row, float ry0) {
		if(row.empty()) return;
		const float gap = 10.f * k.sx, rowW = ax1 - ax0 - 2.f * pad;
		const float maxW = rowH * 1.62f; // keep boards from stretching wide when a row has one seat
		const float bw = std::min(maxW, (rowW - gap * (row.size() - 1)) / (float)row.size());
		const float total = bw * row.size() + gap * (row.size() - 1);
		float bx = ax0 + pad + (rowW - total) / 2.f;
		for(int i : row) {
			boardR[i] = core::recti((s32)bx, (s32)ry0, (s32)(bx + bw), (s32)(ry0 + rowH));
			bx += bw + gap;
		}
	};
	layRow(farRow, farY0);
	layRow(nearRow, nearY0);
	// centre line
	{
		const video::SColor gl(255, 200, 160, 80);
		drv->draw2DRectangle(core::rect<s32>((s32)(ax0 + pad), (s32)midY, (s32)cxm, (s32)midY + 1), col(0.f, gl), col(k.a * 0.7f, gl), col(0.f, gl), col(k.a * 0.7f, gl));
		drv->draw2DRectangle(core::rect<s32>((s32)cxm, (s32)midY, (s32)(ax1 - pad), (s32)midY + 1), col(k.a * 0.7f, gl), col(0.f, gl), col(k.a * 0.7f, gl), col(0.f, gl));
		if(!in.caption.empty()) {
			auto* f = k.nameF;
			const auto d = TextSize(f, in.caption);
			const float pw = d.Width + 30.f * k.sx, ph = std::min(midH, d.Height + 8.f * k.sy);
			SmallPill(core::recti((s32)(cxm - pw / 2.f), (s32)(midY - ph / 2.f), (s32)(cxm + pw / 2.f), (s32)(midY + ph / 2.f)), kAlly, k.a);
			Text(f, in.caption, cxm - d.Width / 2.f, midY - d.Height / 2.f, col(k.a, { 255, 250, 236, 200 }), k.a * 0.8f, 1);
		}
		if(in.teams) {
			const std::wstring l = L"YOUR TEAM", r = L"OPPONENTS";
			const auto ld = TextSize(k.tagF, l), rd = TextSize(k.tagF, r);
			Text(k.tagF, r, ax0 + pad + 4.f * k.sx, midY - rd.Height - 3.f * k.sy, col(k.a * 0.8f, kEnemy), 0.f, 0);
			Text(k.tagF, l, ax0 + pad + 4.f * k.sx, midY + 4.f * k.sy, col(k.a * 0.8f, kAlly), 0.f, 0);
			(void)ld;
		}
	}
	for(size_t i = 0; i < in.seats.size(); ++i)
		DrawBoard(k, in.seats[i], (int)i, boardR[i]);
	// the attack: a beam from the turn player's board to its target, with sparks running along it
	int from = -1, to = -1;
	for(int i = 0; i < (int)in.seats.size(); ++i) {
		if(in.seats[i].alive && in.seats[i].turn) from = i;
		if(in.seats[i].alive && in.seats[i].target) to = i;
	}
	if(from >= 0 && to >= 0 && from != to) {
		auto edgePt = [&](int i, int other) {
			const auto& r = boardR[i];
			const auto& o = boardR[other];
			const float cx = (r.UpperLeftCorner.X + r.LowerRightCorner.X) / 2.f;
			if(in.seats[i].across != in.seats[other].across)
				return core::vector2df(cx, in.seats[i].across ? (float)r.LowerRightCorner.Y : (float)r.UpperLeftCorner.Y);
			const bool left = o.UpperLeftCorner.X > r.UpperLeftCorner.X;
			return core::vector2df(left ? (float)r.LowerRightCorner.X : (float)r.UpperLeftCorner.X, (r.UpperLeftCorner.Y + r.LowerRightCorner.Y) / 2.f);
		};
		const auto a = edgePt(from, to), b = edgePt(to, from);
		Line(a, b, 7.f * k.u, addc(k.a * (0.45f + 0.2f * k.pulse), { 255, 255, 80, 60 }));
		Line(a, b, 2.5f * k.u, addc(k.a * 0.9f, { 255, 255, 220, 200 }));
		for(int j = 0; j < 4; ++j) {
			float t = std::fmod(clockMs * 0.0007f + j * 0.25f, 1.f);
			const auto p = a + (b - a) * t;
			Quad(tGlow, p, 14.f * k.u, 14.f * k.u, 0.f, addc(k.a * std::sin(t * PI), { 255, 255, 190, 150 }), true);
		}
		Quad(tRing, b, 22.f * k.u * (1.f + 0.15f * k.pulse), 22.f * k.u * (1.f + 0.15f * k.pulse), 0.f, addc(k.a * 0.9f, { 255, 255, 100, 80 }), true);
	}
}

void Pill(const core::recti& r) {
	if(!Enabled()) return;
	const s32 x0 = r.UpperLeftCorner.X, y0 = r.UpperLeftCorner.Y, x1 = r.LowerRightCorner.X, y1 = r.LowerRightCorner.Y;
	if(x1 - x0 < 2 || y1 - y0 < 2) return;
	const video::SColor top(230, 30, 34, 44), bot(230, 12, 14, 19);
	drv->draw2DRectangle(core::rect<s32>(x0 + 1, y0 + 1, x1 - 1, y1 - 1), top, top, bot, bot);
	const video::SColor gold(210, 201, 163, 82), goldDim(150, 140, 110, 50);
	drv->draw2DLine({ x0 + 2, y0 }, { x1 - 2, y0 }, gold);
	drv->draw2DLine({ x0 + 2, y1 - 1 }, { x1 - 2, y1 - 1 }, goldDim);
	drv->draw2DLine({ x0, y0 + 2 }, { x0, y1 - 2 }, goldDim);
	drv->draw2DLine({ x1 - 1, y0 + 2 }, { x1 - 1, y1 - 2 }, goldDim);
}

// MDFX_DEMO_CHAIN=<code>,<code>,...: once the duel has been idle a while, play CHAIN for 2 and 3 links, then
// RESOLVE 3 -> 1, through the same overlay calls the duel thread uses (presentation test only).
void DevDemo(bool inDuel) {
	static const char* env = std::getenv("MDFX_DEMO_CHAIN");
	if(!env || !*env || !inDuel || !drv) return;
	static std::vector<uint32_t> codes;
	static int step = 0;
	static float idleSince = -1.f;
	if(codes.empty()) {
		std::string cur;
		for(const char* c = env;; ++c) {
			if(*c == ',' || *c == 0) { if(!cur.empty()) codes.push_back((uint32_t)std::strtoul(cur.c_str(), nullptr, 10)); cur.clear(); if(!*c) break; }
			else cur += *c;
		}
		if(codes.size() < 3) { codes.clear(); return; }
	}
	if(step > 5) return;
	if(ov.kind != Overlay::NONE) { idleSince = -1.f; return; }
	if(idleSince < 0.f) { idleSince = clockMs; return; }
	if(clockMs - idleSince < (step == 0 ? 6000.f : 250.f)) return;
	idleSince = -1.f;
	const std::vector<uint32_t> two(codes.begin(), codes.begin() + 2), three(codes.begin(), codes.begin() + 3);
	switch(step++) {
	case 0: ChainDisplay(two); break;
	case 1: ChainDisplay(three); break;
	case 2: ResolveDisplay(three, 3, true); break;
	case 3: ResolveDisplay(three, 2, false); break;
	case 4: ResolveDisplay(three, 1, false); break;
	default: break;
	}
}

// big LP change number from title-art digits: d0..d9 + dminus (damage, red), n0..n9 + nplus (gain, gold).
// progress comes from EDOPRO's own LP roll (framesLeft counts LpRollFrames() -> 0). false = draw the stock text.
// called from DrawMisc (under the cards' stat pills): only checks the art and queues the number; mdfx::Draw
// renders it last so it sits above everything
bool LpNumber(const std::wstring& text, int player, int framesLeft) {
	if(!Enabled() || text.empty() || !Ui("d0") || !Ui("n0")) return false;
	pendingNum = { text, player, framesLeft, true };
	return true;
}
namespace {
bool DrawLpNumber(const std::wstring& text, int player, int framesLeft) {
	if(!Enabled() || text.empty()) return false;
	const bool gain = text[0] == L'+';
	std::vector<video::ITexture*> glyphs;
	for(wchar_t ch : text) {
		std::string key;
		if(ch == L'-') key = "dminus";
		else if(ch == L'+') key = "nplus";
		else if(ch >= L'0' && ch <= L'9') key = std::string(gain ? "n" : "d") + (char)('0' + (ch - L'0'));
		else continue;
		auto* g = Ui(key);
		if(!g) return false;
		glyphs.push_back(g);
	}
	if(glyphs.empty()) return false;
	const float k = 1.f - clamp01((float)framesLeft / (float)std::max(1, LpRollFrames()));
	const float W = SW(), H = SH();
	const float h = 0.17f * H;
	const float punch = 1.f + 0.9f * (1.f - outCubic(k / 0.14f));
	// glyph box: digits keep their aspect at height h; signs are capped to a dash/cross width
	auto box = [&](video::ITexture* g) {
		const auto sz = g->getOriginalSize();
		float w = h * 0.78f * sz.Width / (float)sz.Height, hh = h;
		if(w > h * 0.62f) { w = h * 0.55f; hh = w * sz.Height / (float)sz.Width; }
		return core::vector2df(w, hh);
	};
	const float a = clamp01(k / 0.05f) * (1.f - smooth(0.78f, 1.f, k));
	const float lift = smooth(0.7f, 1.f, k) * 0.06f * H;
	const float cy = (player == 0 ? 0.66f : 0.30f) * H - lift;
	const float cx = 0.6f * W;
	float total = 0.f;
	for(auto* g : glyphs) total += box(g).X;
	const float shake = (1.f - clamp01(k / 0.25f)) * 10.f * S();
	float x = cx - total * punch / 2.f + frand(-shake, shake);
	Quad(tGlow, { cx, cy }, total * 1.8f, h * 1.6f, 0.f, addc(a * 0.5f, gain ? video::SColor(255, 255, 210, 90) : video::SColor(255, 255, 40, 20)), true);
	for(auto* g : glyphs) {
		const auto b = box(g) * punch;
		Quad(g, { x + b.X / 2.f, cy }, b.X, b.Y, 0.f, col(a, { 255, 255, 255, 255 }), false);
		x += b.X;
	}
	if(k < 0.12f) Quad(tGlow, { cx, cy }, total * 2.4f, h * 2.f, 0.f, addc((1.f - k / 0.12f) * 0.8f, { 255, 255, 255, 255 }), true);
	return true;
}
} // namespace

// --- test aids ------------------------------------------------------------------------------------
const char* AutoPuzzle() {
	static const char* p = std::getenv("MDFX_AUTOPUZZLE");
	return (p && *p) ? p : nullptr;
}
void DevCapture(bool inDuel) {
	static const char* dir = std::getenv("MDFX_CAPTURE");
	if(!dir || !*dir || !inDuel || !drv) return;
	static const char* everyEnv = std::getenv("MDFX_CAPTURE_MS");
	static const float every = everyEnv ? (float)std::atoi(everyEnv) : 100.f;
	static float last = -1e9f;
	static int n = 0;
	if(clockMs - last < every && clockMs >= last) return;
	last = clockMs;
	auto* img = drv->createScreenShot();
	if(!img) return;
	const auto file = epro::format(EPRO_TEXT("{}/{:05d}.png"), Utils::ToPathString(std::string(dir)), n++);
	drv->writeImageToFile(img, { file.data(), static_cast<u32>(file.size()) });
	img->drop();
}

}
