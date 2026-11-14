// The span engine (m20-system's core/rtl/span_engine.v) for the renderer's
// opaque textured faces: included by s_hwr.c, inside its rasterizer (it
// uses PRIM, HVERT and the edge walk). The engine maps the texture with
// perspective every 16 pixels and lights it per pixel from a small
// lightmap; the CPU draws everything else (textures with transparent
// texels, flat faces, shadows, sprites, the 2D) in the same back-to-front
// order. Faces under water use a second palette, tinted by the water
// colour (one at a time in the engine: switching waits for it).
//
// A face: the planes of 1/z, s/z and t/z on the screen (exact from any
// three of its clipped vertices: the renderer interpolates them linearly),
// in the engine's fixed point, as Quake's driver does. s and t are texels
// from the face's lowest corner (soff, toff) in its 256 x 256 page. The
// light: each vertex's brightness as the engine's (256 - 128 f, f the
// shade tables' factor), fitted as a + b s + c t (least squares: exact for
// a triangle, and for the rectangles of rooms) and sampled on a grid every
// 2^lshift texels, which the engine interpolates bilinearly: the lightmap,
// in a ring. The rows come from the CPU's edge walk (ScanFace, RowSpan),
// so both cover the same pixels: one span command each.
//
// Sharing the frame: the engine writes SDRAM, the CPU through its cache.
// At the list's start the frame (cleared, or the background picture) goes
// to SDRAM and out of the cache. A face the CPU draws waits for the engine
// if it overlaps what the engine was given (a box of rows and 64-byte
// columns); the rows the CPU drew go to SDRAM before the engine gets its
// next face; at the end the engine is waited for.
//
// The engine leaves texels 224..255 unlit (Quake's fullbright colours):
// HWR_SpanLevel moves the level's texels off them, to the nearest colour of
// 1..223 (0 stays: transparent), for the CPU's drawing too.
//
// On the host the engine's C model (core/firmware/span_core.h) draws each
// span at once (HWR_SpanOn: M20_SPAN=1 in the harness).

#ifdef M20_BOARD
#ifdef M20_SPANOFF   // make SPANOFF=1: the CPU only
bool HWR_SpanOn = false;
#else
bool HWR_SpanOn = true;
#endif
#ifdef M20_ZENGINE   // make ZENGINE=1: a bitstream with the engine's depth test
bool HWR_ZEngine = true;
#else
bool HWR_ZEngine = false;
#endif
#else
bool HWR_SpanOn = false;
bool HWR_ZEngine = true;
#endif
HWR_SPAN_STATS HWR_SpanStats;
#define LIT_A (1u << 30)
#define LIT_B (2u << 30)

// The engine's palettes: the level's, and tinted by the water colour (the
// faces under water: s_hwr.c's m_ShadeWater). The engine holds one at a
// time (m_EngPal, -1: neither yet); a face that needs the other waits for
// the engine to finish, then loads it.
static uint32_t m_Pal888[256], m_Pal888W[256];
static const uint32_t *m_PalCur = m_Pal888;
static int32_t m_EngPal = -1;
static bool m_SpanWater;   // the next face is under water

#define LM_RING 32768
static uint8_t m_LmRing[LM_RING] __attribute__((aligned(64)));
static uint32_t m_LmPos;

// What the engine was given since it was last seen idle: rows y0..y1 and
// 64-byte columns x0..x1 (32 pixels), empty when x0 > x1
static int32_t m_EqX0 = 1, m_EqX1 = 0, m_EqY0, m_EqY1;
// The rows the CPU drew in since the engine last had a face
static int32_t m_CpuY0 = M20_H, m_CpuY1 = -1;

#ifdef M20_BOARD
#define SPAN_HW ((volatile uint32_t *)0x490000u)
#define SPAN_BUSY (1u << 31)
#define SDRAM_OFF(p) ((uint32_t)(uintptr_t)(p)-0x800000u)
#define CLOCK_CNT (*(volatile uint32_t *)0x44000Cu)
static bool m_HwQueued;
static int16_t m_ZDummy[1024] __attribute__((aligned(64)));   // z writes, stride 0

static void HwWait(void)
{
    m_EqX0 = 1;
    m_EqX1 = 0;
    if (!m_HwQueued) {
        return;
    }
    uint32_t t = CLOCK_CNT;
    while (SPAN_HW[20] & SPAN_BUSY) {
    }
    HWR_SpanStats.wait_clk += CLOCK_CNT - t;
    HWR_SpanStats.waits++;
    m_HwQueued = false;
}

static void CacheFlush(const void *p, uint32_t n)
{
    for (uintptr_t a = (uintptr_t)p & ~(uintptr_t)63; a < (uintptr_t)p + n;
         a += 64u) {
        asm volatile("cbo.flush (%0)" ::"r"(a) : "memory");
    }
}

static void CacheClean(const void *p, uint32_t n)
{
    for (uintptr_t a = (uintptr_t)p & ~(uintptr_t)63; a < (uintptr_t)p + n;
         a += 64u) {
        asm volatile("cbo.clean (%0)" ::"r"(a) : "memory");
    }
}
#define SPAN_W(r, v) (SPAN_HW[r] = (v))
#else
// The engine's command trace (the harness's M20_TRACE=frames): the words
// the board would write, as records of 32-bit words: {0, reg, value} a
// command, {1, reg - 16, value} a direct register, {2, offset, 16 words} a
// 64-byte line of SDRAM, {3, 0, 0} the CPU waiting for the engine. The
// SDRAM: frame 0, depth 0x80000, pages 0x100000 + 64 KB each, the lightmap
// ring 0x500000.
#include <stdio.h>
FILE *HWR_SpanTrace;
static int32_t m_TracePages;
#define TR_FB 0x000000u
#define TR_ZB 0x080000u
#define TR_TEX 0x100000u
#define TR_LM 0x500000u
static void SpanTraceRec(uint32_t a, uint32_t b, uint32_t c)
{
    if (HWR_SpanTrace) {
        uint32_t w[3] = { a, b, c };
        fwrite(w, 4, 3, HWR_SpanTrace);
    }
}
static void SpanTraceMem(uint32_t off, const void *p, uint32_t n)
{
    for (uint32_t i = 0; HWR_SpanTrace && i < n; i += 64) {
        uint32_t w[18] = { 2, off + i };
        memcpy(&w[2], (const uint8_t *)p + i, 64);
        fwrite(w, 4, 18, HWR_SpanTrace);
    }
}
#define SPAN_W(r, v)                                                           \
    SpanTraceRec((r) < 16 ? 0u : 1u, (r) < 16 ? (r) : (r)-16, (uint32_t)(v))
// A new trace starts with the pages
void HWR_SpanTraceStart(void)
{
    for (int32_t p = 0; p < m_TracePages; p++) {
        SpanTraceMem(TR_TEX + (uint32_t)p * 65536u, TexturePagePtrs[p], 65536);
    }
}
static void HwWait(void)
{
    m_EqX0 = 1;
    m_EqX1 = 0;
    SpanTraceRec(3, 0, 0);
}
static void CacheFlush(const void *p, uint32_t n)
{
}
static void CacheClean(const void *p, uint32_t n)
{
}
// The C model's pixels
static uint16_t *m_PixFb;
static uint16_t *m_PixZb;   // (NULL: no depth test)
static const uint8_t *m_PixTex;
static const uint8_t *m_PixLight;
// (with the depth test, as the engine's: a pixel at least as near as the
// depth there is drawn, and its depth written)
#define SC_LPIXEL(i, tex_off, scale, z16)                                      \
    do {                                                                       \
        uint16_t zq_ = (uint16_t)(z16);                                        \
        if (!m_PixZb || zq_ >= m_PixZb[i]) {                                   \
            uint8_t t_ = m_PixTex[tex_off];                                    \
            m_PixFb[i] = sc_shade(m_PalCur[t_], t_, scale);                    \
            if (m_PixZb) {                                                     \
                m_PixZb[i] = zq_;                                              \
            }                                                                  \
        }                                                                      \
    } while (0)
#define SC_PIXEL(i, tex_off, z16) ((void)0)
#define SC_LIGHTMAP(k) ((int32_t)m_PixLight[k])
#endif

// span_core.h, after its pixel macros (the lit spans' model only on the
// host; the board needs its registers' layout)
#ifdef M20_BOARD
#define SC_PIXEL(i, tex_off, z16) ((void)0)
#endif
#include "span_core.h"

static sc_regs_t m_Eng;

// ---------------------------------------------------------------------
// Fixed point (as quake/port/span_engine.c)
// ---------------------------------------------------------------------
// The exponent that fits the largest |o + x dx + y dy| over the screen into
// 30 bits (its extremes are at the corners)
static int32_t SpExponent(float o, float dx, float dy)
{
    // (the corners are o + {0, a} + {0, b}: the largest and smallest from
    // a's and b's signs)
    float ca = (float)(M20_W - 1) * dx, cb = (float)(M20_H - 1) * dy;
    float hi = o + (ca > 0.0f ? ca : 0.0f) + (cb > 0.0f ? cb : 0.0f);
    float lo = o + (ca < 0.0f ? ca : 0.0f) + (cb < 0.0f ? cb : 0.0f);
    float m = fabsf(hi), c = fabsf(lo);
    if (c > m) {
        m = c;
    }
    if (m < 1e-30f) {
        return 60;
    }
    union {
        float f;
        uint32_t u;
    } b = { m };
    int32_t e = 30 - ((int32_t)(b.u >> 23) - 126);
    return e > 60 ? 60 : e < 0 ? 0 : e;
}

// x * 2^e, rounded
static int32_t SpFix(float x, int32_t e)
{
    union {
        uint32_t u;
        float f;
    } scale = { (uint32_t)(e + 127) << 23 };
    return (int32_t)lrintf(x * scale.f);
}

// The value at the screen's corner, from the centre, in wrapping integers
static int32_t SpOrigin(float o, float dx, float dy, int32_t ix, int32_t iy, int32_t e)
{
    int32_t cx = M20_W / 2, cy = M20_H / 2;
    return (int32_t)((uint32_t)SpFix(o + (float)cx * dx + (float)cy * dy, e)
                     - (uint32_t)cx * (uint32_t)ix - (uint32_t)cy * (uint32_t)iy);
}

// ---------------------------------------------------------------------
// The level, the palette, the list
// ---------------------------------------------------------------------
// The level's texels off 224..255, to the nearest of 1..223, and its pages
// to SDRAM
void HWR_SpanLevel(int32_t pages)
{
    uint8_t remap[256];
    for (int32_t i = 0; i < 256; i++) {
        remap[i] = (uint8_t)i;
    }
    for (int32_t i = 224; i < 256; i++) {
        int32_t r = GamePalette[i].r, g = GamePalette[i].g, b = GamePalette[i].b;
        uint32_t best = 1, bestd = ~0u;
        for (int32_t j = 1; j < 224; j++) {
            int32_t dr = r - GamePalette[j].r, dg = g - GamePalette[j].g,
                    db = b - GamePalette[j].b;
            uint32_t d = (uint32_t)(dr * dr * 3 + dg * dg * 6 + db * db);
            if (d < bestd) {
                bestd = d;
                best = j;
            }
        }
        remap[i] = (uint8_t)best;
    }
    for (int32_t p = 0; p < pages; p++) {
        uint8_t *t = (uint8_t *)TexturePagePtrs[p];
        for (int32_t i = 0; i < 65536; i++) {
            t[i] = remap[t[i]];
        }
#ifdef M20_TILED
        // into 8 x 8 tiles (s_hwr.c, TEX_OFF): each band of 8 rows is the
        // same 2 KB either way
        static uint8_t band[2048];
        for (int32_t y0 = 0; y0 < 256; y0 += 8) {
            uint8_t *b = t + y0 * 256;
            memcpy(band, b, sizeof(band));
            for (int32_t y = 0; y < 8; y++) {
                for (int32_t x = 0; x < 256; x++) {
                    b[TEX_OFF(x, y)] = band[y * 256 + x];
                }
            }
        }
#endif
        CacheClean(t, 65536);
    }
#ifndef M20_BOARD
    m_TracePages = pages;
#endif
}

static void SpanPalette(void)
{
    for (int32_t i = 0; i < 256; i++) {
        uint32_t r = GamePalette[i].r * 4, g = GamePalette[i].g * 4,
                 b = GamePalette[i].b * 4;
        m_Pal888[i] = r << 16 | g << 8 | b;
        // (as m_ShadeWater: the channel times the water colour, truncated)
        uint32_t wr = (uint32_t)((float)r * m_WaterColor.r),
                 wg = (uint32_t)((float)g * m_WaterColor.g),
                 wb = (uint32_t)((float)b * m_WaterColor.b);
        wr = wr > 255 ? 255 : wr;
        wg = wg > 255 ? 255 : wg;
        wb = wb > 255 ? 255 : wb;
        m_Pal888W[i] = wr << 16 | wg << 8 | wb;
    }
    m_EngPal = -1;
}

// One of the palettes into the engine (idle: direct registers)
static void SpanLoadPalette(bool water)
{
    m_PalCur = water ? m_Pal888W : m_Pal888;
    SPAN_W(24, 0);
    for (int32_t k = 0; k < 256; k++) {
        SPAN_W(25, m_PalCur[k]);
    }
    m_EngPal = water;
    HWR_SpanStats.palettes++;
}

// The brightness (the shade tables' factor) of a light value, as the
// engine's lightmap sample: 256 - 128 f
static float SpanLightSample(int32_t g)
{
    float f = (8192.0f - (float)g) / 4096.0f * T1MConfig.brightness;
    if (f > 1.0f) {
        f = 1.0f;
    } else if (f < 0.0f) {
        f = 0.0f;
    }
    return 256.0f - 128.0f * f;
}

static void SpanBegin(void)
{
    static bool tables;
    if (!tables) {
        sc_tables();
        tables = true;
    }
    m_CpuY0 = M20_H;
    m_CpuY1 = -1;
    m_LmPos = 0;
    if (!HWR_SpanOn) {
        return;
    }
#ifdef M20_BOARD
    HwWait();
    uint32_t t_fl = HWR_CLK();
    CacheFlush(M20_Frame, M20_W * M20_H * 2);
    if (m_Stream) {
        CacheFlush(m_ZBuf, M20_W * M20_H * 2);
    }
    HWR_TADD(flush_us, t_fl);   // (cycles)
#else
    if (HWR_SpanTrace) {
        m_EngPal = -1;
    }
#endif
    if (m_EngPal < 0) {
        SpanLoadPalette(false);
    }
#ifdef M20_BOARD
    SPAN_W(16, SDRAM_OFF(M20_Frame));
    SPAN_W(17, M20_W);
    if (m_Stream) {   // the depth buffer, tested (direct register 6 at 0x68, bit 0)
        SPAN_W(18, SDRAM_OFF(m_ZBuf));
        SPAN_W(19, M20_W);
        SPAN_W(26, 1);
    } else {
        SPAN_W(18, SDRAM_OFF(m_ZDummy));
        SPAN_W(19, 0);
        SPAN_W(26, 0);
    }
#else
    SPAN_W(16, TR_FB);
    SPAN_W(17, M20_W);
    SPAN_W(18, TR_ZB);
    SPAN_W(19, m_Stream ? M20_W : 0);
    SPAN_W(26, m_Stream ? 1 : 0);
#endif
}

static void SpanEnd(void)
{
    HwWait();
}

// A primitive the CPU draws, within x0..x1, y0..y1: after the engine's, if
// they may share a cache line
static void SpanCpu(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    if (!HWR_SpanOn) {
        return;
    }
    x0 = x0 < 0 ? 0 : x0;
    x1 = x1 >= M20_W ? M20_W - 1 : x1;
    y0 = y0 < 0 ? 0 : y0;
    y1 = y1 >= M20_H ? M20_H - 1 : y1;
    if (x0 > x1 || y0 > y1) {
        return;
    }
    if (m_EqX0 <= m_EqX1 && (x0 >> 5) <= m_EqX1 && (x1 >> 5) >= m_EqX0
        && y0 <= m_EqY1 && y1 >= m_EqY0) {
        HwWait();
    }
    m_CpuY0 = y0 < m_CpuY0 ? y0 : m_CpuY0;
    m_CpuY1 = y1 > m_CpuY1 ? y1 : m_CpuY1;
}

// The box of a primitive the CPU draws
static void SpanCpuPrim(const PRIM *p)
{
    if (!HWR_SpanOn) {
        return;
    }
    const HVERT *v = &m_Verts[p->first];
    int32_t n = p->n, sh = (p->flags & PF_SUBPIX) ? 4 : 0;
    if (p->type == PRIM_SPRITE || p->type == PRIM_QUAD2D
        || p->type == PRIM_LINE2D) {
        n = 2;
    }
    int32_t x0 = v[0].x, x1 = x0, y0 = v[0].y, y1 = y0;
    for (int32_t i = 1; i < n; i++) {
        x0 = v[i].x < x0 ? v[i].x : x0;
        x1 = v[i].x > x1 ? v[i].x : x1;
        y0 = v[i].y < y0 ? v[i].y : y0;
        y1 = v[i].y > y1 ? v[i].y : y1;
    }
    SpanCpu((x0 >> sh) - 1, (y0 >> sh) - 1, (x1 >> sh) + 1, (y1 >> sh) + 1);
}

// ---------------------------------------------------------------------
// A face for the engine. Returns false if the CPU should draw it.
// ---------------------------------------------------------------------
// Its vertices: the screen (pixels, float), 1/z, the texel (s, t: texels,
// at texel centres), the light; and the screen position in whole pixels
// for the edge walk (px, py: as the CPU's, PixelVerts)
typedef struct SVERT {
    float x, y, iz, s, t;
    int32_t g, px, py;
} SVERT;

// The edge walk for the engine: each edge's x (16.16, with the pixel
// centre's half) at its top row and its step, as ScanEdge computes them, so
// that the engine's faces cover the pixels the CPU's would; the edges going
// down the screen (in the vertices' order) one side, those going up the
// other, walked together row by row: no tables
typedef struct XEDGE {
    int32_t y0, y1;   // rows y0 .. y1 - 1
    int32_t x, dx;
} XEDGE;

static int32_t SortEdges(XEDGE *e, int32_t n)   // by y0 (few: insertion)
{
    for (int32_t i = 1; i < n; i++) {
        XEDGE t = e[i];
        int32_t j = i;
        for (; j > 0 && e[j - 1].y0 > t.y0; j--) {
            e[j] = e[j - 1];
        }
        e[j] = t;
    }
    return n;
}

// floorf for the screen's range, inline (picolibc's floorf is a call)
static inline int32_t FloorI(float f)
{
    int32_t i = (int32_t)f;   // (toward 0)
    return i - (f < (float)i);
}

static bool SpanFaceV(const SVERT *v, int32_t n, uint16_t tex)
{
    if (n < 3 || n > MAX_CVERTS) {
        return false;
    }

    // Each vertex: 1/z scaled so that the near plane is just below 1 (the
    // engine's 1/z is 2^31 times it, and its depth the top half: s_hwr.c's
    // Z_SCALE x near / z)
    uint32_t ts = HWR_CLK();
    const float m_WScale = (float)phd_GetNearZ() * (Z_SCALE / 32768.0f);
    float W[MAX_CVERTS];
    float smin = v[0].s, tmin = v[0].t, smax = v[0].s, tmax = v[0].t;
    for (int32_t i = 0; i < n; i++) {
        W[i] = m_WScale * v[i].iz;
        smin = v[i].s < smin ? v[i].s : smin;
        smax = v[i].s > smax ? v[i].s : smax;
        tmin = v[i].t < tmin ? v[i].t : tmin;
        tmax = v[i].t > tmax ? v[i].t : tmax;
    }
    int32_t umin = (int32_t)smin, vmin = (int32_t)tmin;
    int32_t umax = (int32_t)smax, vmax = (int32_t)tmax;
    if (umin < 0 || vmin < 0 || umax > 255 || vmax > 255) {
        return false;
    }

    // The planes from three vertices: 0 and the neighbouring pair with the
    // largest area
    int32_t b = 1, c = 2;
    float det = 0.0f;
    for (int32_t i = 1; i + 1 < n; i++) {
        float d = (v[i].x - v[0].x) * (v[i + 1].y - v[0].y)
            - (v[i + 1].x - v[0].x) * (v[i].y - v[0].y);
        if (fabsf(d) > fabsf(det)) {
            det = d;
            b = i;
            c = i + 1;
        }
    }
    if (fabsf(det) < 1.0f) {
        return false;
    }
    float id = 1.0f / det;
    float x1 = v[b].x - v[0].x, y1 = v[b].y - v[0].y, x2 = v[c].x - v[0].x,
          y2 = v[c].y - v[0].y;
#define PLANE(q0, q1, q2, o, dx, dy)                                           \
    do {                                                                       \
        float d1_ = (q1) - (q0), d2_ = (q2) - (q0);                            \
        dx = (d1_ * y2 - d2_ * y1) * id;                                       \
        dy = (x1 * d2_ - x2 * d1_) * id;                                       \
        o = (q0)-v[0].x * dx - v[0].y * dy;                                    \
    } while (0)
    float zo, zdx, zdy, so, sdx, sdy, to, tdx, tdy;
    PLANE(W[0], W[b], W[c], zo, zdx, zdy);
    PLANE((v[0].s - umin) * W[0], (v[b].s - umin) * W[b],
          (v[c].s - umin) * W[c], so, sdx, sdy);
    PLANE((v[0].t - vmin) * W[0], (v[b].t - vmin) * W[b],
          (v[c].t - vmin) * W[c], to, tdx, tdy);
#undef PLANE
    // (pixel x, y's centre is at x, y on the screen: the CPU rounds the
    // vertices to pixels and samples there, and the engine evaluates the
    // planes at the pixel's x, y)

    HWR_TADD(sec[0], ts);
    ts = HWR_CLK();
    m_Eng.es = SpExponent(so, sdx, sdy);
    m_Eng.et = SpExponent(to, tdx, tdy);
    m_Eng.su = SpFix(sdx, m_Eng.es);
    m_Eng.sv = SpFix(sdy, m_Eng.es);
    m_Eng.s0 = SpOrigin(so, sdx, sdy, m_Eng.su, m_Eng.sv, m_Eng.es);
    m_Eng.tu = SpFix(tdx, m_Eng.et);
    m_Eng.tv = SpFix(tdy, m_Eng.et);
    m_Eng.t0 = SpOrigin(to, tdx, tdy, m_Eng.tu, m_Eng.tv, m_Eng.et);
    m_Eng.zu = SpFix(zdx, 31);
    m_Eng.zv = SpFix(zdy, 31);
    m_Eng.z0 = SpOrigin(zo, zdx, zdy, m_Eng.zu, m_Eng.zv, 31);
    m_Eng.sadj = 0;
    m_Eng.tadj = 0;
    m_Eng.smax = ((umax - umin) << 16) | 0xFFFF;
    m_Eng.tmax = ((vmax - vmin) << 16) | 0xFFFF;
    m_Eng.tw_log2 = 8;
    m_Eng.th_log2 = 8;
    m_Eng.soff = umin;
    m_Eng.toff = vmin;
    m_Eng.tex_w = 256;
#ifdef M20_TILED
    m_Eng.tiled = 1;
#endif

    HWR_TADD(sec[1], ts);
    ts = HWR_CLK();
    // The light: a + b s + c t, least squares over the vertices
    float la = 0, lb = 0, lc = 0;
    {
        float S00 = n, S0s = 0, S0t = 0, Sss = 0, Sst = 0, Stt = 0, R0 = 0,
              Rs = 0, Rt = 0;
        for (int32_t i = 0; i < n; i++) {
            float s = v[i].s - umin, t = v[i].t - vmin,
                  l = SpanLightSample(v[i].g);
            S0s += s;
            S0t += t;
            Sss += s * s;
            Sst += s * t;
            Stt += t * t;
            R0 += l;
            Rs += l * s;
            Rt += l * t;
        }
        float m00 = Sss * Stt - Sst * Sst, m01 = S0t * Sst - S0s * Stt,
              m02 = S0s * Sst - S0t * Sss;
        float dd = S00 * m00 + S0s * m01 + S0t * m02;
        if (fabsf(dd) > 1e-3f) {
            float m11 = S00 * Stt - S0t * S0t, m12 = S0t * S0s - S00 * Sst,
                  m22 = S00 * Sss - S0s * S0s;
            float idd = 1.0f / dd;
            la = (m00 * R0 + m01 * Rs + m02 * Rt) * idd;
            lb = (m01 * R0 + m11 * Rs + m12 * Rt) * idd;
            lc = (m02 * R0 + m12 * Rs + m22 * Rt) * idd;
        } else {
            la = R0 / n;   // (texels in a line: one light)
        }
    }
    HWR_TADD(sec[2], ts);
    int32_t ext = umax - umin > vmax - vmin ? umax - umin : vmax - vmin, k = 0;
    while ((ext >> k) > 6) {
        k++;   // up to 8 x 8 samples
    }
    int32_t lw = ((umax - umin) >> k) + 2, lh = ((vmax - vmin) >> k) + 2;
    uint32_t bytes = (uint32_t)(lw * lh + 63) & ~63u;
    if (m_LmPos + bytes > LM_RING) {   // full: all loaded once the engine is idle
        HwWait();
        m_LmPos = 0;
    }
    uint32_t tlm = HWR_CLK();
    uint8_t *lm = m_LmRing + m_LmPos;
    m_LmPos += bytes;
    {   // (in 16.16, stepped)
        int32_t q0 = (int32_t)(la * 65536.0f) + 32768;
        int32_t qs = (int32_t)(lb * (float)(65536 << k));
        int32_t qt = (int32_t)(lc * (float)(65536 << k));
        uint8_t *row = lm;
        // (a plane: its extremes at the corners; clamped only if they're out)
        int32_t c1 = q0 + qs * (lw - 1), c2 = q0 + qt * (lh - 1);
        int32_t c3 = c1 + qt * (lh - 1);
        int32_t qmin = q0 < c1 ? q0 : c1, qmax = q0 < c1 ? c1 : q0;
        qmin = c2 < qmin ? c2 : qmin;
        qmax = c2 > qmax ? c2 : qmax;
        qmin = c3 < qmin ? c3 : qmin;
        qmax = c3 > qmax ? c3 : qmax;
        if (qmin >= 0 && qmax < (256 << 16)) {
            for (int32_t j = 0; j < lh; j++, q0 += qt, row += lw) {
                int32_t q = q0;
                for (int32_t i = 0; i < lw; i++, q += qs) {
                    row[i] = (uint8_t)(q >> 16);
                }
            }
        } else {
            for (int32_t j = 0; j < lh; j++, q0 += qt, row += lw) {
                int32_t q = q0;
                for (int32_t i = 0; i < lw; i++, q += qs) {
                    int32_t s = q >> 16;
                    row[i] = (uint8_t)(s < 0 ? 0 : s > 255 ? 255 : s);
                }
            }
        }
    }
    m_Eng.lshift = k;
    m_Eng.lw = lw;

    const uint8_t *tile = (const uint8_t *)TexturePagePtrs[tex];
#ifdef M20_BOARD
    if (m_CpuY1 >= m_CpuY0) {   // the CPU's rows to SDRAM first
        CacheFlush(M20_Frame + m_CpuY0 * M20_W,
                   (uint32_t)(m_CpuY1 - m_CpuY0 + 1) * M20_W * 2);
        m_CpuY0 = M20_H;
        m_CpuY1 = -1;
    }
    CacheClean(lm, (uint32_t)(lw * lh));
    uint32_t lm_off = SDRAM_OFF(lm), tile_off = SDRAM_OFF(tile);
#else
    m_PixTex = tile;
    m_PixLight = lm;
    m_PixZb = NULL;
    uint32_t lm_off = TR_LM + (uint32_t)(lm - m_LmRing),
             tile_off = TR_TEX + (uint32_t)tex * 65536u;
    SpanTraceMem(lm_off, lm, bytes);
#endif
    if ((int32_t)m_SpanWater != m_EngPal) {   // the other palette: the engine idle first
        HwWait();
        SpanLoadPalette(m_SpanWater);
    }
    uint32_t treg = HWR_CLK();
    HWR_TADD(lm_clk, tlm);
    SPAN_W(0, LIT_B | 1u << 27 | (uint32_t)lh << 22 | (uint32_t)lw << 17
        | lm_off >> 6);
    SPAN_W(0, LIT_A | (uint32_t)m_Eng.tiled << 29 | (uint32_t)m_Eng.lshift << 26
        | (uint32_t)m_Eng.th_log2 << 22 | (uint32_t)m_Eng.tw_log2 << 18
        | (uint32_t)m_Eng.toff << 9 | (uint32_t)m_Eng.soff);
    SPAN_W(1, tile_off);
    SPAN_W(2, (uint32_t)m_Eng.es << 12 | (uint32_t)m_Eng.et << 18);
    SPAN_W(3, (uint32_t)m_Eng.s0);
    SPAN_W(4, (uint32_t)m_Eng.su);
    SPAN_W(5, (uint32_t)m_Eng.sv);
    SPAN_W(6, (uint32_t)m_Eng.t0);
    SPAN_W(7, (uint32_t)m_Eng.tu);
    SPAN_W(8, (uint32_t)m_Eng.tv);
    SPAN_W(9, (uint32_t)m_Eng.z0);
    SPAN_W(10, (uint32_t)m_Eng.zu);
    SPAN_W(11, (uint32_t)m_Eng.zv);
    SPAN_W(12, 0);
    SPAN_W(13, 0);
    SPAN_W(14, (uint32_t)m_Eng.smax);
    SPAN_W(15, (uint32_t)m_Eng.tmax);
    HWR_TADD(regs_clk, treg);
#ifdef M20_BOARD
    m_HwQueued = true;
#endif

    ts = HWR_CLK();
    // The rows: the edge walk, one span each
    XEDGE el[MAX_CVERTS], er[MAX_CVERTS];
    int32_t nl = 0, nr = 0, ytop = M20_H, ybot = 0;
    for (int32_t i = 0; i < n; i++) {
        const SVERT *a = &v[i], *b = &v[i + 1 < n ? i + 1 : 0];
        if (a->py == b->py) {
            continue;
        }
        XEDGE *e;
        if (a->py < b->py) {
            e = &er[nr++];
        } else {
            const SVERT *t = a;
            a = b;
            b = t;
            e = &el[nl++];
        }
        e->y0 = a->py;
        e->y1 = b->py;
        // (|b->px - a->px| < 2^15: StreamFace's guard band, or the window)
        e->dx = ((b->px - a->px) << 16) / (b->py - a->py);
        e->x = (a->px << 16) + 0x8000;
        ytop = a->py < ytop ? a->py : ytop;
        ybot = b->py > ybot ? b->py : ybot;
    }
    int32_t ys = ytop < m_ClipT ? m_ClipT : ytop;
    int32_t ye = ybot > m_ClipB + 1 ? m_ClipB + 1 : ybot;
    if (!nl || !nr || ys >= ye) {
        return true;
    }
    SortEdges(el, nl);
    SortEdges(er, nr);
    m_RowMin = ys;
    m_RowMax = ye - 1;
    int32_t bx0 = M20_W, bx1 = -1;
    const XEDGE *l = el, *le = el + nl, *r = er, *re = er + nr;
    for (int32_t y = ys; y < ye; y++) {
        while (y >= l->y1 && ++l < le) {
        }
        while (y >= r->y1 && ++r < re) {
        }
        if (l >= le || r >= re) {
            break;
        }
        if (y < l->y0 || y < r->y0) {
            continue;   // (a gap: not a convex face)
        }
        int32_t xl = l->x + l->dx * (y - l->y0);
        int32_t xr = r->x + r->dx * (y - r->y0);
        if (xl > xr) {
            int32_t t = xl;
            xl = xr;
            xr = t;
        }
        int32_t x0 = xl >> 16, xe = xr >> 16;   // (xe exclusive)
        x0 = x0 < m_ClipL ? m_ClipL : x0;
        xe = xe > m_ClipR + 1 ? m_ClipR + 1 : xe;
        if (x0 >= xe) {
            continue;
        }
        bx0 = x0 < bx0 ? x0 : bx0;
        bx1 = xe - 1 > bx1 ? xe - 1 : bx1;
        HWR_SpanStats.pixels += xe - x0;
#ifdef M20_BOARD
        uint32_t tf = HWR_CLK();
        SPAN_HW[0] = (uint32_t)(xe - x0) << 19 | (uint32_t)y << 10 | (uint32_t)x0;
        HWR_TADD(fifo_clk, tf);
        HWR_Times.rows++;
#else
        SPAN_W(0, (uint32_t)(xe - x0) << 19 | (uint32_t)y << 10 | (uint32_t)x0);
        m_PixFb = M20_Frame + y * M20_W + x0;
#if M20_ZBUF
        m_PixZb = m_Stream ? m_ZBuf + y * M20_W + x0 : NULL;
#endif
        sc_span_lit(&m_Eng, x0, y, xe - x0);
#endif
    }
    HWR_TADD(sec[3], ts);
    HWR_SpanStats.faces++;
    if (bx0 <= bx1) {   // its box, to what the engine has queued
        int32_t c0 = bx0 >> 5, c1 = bx1 >> 5;
        if (m_EqX0 > m_EqX1) {
            m_EqX0 = c0;
            m_EqX1 = c1;
            m_EqY0 = m_RowMin;
            m_EqY1 = m_RowMax;
        } else {
            m_EqX0 = c0 < m_EqX0 ? c0 : m_EqX0;
            m_EqX1 = c1 > m_EqX1 ? c1 : m_EqX1;
            m_EqY0 = m_RowMin < m_EqY0 ? m_RowMin : m_EqY0;
            m_EqY1 = m_RowMax > m_EqY1 ? m_RowMax : m_EqY1;
        }
    }
    return true;
}

// A face from the list (the sorted path): its vertices from the list's
static bool SpanFace(const PRIM *p)
{
    const HVERT *h = &m_Verts[p->first];
    int32_t n = p->n;
    if (n < 3 || n > MAX_CVERTS) {
        return false;
    }
    SVERT v[MAX_CVERTS];
    for (int32_t i = 0; i < n; i++) {
        float rz = (1.0f / 256.0f) / h[i].iz;
        v[i].x = h[i].x * (1.0f / 16.0f);
        v[i].y = h[i].y * (1.0f / 16.0f);
        v[i].iz = h[i].iz;
        v[i].s = h[i].uz * rz;
        v[i].t = h[i].vz * rz;
        v[i].g = h[i].g;
        v[i].px = (h[i].x + 8) >> 4;
        v[i].py = (h[i].y + 8) >> 4;
    }
    m_SpanWater = p->water;
    return SpanFaceV(v, n, p->tex);
}

// A face straight from InsertFace (the engine tests the depth): clipped to
// the window (the room's portals, as the list's faces are) by the edge
// walk's rows and columns, without clipping the polygon, unless it reaches
// far off the screen
#define GUARD 4096.0f
static bool StreamFace2(CVERT *c, int32_t n, uint16_t tex);
static bool StreamFace(CVERT *c, int32_t n, uint16_t tex)
{
    uint32_t t0 = HWR_CLK();
    bool ok = StreamFace2(c, n, tex);
    HWR_TADD(stream_clk, t0);
    return ok;
}
static bool StreamFace2(CVERT *c, int32_t n, uint16_t tex)
{
    int32_t l, t, r, b;
    GetWindow(&l, &t, &r, &b);
    if (l > r || t > b) {
        return true;   // (nothing to draw)
    }
    float x0 = c[0].x, x1 = x0, y0 = c[0].y, y1 = y0;
    for (int32_t i = 1; i < n; i++) {
        x0 = c[i].x < x0 ? c[i].x : x0;
        x1 = c[i].x > x1 ? c[i].x : x1;
        y0 = c[i].y < y0 ? c[i].y : y0;
        y1 = c[i].y > y1 ? c[i].y : y1;
    }
    if (x1 < (float)l || x0 > (float)(r + 1) || y1 < (float)t
        || y0 > (float)(b + 1)) {
        return true;   // outside the window
    }
    bool clipped = false;
    if (x0 < -GUARD || x1 > GUARD || y0 < -GUARD || y1 > GUARD) {
        n = ClipScreen(c, n);
        if (!n) {
            return true;
        }
        clipped = true;
    }
    if (n > MAX_CVERTS) {
        return false;
    }
    SVERT v[MAX_CVERTS];
    for (int32_t i = 0; i < n; i++) {
        v[i].x = c[i].x;
        v[i].y = c[i].y;
        v[i].iz = c[i].iz;
        if (clipped) {   // (ClipSide interpolates u/z and v/z only)
            float rz = (1.0f / 256.0f) / c[i].iz;
            v[i].s = c[i].uz * rz;
            v[i].t = c[i].vz * rz;
        } else {
            v[i].s = c[i].s;
            v[i].t = c[i].t;
        }
        v[i].g = (int32_t)c[i].g;
        // (as EmitFace and PixelVerts round: to 1/16, then to the pixel)
        v[i].px = (FloorI(c[i].x * 16.0f + 0.5f) + 8) >> 4;
        v[i].py = (FloorI(c[i].y * 16.0f + 0.5f) + 8) >> 4;
    }
    int32_t cl = m_ClipL, ct = m_ClipT, cr = m_ClipR, cb = m_ClipB;
    m_ClipL = l;
    m_ClipT = t;
    m_ClipR = r;
    m_ClipB = b;
    bool ok = SpanFaceV(v, n, tex);
    m_ClipL = cl;
    m_ClipT = ct;
    m_ClipR = cr;
    m_ClipB = cb;
    return ok;
}
#undef GUARD
