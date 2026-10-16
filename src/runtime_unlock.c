/* SacredFire: unlock_all. The game's build setups (CBuildData objects, loaded from
 * BuildsSetup/BuildsSetupData.igz) carry an `_unlockAll` flag (+0x81) that the retail setups
 * leave off; turned on, every character, kart and customization item (all of the Pit Stop) is
 * available. The flag is only read, never saved: the save and the online inventory keep what the
 * player owns, so turning it off brings back exactly their own items. BB_UNLOCK_ALL_VTABLE (the
 * class's vtable as an image offset, found by scripts/patches.py --print-unlock-vtable) names the
 * objects: a background thread scans the game's writable memory a few times in the first minutes
 * and sets the flag to the wanted state on each one found; the in-game menu changes that state
 * at any time (runtime_unlock_all_set, applied at once). BB_UNLOCK_ALL=1: on at start. Without
 * the vtable nothing runs. */
#define _GNU_SOURCE
#include "runtime.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#define make_dir(path) mkdir(path)
#else
#define make_dir(path) mkdir(path, 0755)
#endif

#define UNLOCK_ALL_OFFSET 0x81
#define GUEST_LIMIT (UINT64_C(1) << 40) /* guest memory lies below 1 TiB */
#define GUEST_MIN UINT64_C(0x1000000000) /* the game's user memory (runtime_memory.c USER_MIN) */

static uint64_t vtable;
static int wanted = 0;     /* the state the flag is set to */
static int requested = -1; /* a change from the menu, -1 none */

static void pause_seconds(unsigned seconds) {
#ifdef _WIN32
    Sleep(seconds * 1000u);
#else
    struct timespec t = {seconds, 0};
    nanosleep(&t, NULL);
#endif
}

/* Sets the flag to `wanted` on every 8-aligned qword equal to vtable in [data, data+size). */
static unsigned mark(const unsigned char *data, size_t size, unsigned char *writable) {
    unsigned found = 0;
    for (size_t i = 0; i + 8 <= size; i += 8) {
        uint64_t value;
        memcpy(&value, data + i, 8);
        if (value != vtable) continue;
        ++found;
        const unsigned char value_wanted = (unsigned char)wanted;
        if (writable[i + UNLOCK_ALL_OFFSET] != value_wanted) writable[i + UNLOCK_ALL_OFFSET] = value_wanted;
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

/* A copy of the saves (user/savedata) before unlock everything turns on from the menu:
 * user/savedata-backups/before-unlock-<date>-<time>. Unlocking writes nothing to the save, the copy
 * is only a safety net for the player's bought and won items. */
static int copy_tree(const char *from, const char *to) {
    DIR *d = opendir(from);
    if (!d) return -1;
    make_dir(to);
    int failed = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char a[1024], b[1024];
        snprintf(a, sizeof a, "%s/%s", from, e->d_name);
        snprintf(b, sizeof b, "%s/%s", to, e->d_name);
        struct stat st;
        if (stat(a, &st)) { failed = 1; continue; }
        if (S_ISDIR(st.st_mode)) { failed |= copy_tree(a, b) != 0; continue; }
        FILE *in = fopen(a, "rb"), *out = in ? fopen(b, "wb") : NULL;
        char buffer[65536];
        size_t n;
        while (in && out && (n = fread(buffer, 1, sizeof buffer, in)) > 0)
            if (fwrite(buffer, 1, n, out) != n) { failed = 1; break; }
        if (!in || !out) failed = 1;
        if (in) fclose(in);
        if (out && fclose(out)) failed = 1;
    }
    closedir(d);
    return failed ? -1 : 0;
}

static void backup_saves(void) {
    const char *user = runtime_file_user_dir();
    char from[700], dir[700], to[760];
    snprintf(from, sizeof from, "%s/savedata", user);
    snprintf(dir, sizeof dir, "%s/savedata-backups", user);
    const time_t now = time(NULL);
    struct tm t;
#ifdef _WIN32
    localtime_s(&t, &now);
#else
    localtime_r(&now, &t);
#endif
    snprintf(to, sizeof to, "%s/before-unlock-%04d%02d%02d-%02d%02d%02d", dir, t.tm_year + 1900, t.tm_mon + 1,
             t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    make_dir(dir);
    if (copy_tree(from, to) == 0) printf("Unlock all: saves backed up to %s\n", to);
    else printf("Unlock all: could not back up all saves to %s\n", to);
}

static void *unlock_thread(void *arg) {
    (void)arg;
    /* The setups load at boot (startup archives); later scans catch a reload. A change from the
     * menu is applied within a second. */
    static const unsigned waits[] = {20, 10, 15, 30, 45, 60, 120};
    size_t next = 0;
    unsigned total = 0, waited = 0;
    for (;;) {
        pause_seconds(1);
        ++waited;
        const int change = __atomic_exchange_n(&requested, -1, __ATOMIC_ACQ_REL);
        const int scheduled = next < sizeof waits / sizeof *waits && waited >= waits[next];
        if (change < 0 && !scheduled) continue;
        if (change == 1 && !wanted) backup_saves();
        if (change >= 0) wanted = change;
        if (scheduled) { ++next; waited = 0; }
        const unsigned found = scan();
        if (change >= 0) printf("Unlock all: %s (%u build setups)\n", wanted ? "on" : "off, the player's own items", found);
        else if (found != total || next == 1) printf("Unlock all: _unlockAll %s on %u build setups\n", wanted ? "set" : "cleared", found);
        total = found;
        fflush(stdout);
    }
    return NULL;
}

/* The in-game menu: on (1) or off (0), applied at once. */
void runtime_unlock_all_set(int on) {
    if (vtable) __atomic_store_n(&requested, on ? 1 : 0, __ATOMIC_RELEASE);
}

void runtime_unlock_all_start(const unsigned char *image) {
    const char *offset = getenv("BB_UNLOCK_ALL_VTABLE");
    if (!offset || !*offset) return;
    vtable = (uint64_t)(uintptr_t)image + strtoull(offset, NULL, 0);
    const char *on = getenv("BB_UNLOCK_ALL");
    wanted = on && on[0] == '1';
    pthread_t thread;
    if (pthread_create(&thread, NULL, unlock_thread, NULL) == 0) pthread_detach(thread);
    printf("Unlock all: %s (every character, kart and Pit Stop item; the menu switches it)\n", wanted ? "on" : "off");
}
