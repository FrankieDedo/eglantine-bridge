// Stand-in for libnx, only what ui.c / ui_list.c use, so the menus can be
// drawn on a PC (tools/ui_preview). Fonts come from Windows instead of the
// Switch's shared fonts; the button glyphs therefore show as boxes here.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef uint64_t u64;
typedef uint32_t u32;
typedef uint8_t u8;
typedef int32_t Result;
#define R_FAILED(r) ((r) != 0)

typedef struct {
    void* address;
    size_t size;
} PlFontData;
enum { PlServiceType_User = 0 };
enum { PlSharedFontType_Standard = 0, PlSharedFontType_NintendoExt = 1 };

static inline Result plInitialize(int t) { (void)t; return 0; }
static inline void plExit(void) {}
static inline Result plGetSharedFontByType(PlFontData* f, int type) {
    const char* path = type == PlSharedFontType_Standard ? "C:/Windows/Fonts/segoeui.ttf"
                                                         : "C:/Windows/Fonts/seguisym.ttf";
    FILE* fp = fopen(path, "rb");
    if (!fp) return 1;
    fseek(fp, 0, SEEK_END);
    f->size = (size_t)ftell(fp);
    fseek(fp, 0, SEEK_SET);
    f->address = malloc(f->size);
    fread(f->address, 1, f->size, fp);
    fclose(fp);
    return 0;
}

typedef struct {
    u64 down;
} PadState;
static inline void padUpdate(PadState* p) { (void)p; }
static inline u64 padGetButtonsDown(PadState* p) { return p->down; }
static inline bool appletMainLoop(void) { return true; }
enum { AppletFocusState_InFocus = 1 };
static inline int appletGetFocusState(void) { return AppletFocusState_InFocus; }
static inline void svcSleepThread(long long ns) { (void)ns; }

typedef struct {
    int count;
    struct {
        u32 x, y;
    } touches[16];
} HidTouchScreenState;
static inline void hidGetTouchScreenStates(HidTouchScreenState* s, int n) { (void)n; s->count = 0; }

enum {
    HidNpadButton_A = 1 << 0,
    HidNpadButton_B = 1 << 1,
    HidNpadButton_L = 1 << 6,
    HidNpadButton_R = 1 << 7,
    HidNpadButton_AnyLeft = 1 << 12,
    HidNpadButton_AnyUp = 1 << 13,
    HidNpadButton_AnyRight = 1 << 14,
    HidNpadButton_AnyDown = 1 << 15,
};
