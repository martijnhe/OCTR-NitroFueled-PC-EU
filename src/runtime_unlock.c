/* SacredFire: unlock_all. The game's build setups (CBuildData objects, loaded from
 * BuildsSetup/BuildsSetupData.igz) carry an `_unlockAll` flag (+0x81) that the retail setups
 * leave off; turned on, every character, kart and customization item (all of the Pit Stop) is
 * available. BB_UNLOCK_ALL_VTABLE (the class's vtable as an image offset, found by
 * scripts/patches.py --print-unlock-vtable) names the objects: a background thread scans the
 * game's writable memory a few times in the first minute and sets the flag on each one found.
 * Without the variable nothing runs. */
#define _GNU_SOURCE
#include "runtime.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define UNLOCK_ALL_OFFSET 0x81
#define GUEST_LIMIT (UINT64_C(1) << 40) /* guest memory lies below 1 TiB */
#define GUEST_MIN UINT64_C(0x1000000000) /* the game's user memory (runtime_memory.c USER_MIN) */

static uint64_t vtable;

static void pause_seconds(unsigned seconds) {
#ifdef _WIN32
    Sleep(seconds * 1000u);
#else
    struct timespec t = {seconds, 0};
    nanosleep(&t, NULL);
#endif
}

/* Sets the flag on every 8-aligned qword equal to vtable in [data, data+size). */
static unsigned mark(const unsigned char *data, size_t size, unsigned char *writable) {
    unsigned found = 0;
    for (size_t i = 0; i + 8 <= size; i += 8) {
        uint64_t value;
        memcpy(&value, data + i, 8);
        if (value != vtable) continue;
        ++found;
        if (writable[i + UNLOCK_ALL_OFFSET] != 1) writable[i + UNLOCK_ALL_OFFSET] = 1;
    }
    return found;
}

static unsigned scan(void) {
    unsigned found = 0;
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION info;
    for (uint64_t a = GUEST_MIN; a < GUEST_LIMIT && VirtualQuery((void *)(uintptr_t)a, &info, sizeof info);
         a = (uint64_t)(uintptr_t)info.BaseAddress + info.RegionSize) {
        if (info.State == MEM_COMMIT && (info.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) &&
            !(info.Protect & PAGE_GUARD))
            found += mark(info.BaseAddress, info.RegionSize, info.BaseAddress);
    }
#else
    /* The game's own memory from 0x1000000000 (the bb-guest-memory memfd, or anonymous), read in
     * place: other file and device mappings (the GPU's) are left alone, and no kernel lock is held
     * (reading through /proc/self/mem took the mmap lock page by page and stalled the game). */
    FILE *maps = fopen("/proc/self/maps", "r");
    if (!maps) return 0;
    char line[512];
    while (fgets(line, sizeof line, maps)) {
        unsigned long long lo, hi, offset, inode;
        char perms[8], device[16];
        int path = 0;
        if (sscanf(line, "%llx-%llx %7s %llx %15s %llu %n", &lo, &hi, perms, &offset, device, &inode, &path) < 6 ||
            perms[0] != 'r' || lo < GUEST_MIN || hi > GUEST_LIMIT)
            continue;
        /* Read-only pages are the port's write tracking: a write there is handled like a game write. */
        const int anonymous = !path || !line[path] || line[path] == '\n';
        if (!anonymous && !strstr(line + path, "bb-guest-memory")) continue;
        found += mark((const unsigned char *)(uintptr_t)lo, (size_t)(hi - lo), (unsigned char *)(uintptr_t)lo);
    }
    fclose(maps);
#endif
    return found;
}

static void *unlock_thread(void *arg) {
    (void)arg;
    /* The setups load at boot (startup archives); later scans catch a reload. */
    static const unsigned waits[] = {20, 10, 15, 30, 45};
    unsigned total = 0;
    for (size_t i = 0; i < sizeof waits / sizeof *waits; ++i) {
        pause_seconds(waits[i]);
        unsigned found = scan();
        if (found != total || i == 0) printf("Unlock all: _unlockAll set on %u build setups\n", found);
        total = found;
    }
    return NULL;
}

void runtime_unlock_all_start(const unsigned char *image) {
    const char *offset = getenv("BB_UNLOCK_ALL_VTABLE");
    if (!offset || !*offset) return;
    vtable = (uint64_t)(uintptr_t)image + strtoull(offset, NULL, 0);
    pthread_t thread;
    if (pthread_create(&thread, NULL, unlock_thread, NULL) == 0) pthread_detach(thread);
    printf("Unlock all: on (every character, kart and Pit Stop item)\n");
}
