/* SacredFire: the title screen and the kart customization screen in ultrawide. Both are 3D sets
 * whose checkered backdrop is only as wide as a 16:9 view; the game widens the horizontal field of
 * view with the aspect (Hor+), so at 21:9 the camera sees past the backdrop's edges (sky, black).
 * On these screens the camera gets the horizontal field of view a 16:9 screen would have (the
 * same width of the set, less of its top and bottom); the menus are drawn separately and keep
 * their ultrawide layout. (More screens can be added to screens[]: BB_FRONTEND_TRACE.) Races, cutscenes and other screens keep the game's own field of view.
 *
 * The camera's field of view (degrees) is written each frame by one instruction,
 * vmulss xmm0, xmm0, [rip+57.29578] (radians to degrees) followed by the store to the render
 * camera's +0x3c. The runtime points that instruction at a float of its own (in the unused tail of
 * the image's last data page), which holds 57.29578 normally and a smaller factor on the screen.
 * Screens are recognised by the main render camera resting at their set's camera position
 * (the main render pass, CPreRenderUpdatePass with _cameraName "main", has the camera at +0x168).
 *
 * BB_FRONTEND_FOV="INSTRUCTION PASS_VTABLE SLOT" (image offsets; scripts/patches.py
 * --print-frontend) turns it on. BB_FRONTEND_TRACE=1 logs where the camera comes to rest (to add a
 * screen). Linux only (the render pass is found through /proc/self/maps). */
#define _GNU_SOURCE /* usleep */
#include "runtime.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#define GUEST_MIN UINT64_C(0x1000000000)
#define GUEST_LIMIT (UINT64_C(1) << 40)
#define DEGREES 57.29578f
/* CPreRenderUpdatePass fields (reflection) and the render camera's layout. */
#define PASS_CAMERA_NAME 0x138
#define PASS_RENDER_CAMERA 0x168
#define CAMERA_FOV 0x3c      /* horizontal field of view, degrees */
#define CAMERA_ASPECT 0x40   /* width / height */
#define CAMERA_POSITION 0xa0 /* three floats */

struct screen {
    const char *name;
    float x, y, z; /* the set's camera position */
};
static const struct screen screens[] = {
    {"title screen", 5369.079f, -145.722f, -929.984f},
    {"kart customization", -104.853f, -57.029f, -914.018f},
};
#define SCREEN_COUNT (int)(sizeof(screens) / sizeof(screens[0]))

static uint64_t pass_vtable;
static volatile float *slot; /* the factor the field-of-view instruction multiplies by */
static int trace;

/* Readable regions of the process, so a stray vtable match is never followed into unmapped
 * memory. */
#define MAX_REGIONS 4096
static unsigned long long region_start[MAX_REGIONS], region_end[MAX_REGIONS];
static int region_count;

static int readable(uint64_t address, uint64_t size) {
    for (int i = 0; i < region_count; ++i)
        if (address >= region_start[i] && address + size <= region_end[i]) return 1;
    return 0;
}

static void read_regions(void) {
    FILE *maps = fopen("/proc/self/maps", "r");
    char line[512];
    region_count = 0;
    while (maps && region_count < MAX_REGIONS && fgets(line, sizeof line, maps)) {
        char perm[8];
        if (sscanf(line, "%llx-%llx %7s", &region_start[region_count], &region_end[region_count], perm) == 3 &&
            perm[0] == 'r')
            ++region_count;
    }
    if (maps) fclose(maps);
}

/* The main render pass: created once, at start; scanned for until found. */
static uint64_t find_main_pass(void) {
    read_regions();
    for (int r = 0; r < region_count; ++r) {
        const uint64_t a = region_start[r], b = region_end[r];
        if (a < GUEST_MIN || b > GUEST_LIMIT) continue;
        for (uint64_t p = a; p + PASS_RENDER_CAMERA + 8 <= b; p += 8) {
            if (*(const uint64_t *)(uintptr_t)p != pass_vtable) continue;
            const uint64_t name = *(const uint64_t *)(uintptr_t)(p + PASS_CAMERA_NAME);
            const uint64_t camera = *(const uint64_t *)(uintptr_t)(p + PASS_RENDER_CAMERA);
            if (readable(name, 5) && !memcmp((const void *)(uintptr_t)name, "main", 5) && readable(camera, 0x100))
                return p;
        }
    }
    return 0;
}

static int at(const float *pos, const struct screen *s) {
    return fabsf(pos[0] - s->x) < 1.0f && fabsf(pos[1] - s->y) < 1.0f && fabsf(pos[2] - s->z) < 1.0f;
}

static void *frontend_thread(void *unused) {
    (void)unused;
    uint64_t pass = 0;
    while (!pass) {
        sleep(2);
        pass = find_main_pass();
    }
    printf("Frontend field of view: main render pass found\n");
    fflush(stdout);
    int current = -1; /* the screen the factor is set for, -1 none */
    float rest[3] = {0}, last[3] = {0};
    int still = 0;
    for (;;) {
        usleep(4000);
        if (*(const uint64_t *)(uintptr_t)pass != pass_vtable) continue;
        const uint64_t camera = *(const uint64_t *)(uintptr_t)(pass + PASS_RENDER_CAMERA);
        if (camera < GUEST_MIN || camera >= GUEST_LIMIT || camera % 8) continue;
        const float *c = (const float *)(uintptr_t)camera;
        float pos[3];
        memcpy(pos, (const void *)(c + CAMERA_POSITION / 4), sizeof pos);
        int screen = -1;
        for (int i = 0; i < SCREEN_COUNT && screen < 0; ++i)
            if (at(pos, &screens[i])) screen = i;
        if (screen != current) {
            if (screen < 0) {
                *slot = DEGREES;
            } else {
                /* The game's own field of view: the factor is still the plain one here. */
                const float fov = current < 0 ? c[CAMERA_FOV / 4] : c[CAMERA_FOV / 4] * DEGREES / *slot;
                const float aspect = c[CAMERA_ASPECT / 4];
                if (aspect > 16.0f / 9.0f + 0.01f && fov > 1.0f && fov < 170.0f) {
                    const float half = fov * 0.5f / DEGREES;
                    const float target = 2.0f * atanf(tanf(half) * (16.0f / 9.0f) / aspect) * DEGREES;
                    *slot = DEGREES * target / fov;
                    printf("Frontend field of view: %s, %.3f -> %.3f degrees\n", screens[screen].name, fov, target);
                    fflush(stdout);
                }
            }
            current = screen;
        }
        if (trace) { /* where the camera comes to rest, once per rest */
            const int moved = fabsf(pos[0] - last[0]) + fabsf(pos[1] - last[1]) + fabsf(pos[2] - last[2]) > 0.01f;
            memcpy(last, pos, sizeof last);
            if (moved) still = 0;
            else if (++still == 250 && memcmp(rest, pos, sizeof pos)) {
                memcpy(rest, pos, sizeof rest);
                printf("Frontend field of view: camera at rest at %.3f %.3f %.3f, %.3f degrees\n", pos[0], pos[1],
                       pos[2], c[CAMERA_FOV / 4]);
                fflush(stdout);
            }
        }
    }
    return NULL;
}

/* Called before the game starts: points the field-of-view instruction at the slot. */
void runtime_frontend_start(const unsigned char *image) {
    const char *on = getenv("BB_FRONTEND_FOV");
    unsigned long long instruction = 0, vtable = 0, slot_offset = 0;
    if (!on || sscanf(on, "%llx %llx %llx", &instruction, &vtable, &slot_offset) != 3) return;
    unsigned char *insn = (unsigned char *)image + instruction;
    /* vmulss xmm0, xmm0, [rip+disp32] reading 57.29578 */
    if (insn[0] != 0xc5 || insn[1] != 0xfa || insn[2] != 0x59 || insn[3] != 0x05) {
        printf("Frontend field of view: unexpected instruction at %#llx; off\n", instruction);
        return;
    }
    int32_t disp;
    memcpy(&disp, insn + 4, 4);
    float constant;
    memcpy(&constant, insn + 8 + disp, 4);
    if (fabsf(constant - DEGREES) > 0.001f) {
        printf("Frontend field of view: the instruction reads %f, not 57.29578; off\n", constant);
        return;
    }
    const int64_t new_disp = (int64_t)slot_offset - (int64_t)(instruction + 8);
    if (new_disp != (int32_t)new_disp) return;
    slot = (volatile float *)(image + slot_offset);
    *slot = DEGREES;
    const long page = sysconf(_SC_PAGESIZE);
    unsigned char *first = (unsigned char *)((uintptr_t)insn & ~(uintptr_t)(page - 1));
    const size_t length = (size_t)(insn + 8 - first);
    if (mprotect(first, length, PROT_READ | PROT_WRITE | PROT_EXEC)) {
        printf("Frontend field of view: could not patch the instruction; off\n");
        return;
    }
    const int32_t d = (int32_t)new_disp;
    memcpy(insn + 4, &d, 4);
    mprotect(first, length, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)insn, (char *)insn + 8);
    pass_vtable = (uint64_t)(uintptr_t)image + vtable;
    const char *t = getenv("BB_FRONTEND_TRACE");
    trace = t && t[0] == '1';
    pthread_t thread;
    if (pthread_create(&thread, NULL, frontend_thread, NULL) == 0) pthread_detach(thread);
    printf("Frontend field of view: the title and customization screens fill the width\n");
}
#else
void runtime_frontend_start(const unsigned char *image) { (void)image; }
#endif
