/* Research probe for the Grand Prix start-up intros (TODO.md), not built with the port: copy it to
 * src/runtime_zprobe.c to use it (build.sh compiles src/runtime*.c). EU 1.21 offsets.
 * BB_GPPROBE=1: lists CGrandPrixSystemData objects (version +0x0c, index +0x98);
 * BB_GP_SET="VERSION INDEX": keeps writing them; BB_ENT_TRIGGER=FILE: when FILE appears, lists the
 * CGameEntity objects (model path at entity data +0x50, flags at entity +0x38, data +0x20). */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define IMAGE UINT64_C(0x800000000)
#define GUEST_MIN UINT64_C(0x1000000000)
#define GUEST_LIMIT (UINT64_C(1) << 40)
#define GP_VTABLE (IMAGE + 0x2621020)

static uint64_t objs[16];
static int nobjs;

static void scan(void) {
    FILE *m = fopen("/proc/self/maps", "r");
    char line[512];
    nobjs = 0;
    while (m && fgets(line, sizeof line, m)) {
        unsigned long long a, b;
        char perm[8];
        if (sscanf(line, "%llx-%llx %7s", &a, &b, perm) != 3 || perm[0] != 'r' || perm[1] != 'w') continue;
        if (a < GUEST_MIN || b > GUEST_LIMIT) continue;
        for (uint64_t p = a; p + 0xa0 <= b && nobjs < 16; p += 8)
            if (*(const uint64_t *)p == GP_VTABLE) objs[nobjs++] = p;
    }
    if (m) fclose(m);
}


#define ENTITY_VTABLE (IMAGE + 0x255fc30)
static unsigned long long rs[4096], re2[4096]; static int rn;
static int readable(uint64_t a, uint64_t n) { for (int i=0;i<rn;++i) if (a>=rs[i] && a+n<=re2[i]) return 1; return 0; }
static const char *cstr(uint64_t p) {
    static char b[96]; b[0]=0;
    if (!readable(p,1)) return "";
    const char *s=(const char*)p; int n=0;
    while (n<90 && readable(p+n,1) && s[n]>=0x20 && s[n]<0x7f) { b[n]=s[n]; ++n; }
    b[n]=0; return b;
}
static void entities(void) {
    FILE *m=fopen("/proc/self/maps","r"); char line[512]; rn=0;
    while (m && rn<4096 && fgets(line,sizeof line,m)) { char perm[8];
        if (sscanf(line,"%llx-%llx %7s",&rs[rn],&re2[rn],perm)==3 && perm[0]=='r') ++rn; }
    if (m) fclose(m);
    int count=0;
    for (int r=0;r<rn;++r) {
        if (rs[r]<GUEST_MIN || re2[r]>GUEST_LIMIT) continue;
        for (uint64_t p=rs[r]; p+0x48<=re2[r]; p+=8) {
            if (*(const uint64_t*)p!=ENTITY_VTABLE) continue;
            uint64_t data=*(const uint64_t*)(p+0x40);
            if (!readable(data,0x40)) continue;
            /* the name: try a char* at data+0x10, or an igName {char*,...} */
            uint64_t n1=*(const uint64_t*)(data+0x10);
            char name[96]; snprintf(name,sizeof name,"%s",cstr(n1));
            if (!name[0] && readable(n1,8)) snprintf(name,sizeof name,"%s",cstr(*(const uint64_t*)n1));
            ++count;
            (void)name;
            printf("EntProbe: %#llx model '%s' bits@38 %#llx flags %#x\n",(unsigned long long)p,
                   cstr(*(const uint64_t*)(data+0x50)),(unsigned long long)*(const uint64_t*)(p+0x38),*(const unsigned*)(data+0x20));
        }
    }
    printf("EntProbe: %d CGameEntity objects\n",count); fflush(stdout);
}
static void *probe(void *arg) {
    (void)arg;
    int set = 0, ver = 0, idx = 0;
    const char *s = getenv("BB_GP_SET");
    if (s && sscanf(s, "%d %d", &ver, &idx) == 2) set = 1;
    for (int tick = 0; tick < 2400; ++tick) {
        if (tick % 20 == 0 || !nobjs) {
            scan();
            for (int i = 0; i < nobjs; ++i) {
                const unsigned char *o = (const unsigned char *)objs[i];
                printf("GPProbe: CGrandPrixSystemData %#llx version %d index %d\n", (unsigned long long)objs[i],
                       *(const int *)(o + 0x0c), *(const int *)(o + 0x98));
            }
            if (!nobjs && tick % 20 == 0) printf("GPProbe: no CGrandPrixSystemData yet\n");
            fflush(stdout);
        }
        if (set)
            for (int i = 0; i < nobjs; ++i) {
                unsigned char *o = (unsigned char *)objs[i];
                if (*(const uint64_t *)o != GP_VTABLE) continue;
                *(int *)(o + 0x0c) = ver;
                *(int *)(o + 0x98) = idx;
            }
        { const char *t=getenv("BB_ENT_TRIGGER"); if (t && !access(t,F_OK)) { remove(t); entities(); } }
        usleep(250000);
    }
    return NULL;
}

__attribute__((constructor)) static void gpprobe_start(void) {
    if (!getenv("BB_GPPROBE")) return;
    pthread_t t;
    if (pthread_create(&t, NULL, probe, NULL) == 0) pthread_detach(t);
}
