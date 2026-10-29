"""Render the actual RGB555 UI on the host, with deterministic device state.

Requires clang and Pillow. Screens and temporary build files go to /tmp.
This checks drawing/layout; camera hardware and input still require a DSi.
"""
import os
from pathlib import Path
import subprocess
import tempfile
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[2]
OUT = Path('/tmp/dsi-camera-ui-dark')
OUT.mkdir(exist_ok=True)
source = (ROOT / 'arm9/source/main.c').read_text()


def section(start, end):
    return source[source.index(start):source.index(end, source.index(start))]


preamble = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "ui_font.h"
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int Camera;
#define KEY_A (1 << 0)
#define KEY_B (1 << 1)
#define KEY_LID (1 << 12)
#define KEY_TOUCH (1 << 14)
#define KEY_SELECT (1 << 2)
#define KEY_L (1 << 9)
#define KEY_X (1 << 10)
#define KEY_Y (1 << 11)
#define CAM_INNER 0
#define CAM_OUTER 1
#define BIT(n) (1u << (n))
#define RGB15(r,g,b) ((r) | ((g) << 5) | ((b) << 10))
#define cameraStreamOverruns() UINT32_MAX
#define SCREEN_H 192
#define THUMB_W 64
#define THUMB_H 48
#define GRID_COLS 4
#define HEADER_H 23
#define ROW_H 50
#define SECTION_GAP 7
static u16 canvas[256 * 192];
int clamp(int n, int a, int b) { return n < a ? a : n > b ? b : n; }
typedef struct { int px, py; } touchPosition;
typedef struct { u16 pressed, held; int x, y; bool reset; } InputEvent;
static const InputEvent *events;
static size_t eventCount, eventIndex;
static InputEvent currentEvent;
static void input(const InputEvent *script, size_t count) {
    events = script; eventCount = count; eventIndex = 0;
    currentEvent = (InputEvent){0};
}
static void swiWaitForVBlank(void) {}
static void scanKeys(void) {
    assert(eventIndex < eventCount);
    currentEvent = events[eventIndex++];
}
static u16 keysDown(void) { return currentEvent.pressed; }
static u16 keysHeld(void) { return currentEvent.held; }
static bool pmShouldReset(void) { return currentEvent.reset; }
static void touchRead(touchPosition *touch) {
    touch->px = currentEvent.x; touch->py = currentEvent.y;
}
static void capture(const char *name) {
    char path[256]; snprintf(path, sizeof(path), "%s/%s.ppm", getenv("DSI_UI_OUT"), name);
    FILE *f = fopen(path, "wb"); if(!f) exit(1);
    fprintf(f, "P6\n256 192\n255\n");
    for(int i = 0; i < 256 * 192; i++) {
        unsigned char rgb[3] = {(canvas[i] & 31)*255/31, ((canvas[i] >> 5)&31)*255/31, ((canvas[i] >> 10)&31)*255/31};
        fwrite(rgb, 3, 1, f);
    }
    fclose(f);
}
'''
code = preamble
code += section('static u16 *s_subGfx;', 'static void uiStatus(')
code += section('static u16 uiCameraTap(', '// The viewfinder is a continuous')
code += section('typedef struct Photo {', 'static int cmpPhoto')
code += section('static int dayKey(', '// Async save pipeline')
code += section('static const u8 s_font', '// --- Timestamp watermark:')
code += section('// --- Bottom-screen UI,', 'static void uiExposureFailure')
code += section('// Waits for A (true)', '// Loads (or builds')
code += section('static bool uiConfirm(', '// Moves the selection')
code += r'''
static void expectConfirm(const InputEvent *script, size_t count, bool result) {
    input(script, count);
    assert(confirmAB() == result);
    assert(eventIndex == count);
}
static void testInput(void) {
    const InputEvent hardwareDelete[] = {{.pressed=KEY_A}};
    const InputEvent hardwareCancel[] = {{.pressed=KEY_B}};
    const InputEvent reset[] = {{.reset=true}};
    expectConfirm(hardwareDelete, 1, true);
    expectConfirm(hardwareCancel, 1, false);
    expectConfirm(reset, 1, false);
    // Exercise the actual dialog loop across every pixel, including edges and the dead gap.
    for(int y = 0; y < SCREEN_H; y++) {
        for(int x = 0; x < 256; x++) {
            const InputEvent script[] = {
                {.pressed=KEY_TOUCH, .held=KEY_TOUCH, .x=x, .y=y},
                {.pressed=KEY_B}
            };
            bool insideRow = y >= 108 && y <= 129;
            bool deletes = insideRow && x >= 133 && x <= 230;
            bool cancels = insideRow && x >= 25 && x <= 118;
            expectConfirm(script, deletes || cancels ? 1 : 2, deletes);
        }
    }
    // A held initiating touch is not a second tap; a fresh tap or B cancels the burst.
    const InputEvent burst[] = {
        {.held=KEY_TOUCH}, {0}, {.pressed=KEY_TOUCH, .held=KEY_TOUCH},
        {.pressed=KEY_B}, {.held=KEY_LID}, {.reset=true}
    };
    input(burst, sizeof(burst)/sizeof(burst[0]));
    assert(!captureCancelled()); assert(!captureCancelled());
    assert(captureCancelled()); assert(captureCancelled());
    assert(captureCancelled()); assert(captureCancelled());
}
static void expectStatus(const char *message, const char *first, const char *second) {
    u16 actual[256 * SCREEN_H];
    uiHeader(""); uiStatus(message); memcpy(actual, canvas, sizeof(actual));
    uiHeader("");
    if(first[0]) uiSmall((256 - uiTextWidth(first, &ui_font_10))/2, 164, first, UI_DIM);
    uiSmall((256 - uiTextWidth(second, &ui_font_10))/2, 179, second, UI_DIM);
    assert(memcmp(actual, canvas, sizeof(actual)) == 0);
}
int main(void) {
    s_subGfx = canvas; s_camera = CAM_OUTER;
    testInput();
    expectStatus("saved", "", "saved");
    expectStatus("VID_0001 10.0FPS D:120 A:0 O:1000 G:12.4s",
                 "VID_0001 10.0FPS D:120 A:0 O:1000", "G:12.4s");
    expectStatus("VID_2147483647 429496729.5FPS G:999.9s D:4294967295 A:4294967295 O:4294967295",
                 "VID_2147483647 429496729.5FPS G:999.9s",
                 "D:4294967295 A:4294967295 O:4294967295");
    {
        int num = 2147483647;
        u32 fps10 = UINT32_MAX, maxGapMs = 999999, ovStart = 0;
        u32 framesDropped = UINT32_MAX, s_audDropped = UINT32_MAX;
        /* VIDEO_SUMMARY */
        assert(strcmp(s_savedMsg,
            "VID_2147483647 429496729.5FPS G:999.9s D:4294967295 A:4294967295 O:4294967295") == 0);
    }
    assert(advancedMoveSelection(ADV_SHUTTER, 1) == ADV_WHITE_BALANCE);
    assert(uiCameraTap(128, 104, true) == KEY_L);
    assert(uiCameraTap(128, 104, false) == 0);
    assert(uiCameraTap(43, 156, true) == KEY_SELECT);
    assert(uiCameraTap(213, 156, true) == KEY_X);
    assert(uiCameraTap(195, 20, false) == KEY_A);
    assert(uiCameraTap(90, 20, true) == KEY_Y);
    assert(uiCameraTap(35, 20, true) == 0);
    assert(uiCameraTap(0, 191, true) == 0);
    uiCameraScreen(CAM_OUTER, true); capture("camera");
    s_videoMode = true;
    assert(uiCameraTap(35, 20, true) == KEY_Y);
    assert(uiCameraTap(90, 20, true) == 0);
    uiCameraScreen(CAM_INNER, true); capture("video");
    uiRecordingScreen(); uiStatus("0:12"); capture("recording");
    uiCameraScreen(CAM_OUTER, true);
    uiStatus("VID_0001 10.0FPS G:12.4s D:120 A:0 O:1000"); capture("diagnostics");
    uiAdvancedScreen(ADV_TIMESTAMP); capture("video-controls");
    s_videoMode = false;
    uiCameraScreen(CAM_OUTER, false); capture("no-sd");
    uiAdvancedScreen(ADV_SHUTTER); capture("controls-auto");
    s_shutter = 11; s_gain = 3; s_warmth = -3; s_nightBurst = true; s_timestamp = true;
    uiAdvancedScreen(ADV_SHUTTER); capture("controls-manual");
    for(int row = 0; row < ADV_COUNT; row++) {
        uiAdvancedScreen(row);
        char name[32]; snprintf(name, sizeof(name), "control-%d", row); capture(name);
    }
    s_camera = CAM_INNER; uiAdvancedScreen(ADV_NIGHT); capture("inner-controls");
    {
        Photo ph[24] = {0}; bool marked[24] = {false};
        int sel = 23, count = 24, index = 23; int *idx = &index;
        ph[23] = (Photo){.num=24, .ts=1793197800};
        { /* ALBUM_INFO */ }
        capture("album");
        const InputEvent cancel[] = {{.pressed=KEY_B}};
        input(cancel, 1);
        assert(!uiConfirm("Delete 24 selected items?")); capture("delete");
        { /* PHOTO_INFO */ }
        capture("photo");
    }
    Photo ph[12]; u16 thumbs[12 * THUMB_W * THUMB_H]; int px[12], py[12]; bool marked[12] = {true};
    for(int i = 0; i < 12; i++) {
        ph[i] = (Photo){.num=i+1, .ts=1793188800 + (i/8)*86400, .isVideo=i==3, .durSec=42};
        for(int j = 0; j < THUMB_W*THUMB_H; j++)
            thumbs[i*THUMB_W*THUMB_H+j] = BIT(15) | RGB15(16+i, 21+i/2, 25);
    }
    Section *sec; int ns; int height = buildLayout(ph, 12, px, py, &sec, &ns);
    for(int scroll = 0; scroll <= height; scroll++)
        composeAlbum(canvas, 12, ph, thumbs, sec, ns, px, py, scroll, 1, marked);
    composeAlbum(canvas, 12, ph, thumbs, sec, ns, px, py, 0, 1, marked); capture("gallery");
    free(sec);
    uiHeader("Camera"); uiTextCenter(44, "a deliberately long centered string that runs off both sides of the screen", UI_FG);
    uiRect(-50, -50, 400, 400, UI_BG);
    uiStatus("VID_0001 10.0FPS D:123 A:456 O:789 G:999.9s");
    return 0;
}
'''
# Use the in-app composition blocks as well as its drawing primitives.
code = code.replace('/* ALBUM_INFO */', section('if(infoDirty) {', 'infoDirty = false;').split('{', 1)[1])
code = code.replace('/* PHOTO_INFO */', section('char when[40], line[40];', 'dirty = false;'))
code = code.replace('/* VIDEO_SUMMARY */', section('u32 gapS = maxGapMs / 1000;', '// Fallback numbering').rsplit('}', 1)[0])
with tempfile.TemporaryDirectory(prefix='dsi-ui-', dir='/tmp') as tmp:
    c = Path(tmp) / 'render.c'
    c.write_text(code)
    binary = Path(tmp) / 'render'
    subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-deprecated-declarations', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=all', '-I', str(ROOT / 'arm9/include'), str(c), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], env={**os.environ, 'DSI_UI_OUT': str(OUT), 'TZ': 'UTC'}, check=True)
for ppm in OUT.glob('*.ppm'):
    with Image.open(ppm) as im:
        im.save(ppm.with_suffix('.png'))
    ppm.unlink()
# Nearest-neighbor enlargement preserves the exact device pixels.
shots = [('camera', 'Camera'), ('controls-manual', 'Controls'), ('album', 'Album'),
         ('recording', 'Recording'), ('video-controls', 'Video controls'), ('delete', 'Delete confirmation')]
board = Image.new('RGB', (256*3+16*4, (192+28)*2+16*3), '#101010')
draw = ImageDraw.Draw(board)
for i, (name, label) in enumerate(shots):
    x = 16+(i%3)*272; y = 16+(i//3)*236
    draw.text((x, y), label, fill='#b0b0b0')
    with Image.open(OUT / (name+'.png')) as im:
        board.paste(im, (x,y+20))
board.resize((board.width*2,board.height*2), Image.Resampling.NEAREST).save(OUT/'overview.png')
print(f'Rendered UI states, confirmation input, burst cancellation, full diagnostics, clipping and album scroll checks passed: {OUT}')
