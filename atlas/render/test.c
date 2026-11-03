// Smoke test for libpinpin_render: render two frames into an IOSurface and check the pixels change.
#include <IOSurface/IOSurface.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>
typedef void *(*NewFn)(void); typedef int (*FrameFn)(void *, void *, uint32_t, uint32_t, uint32_t, double);
static CFNumberRef num(int v) { return CFNumberCreate(NULL, kCFNumberIntType, &v); }
int main(int argc, char **argv) {
    void *h = dlopen(argv[1], RTLD_NOW); if (!h) { printf("dlopen: %s\n", dlerror()); return 1; }
    NewFn n = (NewFn)dlsym(h, "pp_render_new"); FrameFn f = (FrameFn)dlsym(h, "pp_render_frame");
    void *r = n(); if (!r) { printf("renderer init failed\n"); return 2; }
    int w = 1920, hh = 1200;
    const void *k[] = {kIOSurfaceWidth, kIOSurfaceHeight, kIOSurfaceBytesPerElement, kIOSurfacePixelFormat};
    const void *v[] = {num(w), num(hh), num(4), num('BGRA')};
    CFDictionaryRef props = CFDictionaryCreate(NULL, k, v, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    IOSurfaceRef s = IOSurfaceCreate(props);
    uint32_t px[2][4]; double ms[20]; struct timespec a, b;
    for (int i = 0; i < 20; i++) {
        clock_gettime(CLOCK_MONOTONIC, &a);
        int rc = f(r, (void *)s, w, hh, 3, i * 0.1);
        clock_gettime(CLOCK_MONOTONIC, &b);
        ms[i] = (b.tv_sec - a.tv_sec) * 1e3 + (b.tv_nsec - a.tv_nsec) / 1e6;
        if (rc) { printf("frame %d rc=%d\n", i, rc); return 3; }
        if (i == 0 || i == 19) {
            IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL);
            uint32_t *p = IOSurfaceGetBaseAddress(s); size_t st = IOSurfaceGetBytesPerRow(s) / 4;
            int j = i ? 1 : 0; px[j][0] = p[0]; px[j][1] = p[600 * st + 960]; px[j][2] = p[1199 * st + 1919]; px[j][3] = p[300 * st + 100];
            IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL);
        }
    }
    double sum = 0; for (int i = 1; i < 20; i++) sum += ms[i];
    printf("frame0 px %08x %08x %08x %08x\nframe19 px %08x %08x %08x %08x\n", px[0][0], px[0][1], px[0][2], px[0][3], px[1][0], px[1][1], px[1][2], px[1][3]);
    printf("first frame %.1f ms (wrap+pipeline), then avg %.2f ms per 1920x1200 frame\n", ms[0], sum / 19);
    int nonzero = px[1][1] != 0 || px[1][2] != 0, changed = px[0][1] != px[1][1] || px[0][3] != px[1][3];
    printf("%s\n", nonzero && changed ? "RENDER OK: pixels drawn and changing" : "RENDER FAIL");
    return nonzero && changed ? 0 : 4;
}
