// m20: the software renderer, in place of TombATI's hardware one (the same
// calls, s_hwr.h), drawing into the platform's RGB565 frame.
//
// Like the original DOS game's renderer, it sorts: everything drawn between
// HWR_InitPolyList and HWR_OutputPolyList (faces, sprites, the 2D overlay)
// goes into a list with a depth, and is drawn back to front at the end (a
// stable radix sort, so that the overlay, at depth 0, is drawn last and in
// the order it was given). Room faces sort by their farthest vertex, the
// objects' by their average depth (as OpenLara's renderer, ../openlara).
//
// Faces: textured ones are mapped with perspective (exact every 16 pixels,
// linear between), Gouraud shaded; colour 0 of the texture pages is
// transparent (TombATI's chroma key). Shading: the vertices' light, 0
// (bright) .. 0x1FFF (dark), as TombATI lights them: full brightness up to
// 0x1000, then down to black, times the brightness setting; 32 steps, a
// table of the palette's 256 colours, as RGB565, for each (and another set
// for under water, tinted by the level's water colour).
#include "specific/s_hwr.h"

#include "3dsystem/3d_gen.h"
#include "config.h"
#include "game/screen.h"
#include "game/viewport.h"
#include "global/vars.h"
#include "log.h"
#include "m20/m20_platform.h"
#include "memory.h"

#include <math.h>
#include <string.h>

#ifndef M20_MAX_PRIMS
#define M20_MAX_PRIMS 6000
#endif
#ifndef M20_MAX_VERTS
#define M20_MAX_VERTS 20000
#endif

HWR_STATS HWR_Stats;
bool HWR_RoomMode;   // S_InsertRoom's faces (3d_gen.c)

typedef enum {
    PRIM_GT,        // textured face; tpage, flags: transparent
    PRIM_G,         // flat face; colour index
    PRIM_SPRITE,    // 2 vertices: corners (x, y, u, v); g the shade
    PRIM_QUAD2D,    // 4 vertices, u the RGB565 colour of each
    PRIM_LINE2D,    // 2 vertices, u the colour
    PRIM_TRANS,     // half-darkened face (shadows, translucent boxes)
    PRIM_LIGHTNING, // face blended with light blue
} PRIM_TYPE;

typedef struct HVERT {
    int32_t x;
    int32_t y;
    float z;        // view depth (PHD_VBUF.zv)
    uint16_t u;     // texel, 8.8 (or a colour)
    uint16_t v;
    int16_t g;      // light
    int16_t pad;
} HVERT;

typedef struct PRIM {
    uint8_t type;
    uint8_t n;          // vertices
    uint8_t water;      // the under-water shading
    uint8_t pad;
    uint16_t tex;       // texture page or colour
    uint16_t first;     // its first vertex
} PRIM;

static PRIM m_Prims[M20_MAX_PRIMS];
static uint16_t m_Keys[M20_MAX_PRIMS];
static uint16_t m_Order[2][M20_MAX_PRIMS];
static HVERT m_Verts[M20_MAX_VERTS];
static int32_t m_NumPrims;
static int32_t m_NumVerts;

static uint16_t m_Shade[32][256];       // light step, colour -> RGB565
static uint16_t m_ShadeWater[32][256];
static RGBF m_WaterColor = { 1.0f, 1.0f, 1.0f };
static uint16_t m_Picture[M20_W * M20_H];   // the background (title, menus)
static bool m_PictureValid;

static int32_t m_ClipL, m_ClipT, m_ClipR, m_ClipB;   // inclusive

// ---------------------------------------------------------------------
// Palette and shading
// ---------------------------------------------------------------------
static uint16_t rgb565(int32_t r, int32_t g, int32_t b)
{
    CLAMP(r, 0, 255);
    CLAMP(g, 0, 255);
    CLAMP(b, 0, 255);
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
}

static void BuildShadeTables()
{
    float brightness = T1MConfig.brightness;
    for (int s = 0; s < 32; s++) {
        float f = (8192.0f - (s * 256 + 128)) / 4096.0f * brightness;
        if (f > 1.0f) {
            f = 1.0f;
        }
        for (int c = 0; c < 256; c++) {
            float r = GamePalette[c].r * 4 * f;
            float g = GamePalette[c].g * 4 * f;
            float b = GamePalette[c].b * 4 * f;
            m_Shade[s][c] = rgb565(r, g, b);
            m_ShadeWater[s][c] = rgb565(
                r * m_WaterColor.r, g * m_WaterColor.g, b * m_WaterColor.b);
        }
    }
}

void HWR_SetPalette()
{
    BuildShadeTables();
}

void HWR_SetWaterColor(const RGBF *color)
{
    m_WaterColor = *color;
    BuildShadeTables();
}

void HWR_DownloadTextures(int32_t pages)
{
    // the pages stay where the level loaded them (TexturePagePtrs)
    LOG_INFO("m20: %d texture pages", pages);
}

// ---------------------------------------------------------------------
// Frame, picture, setup
// ---------------------------------------------------------------------
void HWR_InitialiseHardware()
{
    HWR_SwitchResolution();
}

void HWR_ShutdownHardware()
{
}

void HWR_SwitchResolution()
{
    Screen_SetupSize();
}

void HWR_ClearSurfaceDepth()
{
    memset(M20_Frame, 0, M20_W * M20_H * 2);
}

void HWR_DumpScreen()
{
    if (m_NumPrims) {
        HWR_OutputPolyList();
    }
    M20_Present();
}

void HWR_FadeToPal(int32_t fade_value, RGB888 *palette)
{
}

void HWR_FadeWait()
{
    HWR_ClearSurfaceDepth();
    HWR_DumpScreen();
}

static uint16_t *PictureBuffer()
{
    return m_Picture;
}

// The frame as the background (the inventory's, over the game)
void HWR_CopyPicture()
{
    memcpy(PictureBuffer(), M20_Frame, M20_W * M20_H * 2);
    m_PictureValid = true;
}

// A picture (6 bits a component) as the background, fitted to the frame
void HWR_DownloadPicture(const PICTURE *pic)
{
    uint16_t *dst = PictureBuffer();
    memset(dst, 0, M20_W * M20_H * 2);
    int32_t w = M20_W, h = M20_H;
    if (pic->width * M20_H > pic->height * M20_W) {
        h = M20_W * pic->height / pic->width;
    } else {
        w = M20_H * pic->width / pic->height;
    }
    int32_t x0 = (M20_W - w) / 2, y0 = (M20_H - h) / 2;
    for (int32_t y = 0; y < h; y++) {
        const RGB888 *src = pic->data + (y * pic->height / h) * pic->width;
        uint16_t *row = dst + (y0 + y) * M20_W + x0;
        for (int32_t x = 0; x < w; x++) {
            const RGB888 *c = &src[x * pic->width / w];
            row[x] = ((c->r >> 1) << 11) | (c->g << 5) | (c->b >> 1);
        }
    }
    m_PictureValid = true;
}

// The background into the frame (S_CopyBufferToScreen)
void HWR_ShowPicture()
{
    if (m_PictureValid) {
        memcpy(M20_Frame, m_Picture, M20_W * M20_H * 2);
    } else {
        HWR_ClearSurfaceDepth();
    }
}

// ---------------------------------------------------------------------
// The list
// ---------------------------------------------------------------------
void HWR_InitPolyList()
{
    m_NumPrims = 0;
    m_NumVerts = 0;
}

static PRIM *AddPrim(PRIM_TYPE type, int32_t n, int32_t key, HVERT **verts)
{
    if (m_NumPrims >= M20_MAX_PRIMS || m_NumVerts + n > M20_MAX_VERTS) {
        HWR_Stats.overflows++;
        return NULL;
    }
    PRIM *p = &m_Prims[m_NumPrims];
    CLAMP(key, 0, 0xFFFF);
    m_Keys[m_NumPrims] = 0xFFFF - key;   // ascending order = far first
    m_NumPrims++;
    p->type = type;
    p->n = n;
    p->water = IsShadeEffect;
    p->first = m_NumVerts;
    *verts = &m_Verts[m_NumVerts];
    m_NumVerts += n;
    return p;
}

// A face's vertex while it's set up: screen position (float, unclamped),
// 1/z, u/z, v/z and light, all of which interpolate linearly on the screen
typedef struct CVERT {
    float x, y;
    float iz, uz, vz;
    float g;
} CVERT;

#define MAX_CVERTS 16

// The game's projection clamps vertices to +-32760 (and its integer
// division rounds): a vertex off the screen (clip bits 0..3) is projected
// again here from its view position, unclamped, so that a face that
// reaches far off the screen keeps its shape (a floor seen edge on, its
// near vertices beside the camera)
static void SetVert(CVERT *c, const PHD_VBUF *v, const PHD_UV *uv)
{
    if (v->clip & 15) {
        float k = (float)PhdPersp / (float)v->zv;
        c->x = v->xv * k + ViewPort_GetCenterX();
        c->y = v->yv * k + ViewPort_GetCenterY();
    } else {
        c->x = v->xs;
        c->y = v->ys;
    }
    c->iz = 1.0f / (float)v->zv;
    c->g = v->g;
    if (uv) {
        c->uz = ((uv->u1 & 0xFF00) + 0x80) * c->iz;
        c->vz = ((uv->v1 & 0xFF00) + 0x80) * c->iz;
    } else {
        c->uz = c->vz = 0.0f;
    }
}

// Clipping at the near plane, from TombATI's HWR_ZedClipper
static int32_t ZedClip(
    int32_t n, PHD_VBUF *const *vb, PHD_UV *const *uv, CVERT *out)
{
    float near_z = phd_GetNearZ();
    float persp = PhdPersp / near_z;
    int32_t count = 0;
    int32_t j = n - 1;
    for (int32_t i = 0; i < n; j = i++) {
        const PHD_VBUF *p1 = vb[j], *p0 = vb[i];
        const PHD_UV *u1 = uv ? uv[j] : NULL, *u0 = uv ? uv[i] : NULL;
        bool in1 = p1->zv >= near_z, in0 = p0->zv >= near_z;
        if (in1 != in0) {
            float t = (near_z - p0->zv) / (float)(p1->zv - p0->zv);
            CVERT *c = &out[count++];
            c->x = (p0->xv + (p1->xv - p0->xv) * t) * persp
                + ViewPort_GetCenterX();
            c->y = (p0->yv + (p1->yv - p0->yv) * t) * persp
                + ViewPort_GetCenterY();
            c->iz = 1.0f / near_z;
            c->g = p0->g + (p1->g - p0->g) * t;
            if (uv) {
                float uu0 = (u0->u1 & 0xFF00) + 0x80, uu1 = (u1->u1 & 0xFF00) + 0x80;
                float vv0 = (u0->v1 & 0xFF00) + 0x80, vv1 = (u1->v1 & 0xFF00) + 0x80;
                c->uz = (uu0 + (uu1 - uu0) * t) * c->iz;
                c->vz = (vv0 + (vv1 - vv0) * t) * c->iz;
            } else {
                c->uz = c->vz = 0.0f;
            }
        }
        if (in0) {
            SetVert(&out[count++], p0, u0);
        }
    }
    return count >= 3 ? count : 0;
}

// Clipping to the screen, one side at a time (Sutherland-Hodgman): side 0
// x >= 0, 1 x <= W, 2 y >= 0, 3 y <= H
static int32_t ClipSide(const CVERT *in, int32_t n, CVERT *out, int side)
{
    float lim = side == 1 ? (float)M20_W : side == 3 ? (float)M20_H : 0.0f;
    int32_t count = 0;
    for (int32_t i = 0; i < n; i++) {
        const CVERT *a = &in[i], *b = &in[(i + 1) % n];
        float da = (side < 2 ? a->x : a->y) - lim;
        float db = (side < 2 ? b->x : b->y) - lim;
        if (side & 1) {
            da = -da;
            db = -db;
        }
        if (da >= 0.0f) {
            out[count++] = *a;
        }
        if ((da >= 0.0f) != (db >= 0.0f) && count < MAX_CVERTS) {
            float t = da / (da - db);
            CVERT *c = &out[count++];
            c->x = a->x + (b->x - a->x) * t;
            c->y = a->y + (b->y - a->y) * t;
            c->iz = a->iz + (b->iz - a->iz) * t;
            c->uz = a->uz + (b->uz - a->uz) * t;
            c->vz = a->vz + (b->vz - a->vz) * t;
            c->g = a->g + (b->g - a->g) * t;
        }
        if (count >= MAX_CVERTS - 1) {
            break;
        }
    }
    return count;
}

static int32_t ClipScreen(CVERT *v, int32_t n)
{
    bool out = false;
    for (int32_t i = 0; i < n; i++) {
        if (v[i].x < 0.0f || v[i].x > M20_W || v[i].y < 0.0f
            || v[i].y > M20_H) {
            out = true;
            break;
        }
    }
    if (!out) {
        return n;
    }
    CVERT tmp[MAX_CVERTS];
    n = ClipSide(v, n, tmp, 0);
    if (n >= 3) n = ClipSide(tmp, n, v, 1);
    if (n >= 3) n = ClipSide(v, n, tmp, 2);
    if (n >= 3) n = ClipSide(tmp, n, v, 3);
    return n >= 3 ? n : 0;
}

static int32_t FaceKey(PHD_VBUF *const *vb, int32_t n)
{
    int32_t key;
    if (HWR_RoomMode) {
        key = vb[0]->zv;
        for (int32_t i = 1; i < n; i++) {
            if (vb[i]->zv > key) {
                key = vb[i]->zv;
            }
        }
    } else {
        int64_t sum = 0;
        for (int32_t i = 0; i < n; i++) {
            sum += vb[i]->zv;
        }
        key = sum / n;
    }
    return key >> W2V_SHIFT;
}

// The face's vertices, clipped, into the list
static void EmitFace(PRIM_TYPE type, CVERT *c, int32_t n, int32_t key, uint16_t tex)
{
    n = ClipScreen(c, n);
    if (!n) {
        return;
    }
    HVERT *dst;
    PRIM *p = AddPrim(type, n, key, &dst);
    if (!p) {
        return;
    }
    p->tex = tex;
    for (int32_t i = 0; i < n; i++) {
        float z = 1.0f / c[i].iz;
        dst[i].x = (int32_t)floorf(c[i].x + 0.5f);
        dst[i].y = (int32_t)floorf(c[i].y + 0.5f);
        dst[i].z = z;
        dst[i].u = (uint16_t)(c[i].uz * z);
        dst[i].v = (uint16_t)(c[i].vz * z);
        dst[i].g = (int16_t)c[i].g;
    }
}

static void InsertFace(
    PRIM_TYPE type, int32_t n, PHD_VBUF *const *vb, PHD_UV *const *uv,
    uint16_t tex)
{
    int16_t clip_and = -1, clip_or = 0;
    for (int32_t i = 0; i < n; i++) {
        clip_and &= vb[i]->clip;
        clip_or |= vb[i]->clip;
    }
    if (clip_and) {
        return;   // all off one side (or all too far: bit 4)
    }

    CVERT c[MAX_CVERTS];
    int32_t count;
    if (clip_or < 0) {
        if (type == PRIM_G) {
            return;   // as TombATI: flat faces aren't clipped at the near plane
        }
        if (!phd_VisibleZClip(vb[0], vb[1], vb[2])) {
            return;
        }
        count = ZedClip(n, vb, uv, c);
        if (!count) {
            return;
        }
    } else {
        for (int32_t i = 0; i < n; i++) {
            SetVert(&c[i], vb[i], uv ? uv[i] : NULL);
        }
        // facing the camera (TombATI's test, in float: the coordinates can
        // be far beyond 16 bits)
        float area = (c[0].y - c[1].y) * (c[2].x - c[1].x)
            - (c[2].y - c[1].y) * (c[0].x - c[1].x);
        if (area < 0.0f) {
            return;
        }
        count = n;
    }
    EmitFace(type, c, count, FaceKey(vb, n), tex);
}

const int16_t *HWR_InsertObjectG3(const int16_t *obj_ptr, int32_t number)
{
    for (int32_t i = 0; i < number; i++) {
        PHD_VBUF *vb[3] = { &PhdVBuf[obj_ptr[0]], &PhdVBuf[obj_ptr[1]],
                            &PhdVBuf[obj_ptr[2]] };
        InsertFace(PRIM_G, 3, vb, NULL, obj_ptr[3]);
        obj_ptr += 4;
    }
    return obj_ptr;
}

const int16_t *HWR_InsertObjectG4(const int16_t *obj_ptr, int32_t number)
{
    for (int32_t i = 0; i < number; i++) {
        PHD_VBUF *vb[4] = { &PhdVBuf[obj_ptr[0]], &PhdVBuf[obj_ptr[1]],
                            &PhdVBuf[obj_ptr[2]], &PhdVBuf[obj_ptr[3]] };
        InsertFace(PRIM_G, 4, vb, NULL, obj_ptr[4]);
        obj_ptr += 5;
    }
    return obj_ptr;
}

const int16_t *HWR_InsertObjectGT3(const int16_t *obj_ptr, int32_t number)
{
    for (int32_t i = 0; i < number; i++) {
        PHD_VBUF *vb[3] = { &PhdVBuf[obj_ptr[0]], &PhdVBuf[obj_ptr[1]],
                            &PhdVBuf[obj_ptr[2]] };
        PHD_TEXTURE *tex = &PhdTextureInfo[obj_ptr[3]];
        PHD_UV *uv[3] = { &tex->uv[0], &tex->uv[1], &tex->uv[2] };
        InsertFace(PRIM_GT, 3, vb, uv, tex->tpage);
        obj_ptr += 4;
    }
    return obj_ptr;
}

const int16_t *HWR_InsertObjectGT4(const int16_t *obj_ptr, int32_t number)
{
    for (int32_t i = 0; i < number; i++) {
        PHD_VBUF *vb[4] = { &PhdVBuf[obj_ptr[0]], &PhdVBuf[obj_ptr[1]],
                            &PhdVBuf[obj_ptr[2]], &PhdVBuf[obj_ptr[3]] };
        PHD_TEXTURE *tex = &PhdTextureInfo[obj_ptr[4]];
        PHD_UV *uv[4] = { &tex->uv[0], &tex->uv[1], &tex->uv[2],
                          &tex->uv[3] };
        InsertFace(PRIM_GT, 4, vb, uv, tex->tpage);
        obj_ptr += 5;
    }
    return obj_ptr;
}

void HWR_DrawSprite(
    int16_t x1, int16_t y1, int16_t x2, int y2, int z, int sprnum, int shade)
{
    if (x2 <= x1 || y2 <= y1) {
        return;
    }
    PHD_SPRITE *sprite = &PhdSpriteInfo[sprnum];
    HVERT *v;
    PRIM *p = AddPrim(PRIM_SPRITE, 2, z >> W2V_SHIFT, &v);
    if (!p) {
        return;
    }
    p->tex = sprite->tpage;
    int32_t u0 = (sprite->offset & 0xFF) << 8;
    int32_t v0 = sprite->offset & 0xFF00;
    v[0].x = x1;
    v[0].y = y1;
    v[0].u = u0;
    v[0].v = v0;
    v[1].x = x2;
    v[1].y = y2;
    v[1].u = sprite->width >> 8;    // texels
    v[1].v = sprite->height >> 8;
    CLAMP(shade, 0, 0x1FFF);
    v[0].g = shade;
}

static uint16_t Rgb(RGB888 c)
{
    return rgb565(c.r, c.g, c.b);
}

void HWR_Draw2DLine(
    int32_t x1, int32_t y1, int32_t x2, int32_t y2, RGB888 color1,
    RGB888 color2)
{
    HVERT *v;
    PRIM *p = AddPrim(PRIM_LINE2D, 2, 0, &v);
    if (!p) {
        return;
    }
    v[0].x = x1;
    v[0].y = y1;
    v[1].x = x2;
    v[1].y = y2;
    v[0].u = Rgb(color1);
}

void HWR_Draw2DQuad(
    int32_t x1, int32_t y1, int32_t x2, int32_t y2, RGB888 tl, RGB888 tr,
    RGB888 bl, RGB888 br)
{
    HVERT *v;
    PRIM *p = AddPrim(PRIM_QUAD2D, 4, 0, &v);
    if (!p) {
        return;
    }
    v[0].x = x1;
    v[0].y = y1;
    v[1].x = x2;
    v[1].y = y2;
    v[0].u = Rgb(tl);
    v[1].u = Rgb(tr);
    v[2].u = Rgb(bl);
    v[3].u = Rgb(br);
}

void HWR_DrawTranslucentQuad(int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
    HVERT *v;
    PRIM *p = AddPrim(PRIM_TRANS, 4, 0, &v);
    if (!p) {
        return;
    }
    v[0].x = x1;
    v[0].y = y1;
    v[1].x = x2;
    v[1].y = y1;
    v[2].x = x2;
    v[2].y = y2;
    v[3].x = x1;
    v[3].y = y2;
}

void HWR_PrintShadow(PHD_VBUF *vbufs, int clip, int vertex_count)
{
    if (vertex_count > MAX_CVERTS - 4) {
        return;
    }
    int32_t key = 0;
    CVERT c[MAX_CVERTS];
    for (int32_t i = 0; i < vertex_count; i++) {
        if (vbufs[i].clip < 0) {
            return;
        }
        if (vbufs[i].zv > key) {
            key = vbufs[i].zv;
        }
        SetVert(&c[i], &vbufs[i], NULL);
    }
    EmitFace(PRIM_TRANS, c, vertex_count, key >> W2V_SHIFT, 0);
}

void HWR_DrawLightningSegment(
    int x1, int y1, int z1, int thickness1, int x2, int y2, int z2,
    int thickness2)
{
    CVERT c[MAX_CVERTS] = { 0 };
    c[0].x = x1;
    c[0].y = y1;
    c[1].x = x1 + thickness1;
    c[1].y = y1;
    c[2].x = x2 + thickness2;
    c[2].y = y2;
    c[3].x = x2;
    c[3].y = y2;
    for (int i = 0; i < 4; i++) {
        c[i].iz = 1.0f;
    }
    EmitFace(PRIM_LIGHTNING, c, 4, ((z1 + z2) / 2) >> W2V_SHIFT, 0);
}

// ---------------------------------------------------------------------
// Rasterizing
// ---------------------------------------------------------------------
// A face's outline, a row at a time: for each row its left and right ends
// and, at each, the interpolated 1/z, u/z, v/z and light
typedef struct EDGE {
    int32_t x;      // 16.16
    float iz, uz, vz;
    int32_t g;      // 16.16
} EDGE;

static EDGE m_Left[M20_H], m_Right[M20_H];
static int32_t m_RowMin, m_RowMax;

static void ScanEdge(const HVERT *a, const HVERT *b, bool textured)
{
    EDGE *table;
    if (a->y == b->y) {
        return;
    }
    if (a->y < b->y) {
        table = m_Right;
    } else {
        const HVERT *t = a;
        a = b;
        b = t;
        table = m_Left;
    }
    int32_t y0 = a->y, y1 = b->y;   // rows y0 .. y1 - 1
    int32_t dy = y1 - y0;
    int32_t ys = y0 < m_ClipT ? m_ClipT : y0;
    int32_t ye = y1 > m_ClipB + 1 ? m_ClipB + 1 : y1;
    if (ys >= ye) {
        return;
    }
    if (ys < m_RowMin) {
        m_RowMin = ys;
    }
    if (ye - 1 > m_RowMax) {
        m_RowMax = ye - 1;
    }
    int32_t dx = (int32_t)(((int64_t)(b->x - a->x) << 16) / dy);
    int32_t x = (a->x << 16) + dx * (ys - y0) + 0x8000;
    int32_t dg = ((b->g - a->g) << 16) / dy;
    int32_t g = (a->g << 16) + dg * (ys - y0);
    if (textured) {
        float iza = 1.0f / a->z, izb = 1.0f / b->z;
        float uza = a->u * iza, uzb = b->u * izb;
        float vza = a->v * iza, vzb = b->v * izb;
        float fdy = 1.0f / dy;
        float diz = (izb - iza) * fdy, duz = (uzb - uza) * fdy,
              dvz = (vzb - vza) * fdy;
        float t = (float)(ys - y0);
        float iz = iza + diz * t, uz = uza + duz * t, vz = vza + dvz * t;
        for (int32_t y = ys; y < ye; y++) {
            EDGE *e = &table[y];
            e->x = x;
            e->g = g;
            e->iz = iz;
            e->uz = uz;
            e->vz = vz;
            x += dx;
            g += dg;
            iz += diz;
            uz += duz;
            vz += dvz;
        }
    } else {
        for (int32_t y = ys; y < ye; y++) {
            EDGE *e = &table[y];
            e->x = x;
            e->g = g;
            x += dx;
            g += dg;
        }
    }
}

static bool ScanFace(const HVERT *v, int32_t n, bool textured)
{
    m_RowMin = M20_H;
    m_RowMax = -1;
    for (int32_t i = 0; i < n; i++) {
        ScanEdge(&v[i], &v[(i + 1) % n], textured);
    }
    return m_RowMin <= m_RowMax;
}

// The row's ends, ordered and clipped: false if empty
static bool RowSpan(
    int32_t y, const EDGE **l, const EDGE **r, int32_t *x0, int32_t *x1)
{
    *l = &m_Left[y];
    *r = &m_Right[y];
    if ((*l)->x > (*r)->x) {
        const EDGE *t = *l;
        *l = *r;
        *r = t;
    }
    *x0 = (*l)->x >> 16;
    *x1 = (*r)->x >> 16;   // exclusive
    if (*x0 < m_ClipL) {
        *x0 = m_ClipL;
    }
    if (*x1 > m_ClipR + 1) {
        *x1 = m_ClipR + 1;
    }
    return *x0 < *x1;
}

#define SUBDIV 16

static void DrawGT(const PRIM *p)
{
    const HVERT *v = &m_Verts[p->first];
    if (!ScanFace(v, p->n, true)) {
        return;
    }
    const uint8_t *tex = (const uint8_t *)TexturePagePtrs[p->tex];
    uint16_t(*shade)[256] = p->water ? m_ShadeWater : m_Shade;

    for (int32_t y = m_RowMin; y <= m_RowMax; y++) {
        const EDGE *l, *r;
        int32_t x0, x1;
        if (!RowSpan(y, &l, &r, &x0, &x1)) {
            continue;
        }
        float w = (float)(r->x - l->x) * (1.0f / 65536.0f);
        float inv = w > 0.5f ? 1.0f / w : 0.0f;
        float t0 = ((x0 << 16) + 0x8000 - l->x) * (1.0f / 65536.0f);
        float diz = (r->iz - l->iz) * inv, duz = (r->uz - l->uz) * inv,
              dvz = (r->vz - l->vz) * inv;
        int32_t dg = (int32_t)((r->g - l->g) * inv);
        float iz = l->iz + diz * t0, uz = l->uz + duz * t0,
              vz = l->vz + dvz * t0;
        int32_t g = l->g + (int32_t)(dg * t0);

        uint16_t *dst = M20_Frame + y * M20_W + x0;
        int32_t n = x1 - x0;
        float z = 1.0f / iz;
        int32_t u = (int32_t)(uz * z), vv = (int32_t)(vz * z);
        while (n > 0) {
            int32_t run = n < SUBDIV ? n : SUBDIV;
            iz += diz * run;
            uz += duz * run;
            vz += dvz * run;
            z = 1.0f / iz;
            int32_t u2 = (int32_t)(uz * z), v2 = (int32_t)(vz * z);
            int32_t du = (u2 - u) / run, dv = (v2 - vv) / run;
            for (int32_t i = 0; i < run; i++) {
                uint8_t c = tex[(vv & 0xFF00) | ((u >> 8) & 0xFF)];
                if (c) {
                    *dst = shade[(g >> 24) & 31][c];
                }
                dst++;
                u += du;
                vv += dv;
                g += dg;
            }
            u = u2;
            vv = v2;
            n -= run;
        }
    }
}

static void DrawG(const PRIM *p)
{
    const HVERT *v = &m_Verts[p->first];
    if (!ScanFace(v, p->n, false)) {
        return;
    }
    uint16_t(*shade)[256] = p->water ? m_ShadeWater : m_Shade;
    uint8_t c = p->tex;
    for (int32_t y = m_RowMin; y <= m_RowMax; y++) {
        const EDGE *l, *r;
        int32_t x0, x1;
        if (!RowSpan(y, &l, &r, &x0, &x1)) {
            continue;
        }
        int32_t w = (r->x - l->x) >> 16;
        int32_t dg = w > 0 ? (r->g - l->g) / w : 0;
        int32_t g = l->g + dg * (x0 - (l->x >> 16));
        uint16_t *dst = M20_Frame + y * M20_W;
        for (int32_t x = x0; x < x1; x++) {
            dst[x] = shade[(g >> 24) & 31][c];
            g += dg;
        }
    }
}

static void DrawTrans(const PRIM *p, uint16_t tint)
{
    const HVERT *v = &m_Verts[p->first];
    if (!ScanFace(v, p->n, false)) {
        return;
    }
    for (int32_t y = m_RowMin; y <= m_RowMax; y++) {
        const EDGE *l, *r;
        int32_t x0, x1;
        if (!RowSpan(y, &l, &r, &x0, &x1)) {
            continue;
        }
        uint16_t *dst = M20_Frame + y * M20_W;
        for (int32_t x = x0; x < x1; x++) {
            dst[x] = ((dst[x] >> 1) & 0x7BEF) + tint;
        }
    }
}

static void DrawSprite(const PRIM *p)
{
    const HVERT *v = &m_Verts[p->first];
    int32_t sw = v[1].x - v[0].x, sh = v[1].y - v[0].y;
    int32_t du = (v[1].u << 16) / sw;   // 8.16
    int32_t dv = (v[1].v << 16) / sh;
    int32_t x0 = v[0].x, y0 = v[0].y, x1 = v[1].x, y1 = v[1].y;
    int32_t u0 = v[0].u << 8, vv = v[0].v << 8;
    if (x0 < m_ClipL) {
        u0 += du * (m_ClipL - x0);
        x0 = m_ClipL;
    }
    if (y0 < m_ClipT) {
        vv += dv * (m_ClipT - y0);
        y0 = m_ClipT;
    }
    if (x1 > m_ClipR + 1) {
        x1 = m_ClipR + 1;
    }
    if (y1 > m_ClipB + 1) {
        y1 = m_ClipB + 1;
    }
    const uint8_t *tex = (const uint8_t *)TexturePagePtrs[p->tex];
    const uint16_t *shade = (p->water ? m_ShadeWater : m_Shade)[v[0].g >> 8];
    for (int32_t y = y0; y < y1; y++, vv += dv) {
        uint16_t *dst = M20_Frame + y * M20_W;
        const uint8_t *row = tex + ((vv >> 8) & 0xFF00);
        int32_t u = u0;
        for (int32_t x = x0; x < x1; x++, u += du) {
            uint8_t c = row[(u >> 16) & 0xFF];
            if (c) {
                dst[x] = shade[c];
            }
        }
    }
}

static void DrawQuad2D(const PRIM *p)
{
    const HVERT *v = &m_Verts[p->first];
    int32_t x0 = v[0].x, y0 = v[0].y, x1 = v[1].x, y1 = v[1].y;
    if (x1 <= x0 || y1 <= y0) {
        return;
    }
    // The corners' colours, interpolated (in 5/6/5 steps)
    int32_t c[4][3];
    for (int i = 0; i < 4; i++) {
        c[i][0] = v[i].u >> 11;
        c[i][1] = (v[i].u >> 5) & 63;
        c[i][2] = v[i].u & 31;
    }
    bool flat = v[0].u == v[1].u && v[0].u == v[2].u && v[0].u == v[3].u;
    int32_t w = x1 - x0, h = y1 - y0;
    for (int32_t y = y0 < m_ClipT ? m_ClipT : y0;
         y < (y1 > m_ClipB + 1 ? m_ClipB + 1 : y1); y++) {
        uint16_t *dst = M20_Frame + y * M20_W;
        int32_t fy = ((y - y0) << 8) / h;
        int32_t l[3], r[3];
        for (int k = 0; k < 3; k++) {
            l[k] = c[0][k] + (((c[2][k] - c[0][k]) * fy) >> 8);
            r[k] = c[1][k] + (((c[3][k] - c[1][k]) * fy) >> 8);
        }
        for (int32_t x = x0 < m_ClipL ? m_ClipL : x0;
             x < (x1 > m_ClipR + 1 ? m_ClipR + 1 : x1); x++) {
            if (flat) {
                dst[x] = v[0].u;
                continue;
            }
            int32_t fx = ((x - x0) << 8) / w;
            int32_t cr = l[0] + (((r[0] - l[0]) * fx) >> 8);
            int32_t cg = l[1] + (((r[1] - l[1]) * fx) >> 8);
            int32_t cb = l[2] + (((r[2] - l[2]) * fx) >> 8);
            dst[x] = (cr << 11) | (cg << 5) | cb;
        }
    }
}

static void DrawLine2D(const PRIM *p)
{
    const HVERT *v = &m_Verts[p->first];
    int32_t x0 = v[0].x, y0 = v[0].y, x1 = v[1].x, y1 = v[1].y;
    int32_t dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int32_t dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int32_t err = dx + dy;
    uint16_t c = v[0].u;
    for (int32_t i = 0; i < 4096; i++) {
        if (x0 >= m_ClipL && x0 <= m_ClipR && y0 >= m_ClipT && y0 <= m_ClipB) {
            M20_Frame[y0 * M20_W + x0] = c;
        }
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int32_t e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void HWR_OutputPolyList()
{
    int32_t n = m_NumPrims;
    HWR_Stats.prims = n;
    HWR_Stats.verts = m_NumVerts;
    if (n > HWR_Stats.prims_peak) {
        HWR_Stats.prims_peak = n;
    }
    if (m_NumVerts > HWR_Stats.verts_peak) {
        HWR_Stats.verts_peak = m_NumVerts;
    }

    // Stable radix sort of the keys, 8 bits at a time
    uint16_t *src = m_Order[0], *dst = m_Order[1];
    for (int32_t i = 0; i < n; i++) {
        src[i] = i;
    }
    for (int shift = 0; shift < 16; shift += 8) {
        int32_t count[257] = { 0 };
        for (int32_t i = 0; i < n; i++) {
            count[((m_Keys[src[i]] >> shift) & 0xFF) + 1]++;
        }
        for (int i = 0; i < 256; i++) {
            count[i + 1] += count[i];
        }
        for (int32_t i = 0; i < n; i++) {
            dst[count[(m_Keys[src[i]] >> shift) & 0xFF]++] = src[i];
        }
        uint16_t *t = src;
        src = dst;
        dst = t;
    }

    m_ClipL = 0;
    m_ClipT = 0;
    m_ClipR = M20_W - 1;
    m_ClipB = M20_H - 1;

    for (int32_t i = 0; i < n; i++) {
        const PRIM *p = &m_Prims[src[i]];
        switch (p->type) {
        case PRIM_GT:
            DrawGT(p);
            break;
        case PRIM_G:
            DrawG(p);
            break;
        case PRIM_SPRITE:
            DrawSprite(p);
            break;
        case PRIM_QUAD2D:
            DrawQuad2D(p);
            break;
        case PRIM_LINE2D:
            DrawLine2D(p);
            break;
        case PRIM_TRANS:
            DrawTrans(p, 0);
            break;
        case PRIM_LIGHTNING:
            DrawTrans(p, 0x4C1F & 0x7BEF);
            break;
        }
    }

    m_NumPrims = 0;
    m_NumVerts = 0;
}
