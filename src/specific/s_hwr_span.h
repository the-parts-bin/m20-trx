// The span engine (m20-system's core/rtl/span_engine.v) for the renderer's
// opaque textured faces: included by s_hwr.c, inside its rasterizer (it
// uses PRIM, HVERT and the edge walk). The engine maps the texture with
// perspective every 16 pixels and lights it per pixel from a small
// lightmap; the CPU draws everything else (textures with transparent
// texels, flat faces, shadows, sprites, the 2D, and the faces under water,
// whose shading the engine's single palette can't do) in the same
// back-to-front order.
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
bool HWR_SpanOn = true;
#else
bool HWR_SpanOn = false;
#endif
HWR_SPAN_STATS HWR_SpanStats;

static uint32_t m_Pal888[256];
static bool m_Pal888Changed = true;

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
#define LIT_A (1u << 30)
#define LIT_B (2u << 30)
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
#else
static void HwWait(void)
{
    m_EqX0 = 1;
    m_EqX1 = 0;
}
static void CacheFlush(const void *p, uint32_t n)
{
}
static void CacheClean(const void *p, uint32_t n)
{
}
// The C model's pixels
static uint16_t *m_PixFb;
static const uint8_t *m_PixTex;
static const uint8_t *m_PixLight;
#define SC_LPIXEL(i, tex_off, scale, z16)                                      \
    do {                                                                       \
        uint8_t t_ = m_PixTex[tex_off];                                        \
        m_PixFb[i] = sc_shade(m_Pal888[t_], t_, scale);                        \
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
    float w = (float)(M20_W - 1), h = (float)(M20_H - 1);
    float m = fabsf(o), c;
    c = fabsf(o + w * dx);
    if (c > m) {
        m = c;
    }
    c = fabsf(o + h * dy);
    if (c > m) {
        m = c;
    }
    c = fabsf(o + w * dx + h * dy);
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
        CacheClean(t, 65536);
    }
}

static void SpanPalette(void)
{
    for (int32_t i = 0; i < 256; i++) {
        uint32_t r = GamePalette[i].r * 4, g = GamePalette[i].g * 4,
                 b = GamePalette[i].b * 4;
        m_Pal888[i] = r << 16 | g << 8 | b;
    }
    m_Pal888Changed = true;
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
    CacheFlush(M20_Frame, M20_W * M20_H * 2);
    if (m_Pal888Changed) {
        SPAN_HW[24] = 0;
        for (int32_t k = 0; k < 256; k++) {
            SPAN_HW[25] = m_Pal888[k];
        }
        m_Pal888Changed = false;
    }
    SPAN_HW[16] = SDRAM_OFF(M20_Frame);
    SPAN_HW[17] = M20_W;
    SPAN_HW[18] = SDRAM_OFF(m_ZDummy);
    SPAN_HW[19] = 0;
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
static bool SpanFace(const PRIM *p)
{
    const HVERT *v = &m_Verts[p->first];
    int32_t n = p->n;
    if (n < 3 || n > MAX_CVERTS) {
        return false;
    }

    // Each vertex: the screen (1/16 pixel), 1/z (world units: the view's
    // z >> W2V_SHIFT, so that 1/z suits the engine's 2^31 scale), the
    // texel (8.8, texel centres), the light
    float X[MAX_CVERTS], Y[MAX_CVERTS], W[MAX_CVERTS], S[MAX_CVERTS],
        T[MAX_CVERTS];
    float smin = 1e9f, tmin = 1e9f, smax = -1e9f, tmax = -1e9f;
    for (int32_t i = 0; i < n; i++) {
        X[i] = v[i].x * (1.0f / 16.0f);
        Y[i] = v[i].y * (1.0f / 16.0f);
        W[i] = (float)(1 << W2V_SHIFT) * v[i].iz;
        float rz = (1.0f / 256.0f) / v[i].iz;
        S[i] = v[i].uz * rz;
        T[i] = v[i].vz * rz;
        smin = S[i] < smin ? S[i] : smin;
        smax = S[i] > smax ? S[i] : smax;
        tmin = T[i] < tmin ? T[i] : tmin;
        tmax = T[i] > tmax ? T[i] : tmax;
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
        float d = (X[i] - X[0]) * (Y[i + 1] - Y[0])
            - (X[i + 1] - X[0]) * (Y[i] - Y[0]);
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
    float x1 = X[b] - X[0], y1 = Y[b] - Y[0], x2 = X[c] - X[0], y2 = Y[c] - Y[0];
#define PLANE(q0, q1, q2, o, dx, dy)                                           \
    do {                                                                       \
        float d1_ = (q1) - (q0), d2_ = (q2) - (q0);                            \
        dx = (d1_ * y2 - d2_ * y1) * id;                                       \
        dy = (x1 * d2_ - x2 * d1_) * id;                                       \
        o = (q0)-X[0] * dx - Y[0] * dy;                                        \
    } while (0)
    float zo, zdx, zdy, so, sdx, sdy, to, tdx, tdy;
    PLANE(W[0], W[b], W[c], zo, zdx, zdy);
    PLANE((S[0] - umin) * W[0], (S[b] - umin) * W[b], (S[c] - umin) * W[c], so,
          sdx, sdy);
    PLANE((T[0] - vmin) * W[0], (T[b] - vmin) * W[b], (T[c] - vmin) * W[c], to,
          tdx, tdy);
#undef PLANE
    // (pixel x, y's centre is at x, y on the screen: the CPU rounds the
    // vertices to pixels and samples there, and the engine evaluates the
    // planes at the pixel's x, y)

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

    // The light: a + b s + c t, least squares over the vertices
    float la = 0, lb = 0, lc = 0;
    {
        float S00 = n, S0s = 0, S0t = 0, Sss = 0, Sst = 0, Stt = 0, R0 = 0,
              Rs = 0, Rt = 0;
        for (int32_t i = 0; i < n; i++) {
            float s = S[i] - umin, t = T[i] - vmin, l = SpanLightSample(v[i].g);
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
    uint8_t *lm = m_LmRing + m_LmPos;
    m_LmPos += bytes;
    {   // (in 16.16, stepped)
        int32_t q0 = (int32_t)(la * 65536.0f) + 32768;
        int32_t qs = (int32_t)(lb * (float)(65536 << k));
        int32_t qt = (int32_t)(lc * (float)(65536 << k));
        uint8_t *row = lm;
        for (int32_t j = 0; j < lh; j++, q0 += qt, row += lw) {
            int32_t q = q0;
            for (int32_t i = 0; i < lw; i++, q += qs) {
                int32_t s = q >> 16;
                row[i] = (uint8_t)(s < 0 ? 0 : s > 255 ? 255 : s);
            }
        }
    }
    m_Eng.lshift = k;
    m_Eng.lw = lw;

    const uint8_t *tile = (const uint8_t *)TexturePagePtrs[p->tex];
#ifdef M20_BOARD
    if (m_CpuY1 >= m_CpuY0) {   // the CPU's rows to SDRAM first
        CacheFlush(M20_Frame + m_CpuY0 * M20_W,
                   (uint32_t)(m_CpuY1 - m_CpuY0 + 1) * M20_W * 2);
        m_CpuY0 = M20_H;
        m_CpuY1 = -1;
    }
    CacheClean(lm, (uint32_t)(lw * lh));
    SPAN_HW[0] = LIT_B | 1u << 27 | (uint32_t)lh << 22 | (uint32_t)lw << 17
        | SDRAM_OFF(lm) >> 6;
    SPAN_HW[0] = LIT_A | (uint32_t)m_Eng.lshift << 26
        | (uint32_t)m_Eng.th_log2 << 22 | (uint32_t)m_Eng.tw_log2 << 18
        | (uint32_t)m_Eng.toff << 9 | (uint32_t)m_Eng.soff;
    SPAN_HW[1] = SDRAM_OFF(tile);
    SPAN_HW[2] = (uint32_t)m_Eng.es << 12 | (uint32_t)m_Eng.et << 18;
    SPAN_HW[3] = (uint32_t)m_Eng.s0;
    SPAN_HW[4] = (uint32_t)m_Eng.su;
    SPAN_HW[5] = (uint32_t)m_Eng.sv;
    SPAN_HW[6] = (uint32_t)m_Eng.t0;
    SPAN_HW[7] = (uint32_t)m_Eng.tu;
    SPAN_HW[8] = (uint32_t)m_Eng.tv;
    SPAN_HW[9] = (uint32_t)m_Eng.z0;
    SPAN_HW[10] = (uint32_t)m_Eng.zu;
    SPAN_HW[11] = (uint32_t)m_Eng.zv;
    SPAN_HW[12] = 0;
    SPAN_HW[13] = 0;
    SPAN_HW[14] = (uint32_t)m_Eng.smax;
    SPAN_HW[15] = (uint32_t)m_Eng.tmax;
    m_HwQueued = true;
#else
    m_PixTex = tile;
    m_PixLight = lm;
#endif

    // The rows: the CPU's edge walk, one span each
    if (!ScanFace(p, false)) {
        return true;
    }
    int32_t bx0 = M20_W, bx1 = -1;
    for (int32_t y = m_RowMin; y <= m_RowMax; y++) {
        const EDGE *l, *r;
        int32_t x0, xe;
        if (!RowSpan(y, &l, &r, &x0, &xe)) {
            continue;
        }
        bx0 = x0 < bx0 ? x0 : bx0;
        bx1 = xe - 1 > bx1 ? xe - 1 : bx1;
        HWR_SpanStats.pixels += xe - x0;
#ifdef M20_BOARD
        SPAN_HW[0] = (uint32_t)(xe - x0) << 19 | (uint32_t)y << 10 | (uint32_t)x0;
#else
        m_PixFb = M20_Frame + y * M20_W + x0;
        sc_span_lit(&m_Eng, x0, y, xe - x0);
#endif
    }
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
