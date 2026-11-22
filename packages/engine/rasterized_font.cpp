// SPDX-License-Identifier: Apache-2.0
#include "graphics/font.h"

namespace {
const gea::framework::graphics::RuntimeFontProvider *runtimeFontProvider = nullptr;
}

#ifndef GEA_EMBEDDED_TTF_RUNTIME_FONTS
#define GEA_EMBEDDED_TTF_RUNTIME_FONTS 0
#endif

#ifndef GEA_EMBEDDED_TTF_FREETYPE
#define GEA_EMBEDDED_TTF_FREETYPE 0
#endif
#if GEA_EMBEDDED_TTF_FREETYPE && !GEA_EMBEDDED_TTF_RUNTIME_FONTS
#error "FreeType requires runtime TTF fonts"
#endif
#if GEA_EMBEDDED_TTF_RUNTIME_FONTS
#include "memory.h"
#if GEA_EMBEDDED_TTF_FREETYPE
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H
#endif

#include <algorithm>
#include <cmath>
#include <climits>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <new>

#ifndef GEA_EMBEDDED_TTF_RUNTIME_TRACE
#define GEA_EMBEDDED_TTF_RUNTIME_TRACE 0
#endif

namespace {

void *gea_runtime_stbtt_malloc(std::size_t size, void *)
{
	if (size == 0) size = 1;
	return gea::framework::memory::Allocator::allocatePreferSpiram(size, alignof(std::max_align_t));
}

void gea_runtime_stbtt_free(void *ptr, void *)
{
	gea::framework::memory::Allocator::free(ptr);
}

}  // namespace

#define STBTT_STATIC
#define STBTT_malloc(x, u) gea_runtime_stbtt_malloc((x), (u))
#define STBTT_free(x, u) gea_runtime_stbtt_free((x), (u))
#define STBTT_RASTERIZER_VERSION 1
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#endif

namespace gea::framework::graphics {

RasterizedFont::RasterizedFont(const RasterizedFontData *data)
	: data_(data)
{
}

class RasterizedGlyphLookup {
public:
	static const Glyph *find(const RasterizedFontData *font, int codepoint)
	{
		if (font->glyphCount > 0) {
			const int first = font->glyphs[0].codepoint;
			const int index = codepoint - first;
			if (index >= 0 && index < font->glyphCount && font->glyphs[index].codepoint == codepoint) {
				return &font->glyphs[index];
			}
		}
		for (int i = 0; i < font->glyphCount; i++) {
			if (font->glyphs[i].codepoint == codepoint) return &font->glyphs[i];
		}
		if (font->strictGlyphLookup)
		{
			for (int i = 0; i < font->glyphCount; i++)
			{
				if (font->glyphs[i].codepoint == font->fallbackCodepoint)
					return &font->glyphs[i];
			}
			return nullptr;
		}
		if (font->glyphCount > 0 && codepoint != '?') {
			for (int i = 0; i < font->glyphCount; i++) {
				if (font->glyphs[i].codepoint == '?') return &font->glyphs[i];
			}
			return &font->glyphs[0];
		}
		return nullptr;
	}
};

namespace {

void ensureGeneratedFontsLinked()
{
#ifdef GEA_EMBEDDED_HAS_GENERATED_FONTS
	generated::ensureLinked();
#endif
}

#if GEA_EMBEDDED_TTF_RUNTIME_FONTS

#if GEA_EMBEDDED_TTF_RUNTIME_TRACE
void runtimeTtfTrace(const char *fmt, ...)
{
	std::printf("GEA_TTF ");
	va_list args;
	va_start(args, fmt);
	std::vprintf(fmt, args);
	va_end(args);
	std::printf("\n");
	std::fflush(stdout);
}
#else
void runtimeTtfTrace(const char *, ...) {}
#endif

constexpr int kRuntimeAsciiFirst = 0x20;
constexpr int kRuntimeAsciiLast = 0x7e;
constexpr int kRuntimeAsciiCount = kRuntimeAsciiLast - kRuntimeAsciiFirst + 1;
#if GEA_EMBEDDED_TTF_FREETYPE
constexpr int kRuntimeExtraCodepoints[] = {0x00b0,0x00b5,0x00d7,0x2013,0x2014,0x2018,0x2019,0x201c,0x201d,0x2022,0x2026,0x2190,0x2191,0x2192,0x2193,0x2212};
constexpr int kRuntimeExtraGlyphs = sizeof(kRuntimeExtraCodepoints)/sizeof(int);
#else
// The degree sign, and the bullet a password input draws for each hidden
// character (lv_textarea's LV_TEXTAREA_PWD_BULLET_UNICODE): without it every
// masked character fell back to nothing. Kept short: each entry costs a Glyph
// in every one of the 64 cache slots, on boards with no PSRAM too.
constexpr int kRuntimeExtraCodepoints[] = {0x00b0, 0x2022};
constexpr int kRuntimeExtraGlyphs = sizeof(kRuntimeExtraCodepoints) / sizeof(int);
#endif
constexpr int kRuntimeGlyphCapacity = kRuntimeAsciiCount + kRuntimeExtraGlyphs;
constexpr int kRuntimeFontCacheSlots = 64;
#if GEA_EMBEDDED_TTF_FREETYPE
constexpr int kRuntimeMaxAtlasWidth = 4096;
constexpr int kRuntimeMaxFontPx = 2048;
#else
constexpr int kRuntimeMaxAtlasWidth = 256;
constexpr int kRuntimeMaxFontPx = 192;
#endif
constexpr int kRuntimeAtlasPadding = 1;

struct RuntimeRasterizedFontSlot {
	bool built = false;
	bool ready = false;
	int familyId = -1;
	int requestedSizePx = 0;
	#if GEA_EMBEDDED_TTF_FREETYPE
 int weight = 400;
 int stretch = 1000;
#else
 static constexpr int weight=400,stretch=1000;
#endif
#if GEA_EMBEDDED_TTF_FREETYPE
	FT_Face face = nullptr;
#endif
	unsigned long usedAt = 0;
	const std::uint8_t *fontBytes = nullptr;
	unsigned long fontLength = 0;
	stbtt_fontinfo fontInfo{};
	RasterizedFontData data{};
	Glyph glyphs[kRuntimeGlyphCapacity]{};
	std::uint8_t metricReady[kRuntimeGlyphCapacity]{};
	std::uint8_t rasterized[kRuntimeGlyphCapacity]{};
	std::uint8_t *atlas = nullptr;
	int atlasCapacityHeight = 0;
	int atlasCursorX = 0;
	int atlasCursorY = 0;
	int atlasRowHeight = 0;
};

int runtimeCodepointAt(int index)
{
	if (index >= 0 && index < kRuntimeAsciiCount) return kRuntimeAsciiFirst + index;
	if (index >= kRuntimeAsciiCount && index < kRuntimeGlyphCapacity) return kRuntimeExtraCodepoints[index - kRuntimeAsciiCount];
	return -1;
}

int roundToInt(float value)
{
	return value >= 0.0f ? static_cast<int>(value + 0.5f) : static_cast<int>(value - 0.5f);
}

int ceilToInt(float value)
{
	return static_cast<int>(std::ceil(value));
}

RuntimeRasterizedFontSlot *runtimeFontSlots(bool allocate = true)
{
	static RuntimeRasterizedFontSlot *slots = nullptr;
	if (!slots && allocate) {
		void *storage = memory::Allocator::allocatePreferSpiram(
		    sizeof(RuntimeRasterizedFontSlot) * kRuntimeFontCacheSlots,
		    alignof(RuntimeRasterizedFontSlot));
		if (!storage) return nullptr;
		slots = static_cast<RuntimeRasterizedFontSlot *>(storage);
		for (int i = 0; i < kRuntimeFontCacheSlots; i++) new (&slots[i]) RuntimeRasterizedFontSlot();
	}
	return slots;
}

unsigned long nextRuntimeFontClock()
{
	static unsigned long clock = 1;
	return clock++;
}

void releaseRuntimeFontSlot(RuntimeRasterizedFontSlot &slot)
{
	if (slot.atlas) memory::Allocator::free(slot.atlas);
#if GEA_EMBEDDED_TTF_FREETYPE
	if(slot.face)FT_Done_Face(slot.face);
#endif
	slot = RuntimeRasterizedFontSlot{};
}

bool buildRuntimeFontSlot(RuntimeRasterizedFontSlot &slot, int familyId, int sizePx, int weight, int stretch)
{
	runtimeTtfTrace("slot.begin family=%d size=%d", familyId, sizePx);
	releaseRuntimeFontSlot(slot);
	slot.built = true;
	slot.familyId = familyId;
	slot.requestedSizePx = sizePx;
	#if GEA_EMBEDDED_TTF_FREETYPE
	slot.weight=weight;slot.stretch=stretch;
#else
 (void)weight;(void)stretch;
#endif

	unsigned long fontLength = 0;
	const std::uint8_t *fontBytes = runtimeFontProvider ? runtimeFontProvider->fontBytes(familyId, &fontLength) : nullptr;
	if (!fontBytes) fontBytes = generated::lookupRuntimeTtfFontForFamily(familyId, &fontLength);
	runtimeTtfTrace("slot.lookup family=%d bytes=%p len=%lu", familyId, static_cast<const void *>(fontBytes), fontLength);
	if (!fontBytes || fontLength == 0) return false;

#if GEA_EMBEDDED_TTF_FREETYPE
 static FT_Library library=[](){FT_Library value=nullptr;return FT_Init_FreeType(&value)==0?value:nullptr;}();
 if(!library || fontLength>static_cast<unsigned long>(LONG_MAX) || FT_New_Memory_Face(library,fontBytes,static_cast<FT_Long>(fontLength),0,&slot.face))return false;
 FT_MM_Var *axes=nullptr;
 bool hasWidth=false;
 if(!FT_Get_MM_Var(slot.face,&axes)) {
   FT_Fixed coords[32]{};
   if(axes->num_axis>32){FT_Done_MM_Var(library,axes);return false;}
   for(FT_UInt i=0;i<axes->num_axis;i++){
     const auto &axis=axes->axis[i];double value=axis.def/65536.0;
     if(axis.tag==FT_MAKE_TAG('w','g','h','t'))value=weight;
     if(axis.tag==FT_MAKE_TAG('w','d','t','h')){value=stretch/10.0;hasWidth=true;}
     coords[i]=static_cast<FT_Fixed>(std::clamp(value,axis.minimum/65536.0,axis.maximum/65536.0)*65536.0);
   }
   const auto error=FT_Set_Var_Design_Coordinates(slot.face,axes->num_axis,coords);
   FT_Done_MM_Var(library,axes);if(error)return false;
 }
 const int clampedSizePx=std::clamp(sizePx>0?sizePx:16,1,kRuntimeMaxFontPx);
 if(FT_Set_Pixel_Sizes(slot.face,0,clampedSizePx))return false;
 // CSS stretch also applies to faces with no width axis, without altering weight.
 FT_Matrix transform{hasWidth?65536:static_cast<FT_Fixed>(stretch*65536LL/1000),0,0,65536};
 FT_Set_Transform(slot.face,&transform,nullptr);
 slot.fontBytes=fontBytes;slot.fontLength=fontLength;
 const int ascender=static_cast<int>((slot.face->size->metrics.ascender+63)/64);
 const int descender=static_cast<int>((-slot.face->size->metrics.descender+63)/64);
 const int lineHeight=std::max(1,ascender+descender);
#else
	const int offset = stbtt_GetFontOffsetForIndex(fontBytes, 0);
	runtimeTtfTrace("slot.offset family=%d offset=%d", familyId, offset);
	if (offset < 0 || !stbtt_InitFont(&slot.fontInfo, fontBytes, offset)) return false;
	runtimeTtfTrace("slot.init family=%d ok=1", familyId);

	slot.fontBytes = fontBytes;
	slot.fontLength = fontLength;
	const int clampedSizePx = std::clamp(sizePx > 0 ? sizePx : 16, 1, kRuntimeMaxFontPx);
	const float scale = stbtt_ScaleForMappingEmToPixels(&slot.fontInfo, static_cast<float>(clampedSizePx));
	runtimeTtfTrace("slot.scale family=%d size=%d scale_x1000=%d", familyId, clampedSizePx, static_cast<int>(scale * 1000.0f));
	if (scale <= 0.0f) return false;

	int ascent = 0;
	int descent = 0;
	int lineGap = 0;
	stbtt_GetFontVMetrics(&slot.fontInfo, &ascent, &descent, &lineGap);
	const int ascender = ceilToInt(static_cast<float>(ascent) * scale);
	const int descender = ceilToInt(static_cast<float>(-descent) * scale);
	const int lineHeight = std::max(1, ascender + descender);

#endif

	int glyphCount = 0;
	for (int i = 0; i < kRuntimeGlyphCapacity; i++) {
		const int codepoint = runtimeCodepointAt(i);
		if (codepoint < 0) continue;
		Glyph &glyph = slot.glyphs[glyphCount++];
		glyph.codepoint = codepoint;
		glyph.sourceX = 0;
		glyph.sourceY = 0;
		glyph.width = 0;
		glyph.height = 0;
		glyph.advance = std::max(1, clampedSizePx / 2);
		glyph.bearingX = 0;
		glyph.bearingY = 0;
	}

	if (glyphCount <= 0) return false;

	slot.atlas = static_cast<std::uint8_t *>(memory::Allocator::allocatePreferSpiram(1, alignof(std::uint8_t)));
	if (!slot.atlas) return false;
	slot.atlas[0] = 0;

	#if GEA_EMBEDDED_TTF_FREETYPE
 static std::uint64_t nextGeneration=0;
 if(nextGeneration==UINT64_MAX)return false;
 slot.data.generation=++nextGeneration;
#endif
 slot.data.id = 0x70000000 + familyId * 1000 + clampedSizePx;
	slot.data.sizePx = clampedSizePx;
	slot.data.lineHeight = lineHeight;
	slot.data.ascender = ascender;
	slot.data.descender = descender;
	slot.data.glyphCount = glyphCount;
	slot.data.glyphs = slot.glyphs;
	slot.data.atlasWidth = 1;
	slot.data.atlasHeight = 1;
	slot.data.atlas = slot.atlas;
	slot.atlasCapacityHeight = 1;
	slot.ready = true;
	runtimeTtfTrace("slot.ready family=%d size=%d glyphs=%d line=%d asc=%d desc=%d",
	    familyId,
	    clampedSizePx,
	    glyphCount,
	    lineHeight,
	    ascender,
	    descender);
	return true;
}

RuntimeRasterizedFontSlot *runtimeSlotForData(const RasterizedFontData *data)
{
	if (!data) return nullptr;
	RuntimeRasterizedFontSlot *slots = runtimeFontSlots();
	if (!slots) return nullptr;
	for (int i = 0; i < kRuntimeFontCacheSlots; i++) {
		if (slots[i].ready && &slots[i].data == data) return &slots[i];
	}
	return nullptr;
}

RuntimeRasterizedFontSlot *runtimeSlotForGlyph(const RasterizedFontData *data, int codepoint, int *indexOut)
{
	RuntimeRasterizedFontSlot *slot = runtimeSlotForData(data);
	if (!slot) return nullptr;
	for (int i = 0; i < data->glyphCount; i++) {
		if (slot->glyphs[i].codepoint != codepoint) continue;
		if (indexOut) *indexOut = i;
		return slot;
	}
	return nullptr;
}

bool reserveRuntimeAtlasRows(RuntimeRasterizedFontSlot &slot, int requiredHeight)
{
	if (requiredHeight <= slot.atlasCapacityHeight && slot.data.atlasWidth == kRuntimeMaxAtlasWidth) return true;
	const int newHeight = std::max(requiredHeight, slot.atlasCapacityHeight + 64);
	runtimeTtfTrace("atlas.reserve family=%d size=%d old=%d new=%d",
	    slot.familyId,
	    slot.data.sizePx,
	    slot.atlasCapacityHeight,
	    newHeight);
	const std::size_t newBytes = static_cast<std::size_t>(kRuntimeMaxAtlasWidth) * static_cast<std::size_t>(newHeight);
	#if GEA_EMBEDDED_TTF_FREETYPE
 if(newBytes>16u*1024u*1024u)return false;
#endif
	std::uint8_t *next = static_cast<std::uint8_t *>(memory::Allocator::allocatePreferSpiram(newBytes, alignof(std::uint8_t)));
	if (!next) return false;
	std::memset(next, 0, newBytes);
	if (slot.atlas && slot.data.atlasWidth > 0 && slot.data.atlasHeight > 0) {
		const int copyWidth = std::min(slot.data.atlasWidth, kRuntimeMaxAtlasWidth);
		for (int row = 0; row < slot.data.atlasHeight && row < newHeight; row++) {
			std::memcpy(next + row * kRuntimeMaxAtlasWidth,
			    slot.atlas + row * slot.data.atlasWidth,
			    static_cast<std::size_t>(copyWidth));
		}
		memory::Allocator::free(slot.atlas);
	}
	slot.atlas = next;
	slot.atlasCapacityHeight = newHeight;
	slot.data.atlas = slot.atlas;
	slot.data.atlasWidth = kRuntimeMaxAtlasWidth;
	slot.data.atlasHeight = newHeight;
	return true;
}

void ensureRuntimeGlyphMetrics(const RasterizedFontData *data, int codepoint)
{
	int index = -1;
	RuntimeRasterizedFontSlot *slot = runtimeSlotForGlyph(data, codepoint, &index);
	if (!slot || index < 0 || slot->metricReady[index]) return;
	static int metricTraceCount = 0;
	if (metricTraceCount < 48) {
		runtimeTtfTrace("glyph.metrics.begin family=%d size=%d cp=%d", slot->familyId, slot->data.sizePx, codepoint);
		metricTraceCount++;
	}
	Glyph &glyph = slot->glyphs[index];
	slot->metricReady[index] = 1;
#if GEA_EMBEDDED_TTF_FREETYPE
 if(!slot->face || !FT_Get_Char_Index(slot->face,codepoint) || FT_Load_Char(slot->face,codepoint,FT_LOAD_RENDER|FT_LOAD_TARGET_NORMAL))return;
 const auto &bitmap=slot->face->glyph->bitmap;
 glyph.width=bitmap.width;glyph.height=bitmap.rows;
 glyph.advance=static_cast<int>((slot->face->glyph->advance.x+32)/64);
 glyph.advance16=static_cast<int>((slot->face->glyph->advance.x+2)/4);
 glyph.bearingX=slot->face->glyph->bitmap_left;glyph.bearingY=slot->face->glyph->bitmap_top;
 if(glyph.width>kRuntimeMaxAtlasWidth){glyph.width=0;glyph.height=0;return;}
#else
	if (codepoint != '?' && codepoint != ' ' && stbtt_FindGlyphIndex(&slot->fontInfo, codepoint) == 0) return;

	const float scale = stbtt_ScaleForMappingEmToPixels(&slot->fontInfo, static_cast<float>(slot->data.sizePx));
	if (scale <= 0.0f) return;

	int advanceWidth = 0;
	int leftSideBearing = 0;
	stbtt_GetCodepointHMetrics(&slot->fontInfo, codepoint, &advanceWidth, &leftSideBearing);

	int x0 = 0;
	int y0 = 0;
	int x1 = 0;
	int y1 = 0;
	stbtt_GetCodepointBitmapBox(&slot->fontInfo, codepoint, scale, scale, &x0, &y0, &x1, &y1);

	glyph.width = std::max(0, x1 - x0);
	glyph.height = std::max(0, y1 - y0);
	glyph.advance = std::max(1, roundToInt(static_cast<float>(advanceWidth) * scale));
	glyph.bearingX = x0;
	glyph.bearingY = -y0;
#endif
	if (glyph.width <= 0 || glyph.height <= 0) return;

	if (slot->atlasCursorX > 0 && slot->atlasCursorX + glyph.width + kRuntimeAtlasPadding > kRuntimeMaxAtlasWidth) {
		slot->atlasCursorX = 0;
		slot->atlasCursorY += slot->atlasRowHeight + kRuntimeAtlasPadding;
		slot->atlasRowHeight = 0;
	}
	const int requiredHeight = slot->atlasCursorY + glyph.height;
	if (!reserveRuntimeAtlasRows(*slot, requiredHeight)) {
		glyph.width = 0;
		glyph.height = 0;
		return;
	}
	glyph.sourceX = slot->atlasCursorX;
	glyph.sourceY = slot->atlasCursorY;
	slot->atlasRowHeight = std::max(slot->atlasRowHeight, glyph.height);
	slot->atlasCursorX += glyph.width + kRuntimeAtlasPadding;
	if (metricTraceCount <= 48) {
		runtimeTtfTrace("glyph.metrics.ready family=%d size=%d cp=%d adv=%d "
						"box=%dx%d bearing=%d,%d src=%d,%d",
		    slot->familyId,
		    slot->data.sizePx,
		    codepoint,
		    glyph.advance,
		    glyph.width,
		    glyph.height,
		    glyph.bearingX,
		    glyph.bearingY,
		    glyph.sourceX,
		    glyph.sourceY);
	}
}

void ensureRuntimeGlyphCoverage(const RasterizedFontData *data, const Glyph &glyph)
{
	int index = -1;
	RuntimeRasterizedFontSlot *slot = runtimeSlotForGlyph(data, glyph.codepoint, &index);
	if (!slot || !slot->atlas || data->atlasWidth <= 0 || index < 0) return;
	ensureRuntimeGlyphMetrics(data, glyph.codepoint);
	if (slot->rasterized[index]) return;
	const Glyph &slotGlyph = slot->glyphs[index];
	if (slotGlyph.width <= 0 || slotGlyph.height <= 0) return;
	if (slotGlyph.sourceY + slotGlyph.height > slot->data.atlasHeight) return;
	if (slotGlyph.sourceX + slotGlyph.width > slot->data.atlasWidth) return;
	slot->rasterized[index] = 1;
	static int rasterTraceCount = 0;
	if (rasterTraceCount < 32) {
		runtimeTtfTrace("glyph.raster family=%d size=%d cp=%d box=%dx%d",
		    slot->familyId,
		    slot->data.sizePx,
		    slotGlyph.codepoint,
		    slotGlyph.width,
		    slotGlyph.height);
		rasterTraceCount++;
	}
#if GEA_EMBEDDED_TTF_FREETYPE
 if(FT_Load_Char(slot->face,slotGlyph.codepoint,FT_LOAD_RENDER|FT_LOAD_TARGET_NORMAL))return;
 const auto &bitmap=slot->face->glyph->bitmap;
 if(bitmap.pixel_mode!=FT_PIXEL_MODE_GRAY || static_cast<int>(bitmap.width)!=slotGlyph.width || static_cast<int>(bitmap.rows)!=slotGlyph.height)return;
 for(int row=0;row<slotGlyph.height;row++) {
   const auto *source=bitmap.buffer+(bitmap.pitch>=0?row:slotGlyph.height-1-row)*std::abs(bitmap.pitch);
   std::memcpy(slot->atlas+(slotGlyph.sourceY+row)*data->atlasWidth+slotGlyph.sourceX,source,slotGlyph.width);
 }
#else
	const float scale = stbtt_ScaleForMappingEmToPixels(&slot->fontInfo, static_cast<float>(slot->data.sizePx));
	if (scale <= 0.0f) return;
	stbtt_MakeCodepointBitmap(&slot->fontInfo,
	    slot->atlas + slotGlyph.sourceY * data->atlasWidth + slotGlyph.sourceX,
	    slotGlyph.width,
	    slotGlyph.height,
	    data->atlasWidth,
	    scale,
	    scale,
	    slotGlyph.codepoint);
#endif
}

const RasterizedFontData *runtimeTtfFontForFamily(int familyId, int sizePx, int weight, int stretch)
{
	if (familyId < 0) return nullptr;
	#if GEA_EMBEDDED_TTF_FREETYPE
 weight=std::clamp(weight>0?weight:400,1,1000);stretch=std::clamp(stretch>0?stretch:1000,1,32767);
#else
 // Existing stb backend does not vary axes; preserve its prior two-tuple cache.
 weight=400;stretch=1000;
#endif
	const int requestedSizePx = std::clamp(sizePx > 0 ? sizePx : 16, 1, kRuntimeMaxFontPx);
	RuntimeRasterizedFontSlot *slots = runtimeFontSlots();
	if (!slots) return nullptr;
	RuntimeRasterizedFontSlot *empty = nullptr;
	RuntimeRasterizedFontSlot *lru = &slots[0];
	const unsigned long now = nextRuntimeFontClock();

	for (int i = 0; i < kRuntimeFontCacheSlots; i++) {
		RuntimeRasterizedFontSlot &slot = slots[i];
		if (slot.built && slot.familyId == familyId && slot.requestedSizePx == requestedSizePx && slot.weight==weight && slot.stretch==stretch) {
			slot.usedAt = now;
			return slot.ready ? &slot.data : nullptr;
		}
		if (!slot.built && !empty) empty = &slot;
		if (slot.usedAt < lru->usedAt) lru = &slot;
	}

	RuntimeRasterizedFontSlot *slot = empty ? empty : lru;
	const bool ready = buildRuntimeFontSlot(*slot, familyId, requestedSizePx, weight, stretch);
	slot->usedAt = now;
	return ready ? &slot->data : nullptr;
}

#endif

}  // namespace

bool RasterizedFont::valid() const
{
	return data_ && data_->atlas && data_->glyphCount > 0;
}

int RasterizedFont::sizePx() const
{
	return data_ ? data_->sizePx : 0;
}

int RasterizedFont::lineHeight() const
{
	return data_ ? data_->lineHeight : 0;
}

int RasterizedFont::ascender() const
{
	return data_ ? data_->ascender : 0;
}

bool RasterizedFont::glyph(int codepoint, Glyph *out) const
{
	if (!out || !data_) return false;
	const Glyph *glyph = RasterizedGlyphLookup::find(data_, codepoint);
	if (!glyph) return false;
#if GEA_EMBEDDED_TTF_RUNTIME_FONTS
	ensureRuntimeGlyphMetrics(data_, glyph->codepoint);
	glyph = RasterizedGlyphLookup::find(data_, glyph->codepoint);
	if (!glyph) return false;
#endif
	*out = *glyph;
	return true;
}

int RasterizedFont::advance(int codepoint, int nextCodepoint) const
{
	Glyph value{};
	if (!glyph(codepoint, &value))
		return data_ && data_->strictGlyphLookup ? 0 : sizePx() / 2;
	if (value.advance16 < 0)
		return value.advance;
	int adjustment = 0;
	int first = 0;
	int last = data_->kerningCount;
	while (first < last)
	{
		const int middle = first + (last - first) / 2;
		const FontKerningPair &pair = data_->kerning[middle];
		if (pair.left < codepoint ||
			(pair.left == codepoint && pair.right < nextCodepoint))
			first = middle + 1;
		else
			last = middle;
	}
	if (first < data_->kerningCount)
	{
		const FontKerningPair &pair = data_->kerning[first];
		if (pair.left == codepoint && pair.right == nextCodepoint)
			adjustment = pair.adjustment16;
	}
	return (value.advance16 + adjustment + 8) >> 4;
}

std::uint8_t RasterizedFont::coverage(const Glyph &glyph, int row, int col) const
{
	if (!data_ || !data_->atlas) return 0;
	if (row < 0 || row >= glyph.height || col < 0 || col >= glyph.width) return 0;
#if GEA_EMBEDDED_TTF_RUNTIME_FONTS
	ensureRuntimeGlyphCoverage(data_, glyph);
#endif
	const int x = glyph.sourceX + col;
	const int y = glyph.sourceY + row;
	// One predictable branch on a per-font constant — the 8-bit path below stays
	// a single indexed load, as it was before 2-bit atlases existed. A 2-bit
	// atlas packs four pixels per byte, rows byte-aligned, leftmost pixel in the
	// most significant bit pair; see packLevelsTo2Bit in
	// packages/core/scripts/generate-gea-embedded-fonts.mjs. Levels normally map
	// back to the 0..255 coverage domain as level * 85. PaperS3 uses a calibrated
	// compile-time LUT: its direct-drive panel pushed the nominal intermediate
	// framebuffer grays (10 and 5) toward white and black, so its edge coverages
	// are calibrated explicitly for the panel. The current pair lands at gray4
	// levels 7 and 5. Endpoints remain
	// exact, and the indexed load replaces rather than adds per-pixel arithmetic.
	if (data_->atlasBits == 2) {
		const int stride = (data_->atlasWidth + 3) >> 2;
		const int level = (data_->atlas[y * stride + (x >> 2)] >> ((3 - (x & 3)) * 2)) & 0x3;
#if defined(GEA_EMBEDDED_FONT_COVERAGE_PAPERS3) && \
	GEA_EMBEDDED_FONT_COVERAGE_PAPERS3
		static constexpr std::uint8_t kCoverage[4] = {0, 144, 176, 255};
		return kCoverage[level];
#else
		return static_cast<std::uint8_t>(level * 85);
#endif
	}
	return data_->atlas[y * data_->atlasWidth + x];
}

RasterizedFont FontRegistry::rasterized(int fontId)
{
	ensureGeneratedFontsLinked();
	return RasterizedFont(generated::lookupFont(fontId));
}

RasterizedFont FontRegistry::rasterizedFamily(int familyId, int sizePx)
{
 return rasterizedFamily(familyId,sizePx,400,1000);
}

RasterizedFont FontRegistry::rasterizedFamily(int familyId, int sizePx,int weight,int stretch)
{
	ensureGeneratedFontsLinked();
#if GEA_EMBEDDED_TTF_RUNTIME_FONTS
	if (const RasterizedFontData *data = runtimeTtfFontForFamily(familyId, sizePx,weight,stretch)) {
		return RasterizedFont(data);
	}
#endif
	return RasterizedFont(generated::lookupFontForFamily(familyId, sizePx));
}

void FontRegistry::invalidateRuntimeFonts()
{
#if GEA_EMBEDDED_TTF_RUNTIME_FONTS
	if (auto *slots = runtimeFontSlots(false)) {
		for (int i = 0; i < kRuntimeFontCacheSlots; ++i) releaseRuntimeFontSlot(slots[i]);
	}
#endif
}

void FontRegistry::setRuntimeFontProvider(const RuntimeFontProvider *provider)
{
	invalidateRuntimeFonts();
	runtimeFontProvider = provider;
}

const BitmapFont8x16 &FontRegistry::bitmap8x16()
{
	return BitmapFont8x16::instance();
}

int FontRegistry::familyId(const char *family)
{
	ensureGeneratedFontsLinked();
	if (runtimeFontProvider) {
		const int id = runtimeFontProvider->lookupFamily(family);
		if (id >= 0) return id;
	}
	return generated::lookupFontFamily(family);
}

const char *FontRegistry::familyName(int familyId)
{
	ensureGeneratedFontsLinked();
	if (runtimeFontProvider) {
		if (const char *name = runtimeFontProvider->familyName(familyId)) return name;
	}
	return generated::lookupFontFamilyName(familyId);
}

namespace generated {

__attribute__((weak)) const RasterizedFontData *lookupFont(int fontId)
{
	(void)fontId;
	return nullptr;
}

__attribute__((weak)) const RasterizedFontData *lookupFontForFamily(int familyId, int sizePx)
{
	(void)familyId;
	(void)sizePx;
	return nullptr;
}

__attribute__((weak)) int lookupFontFamily(const char *family)
{
	(void)family;
	return -1;
}

__attribute__((weak)) const char *lookupFontFamilyName(int familyId)
{
	(void)familyId;
	return nullptr;
}

__attribute__((weak)) const std::uint8_t *lookupRuntimeTtfFontForFamily(int familyId, unsigned long *length)
{
	(void)familyId;
	if (length) *length = 0;
	return nullptr;
}

}  // namespace generated

}  // namespace gea::framework::graphics
