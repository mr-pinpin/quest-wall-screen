// pinpin-displays: real extra macOS displays for the headset's summoned screens.
// Each screen the user summons in the headset becomes a virtual display on this Mac (CGVirtualDisplay, the API
// DeskPad and BetterDisplay use): macOS treats it as a monitor, so windows can be dragged onto it and apps can go
// full screen in it. Displays exist while this process runs; the set is saved and recreated on start.
//
// HTTP on the LAN (the headset calls it directly):
//   GET  /displays                            -> [{"screen":2,"display":5,"w":3840,"h":2400,"scale":2}, ...]
//                                                 (w/h are backing pixels; macOS lays out w/scale x h/scale points)
//   POST /displays?id=2&w=1920&h=1200&scale=2 -> create (or resize) the display for screen 2; w/h in points,
//                                                 scale 2 = HiDPI (Retina-sharp text), 1 = plain
//   POST /remove?id=2                   -> remove it (its windows move back, like unplugging a monitor)
//   POST /place?app=com.google.Chrome&screen=2 -> move that app's focused window to fill the screen's display
// Only loopback and the house network (192.168.0.0/16) may call it.
//
// Mouse on UDP :57711 (one datagram per event, so pointing stays low latency):
//   "m <screen> <u> <v>"  move the pointer there   u, v in 0..10000 across that display (0,0 = top left)
//   "c <screen> <u> <v>"  click there              (down, up 40 ms later)
//   "w <screen> <u> <v> <dy> <dx>"  scroll at u,v  dy/dx in wheel units (~120 = one notch), posted as pixel
//                                                   scroll; dy>0 reveals content below, dx>0 scrolls right.
//                                                   Used by the headset's wave-to-scroll and the assistant's
//                                                   computer-use scroll (both share this one path).
// Sunshine refuses clicks outside the display it streams, so clicks on extra screens are posted here, which
// needs Accessibility. It ships as "PinPin Displays.app" because macOS keeps that grant reliably for an app
// bundle (it would not for a bare ad-hoc binary). Keys need nothing here: they reach the focused window.
//
// Still wallpaper (flag: the file ~/.local/share/pinpin-displays/still-wallpaper exists): each display this helper
// creates gets a plain dark image as its desktop picture. macOS's aerial (video) wallpapers animate every "idle"
// virtual desktop, so a screen nobody touches still costs a full capture and encode on every frame (work-09,
// 2026-11-03: 1-2% identical frames). The user's own displays are never touched.
//
// Session watch (flag: the file ~/.local/share/pinpin-displays/session-watch exists, read live on every tick, so it
// is turned on or off without a restart; rules in displays_watch.h, which sits next to this file): every 10 s the helper compares its displays with the online display list. A display gone
// for 3 checks (WindowServer restarted, 2026-11-05 17:45 UTC) is re-created from its saved size, at most once per
// 5 min per screen (K24: no churn); when re-creating cannot work (no online display at all, or re-creates that do not
// stick) it exits so PinPin Host starts a fresh helper in the live session. It also exits when it started before the
// running WindowServer (its login session is gone), so an orphan never keeps :57710 and a stale list. Losing the Host
// parent alone is not a reason: a Host restart leaves its plugins running for the next Host to adopt. GET /displays reports "online" per display (flag or not), and
// GET /health the watch's state.
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ApplicationServices/ApplicationServices.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <libproc.h>
#include <sys/sysctl.h>
#include "displays_watch.h"

@interface CGVirtualDisplayDescriptor : NSObject
@property(retain, nonatomic) dispatch_queue_t queue;
@property(retain, nonatomic) NSString *name;
@property(nonatomic) unsigned int maxPixelsHigh, maxPixelsWide;
@property(nonatomic) CGSize sizeInMillimeters;
@property(nonatomic) unsigned int productID, vendorID, serialNum;
@property(copy, nonatomic) void (^terminationHandler)(id, id);
@end
@interface CGVirtualDisplayMode : NSObject
- (instancetype)initWithWidth:(unsigned int)width height:(unsigned int)height refreshRate:(double)rate;
@end
@interface CGVirtualDisplaySettings : NSObject
@property(retain, nonatomic) NSArray *modes;
@property(nonatomic) unsigned int hiDPI;
@end
@interface CGVirtualDisplay : NSObject
- (instancetype)initWithDescriptor:(CGVirtualDisplayDescriptor *)descriptor;
- (BOOL)applySettings:(CGVirtualDisplaySettings *)settings;
@property(readonly, nonatomic) CGDirectDisplayID displayID;
@end

static const int kPort = 57710, kMousePort = 57711;
static NSMutableDictionary<NSNumber *, CGVirtualDisplay *> *displays;
static NSMutableDictionary<NSNumber *, NSArray<NSNumber *> *> *sizes;  // points w, h, scale
static NSString *statePath, *dataDir;
static void checkDisplays(void);
static BOOL watchOn;                 // flag session-watch
static dw_entry watch[65];           // by screen id
static int emptyChecks, recreatedTotal;
static long myStart;

// A small solid dark-grey PNG, written once; macOS scales it to fill the display.
static NSURL *stillImage(void) {
    NSString *path = [dataDir stringByAppendingPathComponent:@"still.png"];
    if (![[NSFileManager defaultManager] fileExistsAtPath:path]) {
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGContextRef c = CGBitmapContextCreate(NULL, 64, 64, 8, 0, cs, kCGImageAlphaPremultipliedLast);
        CGContextSetRGBFillColor(c, 0.125, 0.129, 0.141, 1); CGContextFillRect(c, CGRectMake(0, 0, 64, 64));
        CGImageRef img = CGBitmapContextCreateImage(c);
        CGImageDestinationRef dst = CGImageDestinationCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path], CFSTR("public.png"), 1, NULL);
        if (dst) { CGImageDestinationAddImage(dst, img, NULL); CGImageDestinationFinalize(dst); CFRelease(dst); }
        CGImageRelease(img); CGContextRelease(c); CGColorSpaceRelease(cs);
    }
    return [NSURL fileURLWithPath:path];
}
static void stillWallpaper(CGDirectDisplayID did, int sid) {
    if (![[NSFileManager defaultManager] fileExistsAtPath:[dataDir stringByAppendingPathComponent:@"still-wallpaper"]]) return;
    // The NSScreen for a new display appears a moment after it is created.
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        for (NSScreen *scr in NSScreen.screens) {
            if ([scr.deviceDescription[@"NSScreenNumber"] unsignedIntValue] != did) continue;
            NSError *err = nil;
            BOOL ok = [[NSWorkspace sharedWorkspace] setDesktopImageURL:stillImage() forScreen:scr
                options:@{NSWorkspaceDesktopImageScalingKey: @(NSImageScaleAxesIndependently), NSWorkspaceDesktopImageFillColorKey: [NSColor colorWithSRGBRed:0.125 green:0.129 blue:0.141 alpha:1]} error:&err];
            NSLog(@"screen %d: still wallpaper %@", sid, ok ? @"set" : err.localizedDescription);
            return;
        }
        NSLog(@"screen %d: no NSScreen for display %u yet, wallpaper unchanged", sid, did);
    });
}

static void save(void) {
    NSMutableArray *a = [NSMutableArray array];
    for (NSNumber *k in sizes) [a addObject:@{@"id": k, @"w": sizes[k][0], @"h": sizes[k][1], @"scale": sizes[k][2]}];
    [[NSJSONSerialization dataWithJSONObject:a options:0 error:nil] writeToFile:statePath atomically:YES];
}
static CGDirectDisplayID create(int sid, unsigned w, unsigned h, unsigned scale) {
    [displays removeObjectForKey:@(sid)];  // resize = recreate
    CGVirtualDisplayDescriptor *d = [CGVirtualDisplayDescriptor new];
    d.queue = dispatch_get_main_queue();
    d.name = [NSString stringWithFormat:@"PinPin Screen %d", sid];
    d.maxPixelsWide = w * scale; d.maxPixelsHigh = h * scale;
    d.sizeInMillimeters = CGSizeMake(w * 0.25, h * 0.25);  // ~100 points per inch: macOS picks sensible text sizes
    d.vendorID = 0x5050; d.productID = 0x5300 + sid; d.serialNum = 0x1000 + sid;
    // Terminated (WindowServer gone, 2026-11-05 17:45 UTC): with the watch on, check now instead of listing it as alive.
    // A resize also terminates the old display; the check then finds the new one online and does nothing.
    d.terminationHandler = ^(id a, id b) {
        NSLog(@"display for screen %d terminated", sid);
        if (watchOn) dispatch_async(dispatch_get_main_queue(), ^{
            if (watch[sid].missing < DW_DEFAULT.need - 1) watch[sid].missing = DW_DEFAULT.need - 1;
            checkDisplays();
        });
    };
    CGVirtualDisplay *v = [[CGVirtualDisplay alloc] initWithDescriptor:d];
    if (!v) { NSLog(@"create failed for screen %d", sid); return 0; }
    CGVirtualDisplaySettings *s = [CGVirtualDisplaySettings new];
    s.hiDPI = scale > 1 ? 1 : 0;
    // With hiDPI on, each mode is also offered at 2x (same points, double pixels) when maxPixels leaves room for it.
    s.modes = @[[[CGVirtualDisplayMode alloc] initWithWidth:w height:h refreshRate:60]];
    if (![v applySettings:s]) { NSLog(@"applySettings failed for screen %d", sid); return 0; }
    if (scale > 1) {  // pick the w x h point mode backed by w*scale x h*scale pixels
        NSDictionary *opts = @{(__bridge NSString *)kCGDisplayShowDuplicateLowResolutionModes: @YES};
        CFArrayRef all = CGDisplayCopyAllDisplayModes(v.displayID, (__bridge CFDictionaryRef)opts);
        for (CFIndex i = 0; all && i < CFArrayGetCount(all); i++) {
            CGDisplayModeRef m = (CGDisplayModeRef)CFArrayGetValueAtIndex(all, i);
            if (CGDisplayModeGetWidth(m) == w && CGDisplayModeGetPixelWidth(m) == w * scale) {
                CGDisplayConfigRef cfg; CGBeginDisplayConfiguration(&cfg);
                CGConfigureDisplayWithDisplayMode(cfg, v.displayID, m, NULL);
                CGCompleteDisplayConfiguration(cfg, kCGConfigurePermanently);
                break;
            }
        }
        if (all) CFRelease(all);
    }
    displays[@(sid)] = v; sizes[@(sid)] = @[@(w), @(h), @(scale)];
    watch[sid].sid = sid; watch[sid].did = v.displayID; watch[sid].missing = 0;  // re-create history is kept
    NSLog(@"screen %d -> display %u (%ux%u points @%ux)", sid, v.displayID, w, h, scale);
    stillWallpaper(v.displayID, sid);
    return v.displayID;
}
// The online displays (the built-in included); -1 when the call fails (no WindowServer connection).
static int onlineList(CGDirectDisplayID *ids, uint32_t max) {
    uint32_t n = 0;
    return CGGetOnlineDisplayList(max, ids, &n) == kCGErrorSuccess ? (int)n : -1;
}
static NSString *listJSON(void) {
    NSMutableArray *a = [NSMutableArray array];
    CGDirectDisplayID on[64]; int n = onlineList(on, 64);
    for (NSNumber *k in [displays.allKeys sortedArrayUsingSelector:@selector(compare:)])
        [a addObject:@{@"screen": k, @"display": @(displays[k].displayID), @"w": @([sizes[k][0] intValue] * [sizes[k][2] intValue]),
                       @"h": @([sizes[k][1] intValue] * [sizes[k][2] intValue]), @"scale": sizes[k][2],
                       @"online": @(dw_online(displays[k].displayID, on, n))}];
    return [[NSString alloc] initWithData:[NSJSONSerialization dataWithJSONObject:a options:0 error:nil] encoding:NSUTF8StringEncoding];
}
// Session watch, every 10 s on the main queue (flag session-watch). Exits rather than guess when this process can no
// longer reach the session: its owner (PinPin Host) restarts it from displays.json in the live one.
// Start time (epoch s) and name of any process, 0 if unknown. sysctl, as ps reads them: proc_pidinfo cannot read a
// root-owned process (WindowServer) from a user process (measured on the Air 2026-11-06: 0).
static long procStart(pid_t pid, char *name, size_t nn) {
    struct kinfo_proc kp; size_t len = sizeof kp; int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, pid};
    if (sysctl(mib, 4, &kp, &len, NULL, 0) != 0 || len != sizeof kp) return 0;
    if (name) { strncpy(name, kp.kp_proc.p_comm, nn - 1); name[nn - 1] = 0; }
    return (long)kp.kp_proc.p_starttime.tv_sec;
}
static long windowServerStart(void) {
    pid_t pids[4096]; int n = proc_listallpids(pids, sizeof pids);
    for (int i = 0; i < n; i++) {
        char name[64] = {0}; long t = procStart(pids[i], name, sizeof name);
        if (strcmp(name, "WindowServer") == 0) return t;
    }
    return 0;
}
static void checkDisplays(void) {
    watchOn = [[NSFileManager defaultManager] fileExistsAtPath:[dataDir stringByAppendingPathComponent:@"session-watch"]];
    if (!watchOn) { emptyChecks = 0; for (int i = 0; i < 65; i++) watch[i].missing = 0; return; }  // off: count nothing
    long ws = windowServerStart();
    if (dw_stale(myStart, ws)) { NSLog(@"session watch: started %ld, before the running WindowServer (%ld): its session is gone, exiting", myStart, ws); exit(6); }
    if (CGDisplayIsAsleep(CGMainDisplayID())) return;  // a sleeping Mac is not a dead session: count nothing
    CGDirectDisplayID on[64]; int n = onlineList(on, 64);
    if (dw_session(n, &emptyChecks, &DW_DEFAULT) == DW_EXIT) { NSLog(@"session watch: no online display (%d) for %d checks, exiting", n, DW_DEFAULT.need); exit(3); }
    if (n <= 0) return;
    for (NSNumber *k in displays.allKeys.copy) {
        int sid = k.intValue, r = dw_step(&watch[sid], on, n, [NSDate date].timeIntervalSince1970, &DW_DEFAULT);
        if (r == DW_EXIT) { NSLog(@"session watch: screen %d re-created %d times and still missing, exiting", sid, watch[sid].nRecreated); exit(4); }
        if (r == DW_RECREATE) {
            NSArray *z = sizes[k]; unsigned old = watch[sid].did;
            CGDirectDisplayID did = create(sid, [z[0] unsignedIntValue], [z[1] unsignedIntValue], [z[2] unsignedIntValue]);
            if (!did) { NSLog(@"session watch: re-creating screen %d failed, exiting", sid); exit(5); }  // displays.json still has it
            recreatedTotal++;
            NSLog(@"session watch: screen %d display %u was gone, re-created as %u", sid, old, did);
        }
    }
}
static NSString *healthJSON(void) {
    CGDirectDisplayID on[64]; int n = onlineList(on, 64);
    NSMutableArray *dead = [NSMutableArray array];
    for (NSNumber *k in displays) if (!dw_online(displays[k].displayID, on, n)) [dead addObject:k];
    NSDictionary *h = @{@"watch": @(watchOn), @"online": @(n), @"dead": dead, @"recreated": @(recreatedTotal),
                        @"parent": @(getppid()), @"orphan": @(getppid() == 1),
                        @"staleSession": @(dw_stale(myStart, windowServerStart()))};
    return [[NSString alloc] initWithData:[NSJSONSerialization dataWithJSONObject:h options:0 error:nil] encoding:NSUTF8StringEncoding];
}
static int param(NSString *q, NSString *key) {
    for (NSString *kv in [q componentsSeparatedByString:@"&"]) {
        NSArray *p = [kv componentsSeparatedByString:@"="];
        if (p.count == 2 && [p[0] isEqualToString:key]) return [p[1] intValue];
    }
    return -1;
}
static NSString *place(NSString *bundle, int sid, int *code) {
    CGVirtualDisplay *d = displays[@(sid)];
    NSRunningApplication *app = [NSRunningApplication runningApplicationsWithBundleIdentifier:bundle].firstObject;
    if (!d || !app) { *code = 404; return @"{\"error\":\"no such screen or app\"}"; }
    if (!AXIsProcessTrusted()) { *code = 403; return @"{\"error\":\"needs Accessibility\"}"; }
    AXUIElementRef ax = AXUIElementCreateApplication(app.processIdentifier);
    CFTypeRef win = NULL;
    if (AXUIElementCopyAttributeValue(ax, kAXFocusedWindowAttribute, &win) != kAXErrorSuccess || !win) {
        CFArrayRef wins = NULL;
        if (AXUIElementCopyAttributeValue(ax, kAXWindowsAttribute, (CFTypeRef *)&wins) == kAXErrorSuccess && wins && CFArrayGetCount(wins))
            win = CFRetain(CFArrayGetValueAtIndex(wins, 0));
        if (wins) CFRelease(wins);
    }
    if (!win) { CFRelease(ax); *code = 404; return @"{\"error\":\"no window\"}"; }
    CGRect b = CGDisplayBounds(d.displayID);
    CGPoint pos = CGPointMake(b.origin.x, b.origin.y + 25); CGSize size = CGSizeMake(b.size.width, b.size.height - 25);
    AXValueRef pv = AXValueCreate(kAXValueCGPointType, &pos), sv = AXValueCreate(kAXValueCGSizeType, &size);
    AXUIElementSetAttributeValue(win, kAXPositionAttribute, pv);
    AXUIElementSetAttributeValue(win, kAXSizeAttribute, sv);
    CFRelease(pv); CFRelease(sv); CFRelease(win); CFRelease(ax);
    return @"{\"ok\":true}";
}
static NSString *paramS(NSString *q, NSString *key) {
    for (NSString *kv in [q componentsSeparatedByString:@"&"]) {
        NSArray *p = [kv componentsSeparatedByString:@"="];
        if (p.count == 2 && [p[0] isEqualToString:key]) return [p[1] stringByRemovingPercentEncoding];
    }
    return nil;
}
// Runs on the main queue (CGVirtualDisplay wants it).
static NSString *handle(NSString *method, NSString *target, int *code) {
    NSArray *parts = [target componentsSeparatedByString:@"?"];
    NSString *path = parts[0], *q = parts.count > 1 ? parts[1] : @"";
    *code = 200;
    if ([method isEqualToString:@"GET"] && [path isEqualToString:@"/displays"]) return listJSON();
    if ([method isEqualToString:@"GET"] && [path isEqualToString:@"/health"]) return healthJSON();
    if ([method isEqualToString:@"POST"] && [path isEqualToString:@"/displays"]) {
        int sid = param(q, @"id"), w = param(q, @"w"), h = param(q, @"h"), sc = param(q, @"scale");
        if (sc < 1) sc = 2;
        if (sid < 1 || sid > 64 || w < 640 || h < 400 || w > 3840 || h > 2400 || sc > 2) { *code = 400; return @"{\"error\":\"id 1-64, w 640-3840, h 400-2400 points, scale 1-2\"}"; }
        if (!create(sid, (unsigned)w, (unsigned)h, (unsigned)sc)) { *code = 500; return @"{\"error\":\"create failed\"}"; }
        save(); return listJSON();
    }
    if ([method isEqualToString:@"POST"] && [path isEqualToString:@"/remove"]) {
        int sid = param(q, @"id");
        [displays removeObjectForKey:@(sid)]; [sizes removeObjectForKey:@(sid)]; save();
        if (sid >= 1 && sid <= 64) memset(&watch[sid], 0, sizeof watch[sid]);
        NSLog(@"screen %d removed", sid); return listJSON();
    }
    if ([method isEqualToString:@"POST"] && [path isEqualToString:@"/place"]) return place(paramS(q, @"app") ?: @"", param(q, @"screen"), code);
    *code = 404; return @"{\"error\":\"not found\"}";
}
static void serve(int fd) {
    dispatch_async(dispatch_get_global_queue(0, 0), ^{
        char buf[2048]; ssize_t n = recv(fd, buf, sizeof buf - 1, 0);
        if (n <= 0) { close(fd); return; }
        buf[n] = 0;
        char m[8] = {}, t[512] = {};
        if (sscanf(buf, "%7s %511s", m, t) != 2) { close(fd); return; }
        __block NSString *body; __block int code;
        NSString *method = @(m), *target = @(t);
        dispatch_sync(dispatch_get_main_queue(), ^{ body = handle(method, target, &code); });
        NSString *head = [NSString stringWithFormat:@"HTTP/1.0 %d %@\r\nContent-Type: application/json\r\nContent-Length: %lu\r\n\r\n",
                          code, code == 200 ? @"OK" : @"Error", (unsigned long)[body lengthOfBytesUsingEncoding:NSUTF8StringEncoding]];
        NSData *out = [[head stringByAppendingString:body] dataUsingEncoding:NSUTF8StringEncoding];
        send(fd, out.bytes, out.length, 0); close(fd);
    });
}
static CGPoint pointOn(int sid, int u, int v, BOOL *ok) {
    CGVirtualDisplay *d = displays[@(sid)]; *ok = d != nil;
    if (!d) return CGPointZero;
    CGRect b = CGDisplayBounds(d.displayID);  // global points, follows the user's arrangement
    return CGPointMake(b.origin.x + b.size.width * fmin(fmax(u, 0), 10000) / 10000.0,
                       b.origin.y + b.size.height * fmin(fmax(v, 0), 10000) / 10000.0);
}
static CGEventSourceRef gEventSrc;  // one source for all synthetic input (created in main)
static void post(CGEventType t, CGPoint p, int clickState) {
    // A real event source + a click state make synthetic clicks look like hardware ones; without them some apps
    // (seen with Chrome) silently ignore the click. clickState 1 = single click; 0 for plain moves.
    CGEventRef e = CGEventCreateMouseEvent(gEventSrc, t, p, kCGMouseButtonLeft);
    if (e) {
        if (clickState > 0) CGEventSetIntegerValueField(e, kCGMouseEventClickState, clickState);
        CGEventPost(kCGHIDEventTap, e); CFRelease(e);
    }
}
static void scrollAt(int sid, int u, int v, int dy, int dx) {
    BOOL ok; CGPoint p = pointOn(sid, u, v, &ok); if (!ok) return;
    // Pixel-unit scroll so the headset's small, fast (~72 Hz) waves are not lost to notch rounding. wheel1 is
    // vertical, wheel2 horizontal. dy>0 means the hand went up and content should reveal below, which is a
    // negative wheel1 delta; dx>0 scrolls right. Route it at p so the window under the point scrolls.
    CGEventRef e = CGEventCreateScrollWheelEvent(NULL, kCGScrollEventUnitPixel, 2, -dy, dx);
    if (e) { CGEventSetLocation(e, p); CGEventPost(kCGHIDEventTap, e); CFRelease(e); }
}
static void mouse(const char *msg) {
    char op = 0; int sid = 0, u = 0, v = 0;
    if (msg[0] == 'w') {
        int dy = 0, dx = 0;
        if (sscanf(msg, "w %d %d %d %d %d", &sid, &u, &v, &dy, &dx) >= 4) scrollAt(sid, u, v, dy, dx);
        return;
    }
    if (sscanf(msg, "%c %d %d %d", &op, &sid, &u, &v) != 4) return;
    BOOL ok; CGPoint p = pointOn(sid, u, v, &ok); if (!ok) return;
    if (op == 'm') CGWarpMouseCursorPosition(p);
    if (op == 'c') {
        CGWarpMouseCursorPosition(p);          // hover first: some apps ignore a click with no prior pointer there
        post(kCGEventMouseMoved, p, 0);
        post(kCGEventLeftMouseDown, p, 1);
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 60 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{ post(kCGEventLeftMouseUp, p, 1); });
    }
}
int main(void) {
    @autoreleasepool {
        displays = [NSMutableDictionary dictionary]; sizes = [NSMutableDictionary dictionary];
        gEventSrc = CGEventSourceCreate(kCGEventSourceStateCombinedSessionState);
        NSString *dir = dataDir = [NSHomeDirectory() stringByAppendingPathComponent:@".local/share/pinpin-displays"];
        [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
        statePath = [dir stringByAppendingPathComponent:@"displays.json"];
        NSArray *saved = [NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:statePath] ?: [NSData data] options:0 error:nil];
        for (NSDictionary *e in saved) create([e[@"id"] intValue], [e[@"w"] unsignedIntValue], [e[@"h"] unsignedIntValue], e[@"scale"] ? [e[@"scale"] unsignedIntValue] : 2);

        int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_port = htons(kPort); a.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 8)) { perror("bind"); return 1; }
        NSLog(@"pinpin-displays on :%d, %lu display(s)", kPort, (unsigned long)displays.count);
        dispatch_source_t src = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, s, 0, dispatch_get_global_queue(0, 0));
        dispatch_source_set_event_handler(src, ^{
            struct sockaddr_in peer; socklen_t len = sizeof peer;
            int fd = accept(s, (struct sockaddr *)&peer, &len); if (fd < 0) return;
            uint32_t ip = ntohl(peer.sin_addr.s_addr);
            if ((ip >> 24) != 127 && (ip >> 16) != ((192 << 8) | 168)) { close(fd); return; }  // loopback or house LAN only
            serve(fd);
        });
        dispatch_resume(src);

        int us = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in ua = {0}; ua.sin_family = AF_INET; ua.sin_port = htons(kMousePort); ua.sin_addr.s_addr = htonl(INADDR_ANY);
        if (bind(us, (struct sockaddr *)&ua, sizeof ua)) { perror("bind udp"); return 1; }
        CGSetLocalEventsSuppressionInterval(0.0);  // a warp must not swallow the click that follows it
        // Ask once: macOS lists the app under Accessibility and offers to open the setting.
        BOOL trusted = AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)@{(__bridge NSString *)kAXTrustedCheckOptionPrompt: @YES});
        NSLog(@"mouse on udp :%d (Accessibility trusted: %d)", kMousePort, trusted);
        dispatch_source_t usrc = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, us, 0, dispatch_get_main_queue());
        dispatch_source_set_event_handler(usrc, ^{
            char buf[128]; struct sockaddr_in peer; socklen_t len = sizeof peer;
            ssize_t n = recvfrom(us, buf, sizeof buf - 1, 0, (struct sockaddr *)&peer, &len);
            uint32_t ip = ntohl(peer.sin_addr.s_addr);
            if (n <= 0 || ((ip >> 24) != 127 && (ip >> 16) != ((192 << 8) | 168))) return;
            buf[n] = 0; mouse(buf);
        });
        dispatch_resume(usrc);

        watchOn = [[NSFileManager defaultManager] fileExistsAtPath:[dir stringByAppendingPathComponent:@"session-watch"]];
        myStart = procStart(getpid(), NULL, 0);
        NSLog(@"session watch %@ at start (started %ld, WindowServer %ld; the flag is read on every 10 s tick)", watchOn ? @"on" : @"off", myStart, windowServerStart());
        dispatch_source_t tm = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
        dispatch_source_set_timer(tm, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC), 10 * NSEC_PER_SEC, NSEC_PER_SEC);
        dispatch_source_set_event_handler(tm, ^{ checkDisplays(); });
        dispatch_resume(tm);
        [[NSRunLoop mainRunLoop] run];
    }
    return 0;
}
