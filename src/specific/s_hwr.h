#ifndef T1M_SPECIFIC_S_HWR_H
#define T1M_SPECIFIC_S_HWR_H

// m20: the renderer is software (s_hwr.c): the same calls as TombATI's
// hardware renderer, which the game makes, drawing into the platform's
// RGB565 frame (src/m20/m20_platform.h)

#include "game/picture.h"
#include "global/types.h"

#include <stdbool.h>
#include <stdint.h>

void HWR_DumpScreen();
void HWR_ClearSurfaceDepth();
void HWR_FadeToPal(int32_t fade_value, RGB888 *palette);
void HWR_FadeWait();
void HWR_CopyPicture();
void HWR_DownloadPicture(const PICTURE *pic);
void HWR_ShowPicture();
void HWR_DrawSprite(
    int16_t x1, int16_t y1, int16_t x2, int y2, int z, int sprnum, int shade);
void HWR_Draw2DLine(
    int32_t x1, int32_t y1, int32_t x2, int32_t y2, RGB888 color1,
    RGB888 color2);
void HWR_Draw2DQuad(
    int32_t x1, int32_t y1, int32_t x2, int32_t y2, RGB888 tl, RGB888 tr,
    RGB888 bl, RGB888 br);
void HWR_DrawTranslucentQuad(int32_t x1, int32_t y1, int32_t x2, int32_t y2);
void HWR_PrintShadow(PHD_VBUF *vbufs, int clip, int vertex_count);
void HWR_DrawLightningSegment(
    int x1, int y1, int z1, int thickness1, int x2, int y2, int z2,
    int thickness2);
void HWR_SwitchResolution();
void HWR_InitialiseHardware();
void HWR_ShutdownHardware();
void HWR_SetPalette();
void HWR_DownloadTextures(int32_t pages);
void HWR_InitPolyList();
void HWR_OutputPolyList();

void HWR_SetWaterColor(const RGBF *color);

const int16_t *HWR_InsertObjectG3(const int16_t *obj_ptr, int32_t number);
const int16_t *HWR_InsertObjectG4(const int16_t *obj_ptr, int32_t number);
const int16_t *HWR_InsertObjectGT3(const int16_t *obj_ptr, int32_t number);
const int16_t *HWR_InsertObjectGT4(const int16_t *obj_ptr, int32_t number);

// m20: the renderer's statistics
typedef struct HWR_STATS {
    int32_t prims;          // primitives in the last frame
    int32_t verts;          // their vertices
    int32_t prims_peak;     // the most in one frame so far
    int32_t verts_peak;
    int32_t overflows;      // primitives dropped (the lists were full)
    int32_t zdiff;          // pixels the depth buffer changed (HWR_ZMode 2)
} HWR_STATS;
extern HWR_STATS HWR_Stats;

// Where a frame's time goes (microseconds, summed): building the list (from
// HWR_InitPolyList to HWR_OutputPolyList: the game's transforms and the
// faces' setup) and drawing it
typedef struct HWR_TIMES {
    uint32_t list_start;
    uint32_t build_us;
    uint32_t draw_us;
} HWR_TIMES;
extern HWR_TIMES HWR_Times;

// The span engine (s_hwr_span.h): on for the opaque textured faces
typedef struct HWR_SPAN_STATS {
    uint32_t faces;         // faces it drew
    uint32_t pixels;        // their pixels
    uint32_t waits;         // times the CPU waited for it
    uint32_t wait_clk;      // the clocks it waited (the board)
} HWR_SPAN_STATS;
extern bool HWR_SpanOn;
extern bool HWR_ZEngine;   // the engine tests the depth (the opaque faces unsorted)
extern HWR_SPAN_STATS HWR_SpanStats;
void HWR_SpanLevel(int32_t pages);

// 0: sorted only, 1: the depth buffer, 2: both, compared (s_hwr.c)
extern int HWR_ZMode;
const uint16_t *HWR_SortedFrame(void);
const uint16_t *HWR_DepthBuffer(void);

#endif
