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
// Only loopback and the house network (192.168.0.0/16) may call it.
#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

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

static const int kPort = 57710;
static NSMutableDictionary<NSNumber *, CGVirtualDisplay *> *displays;
static NSMutableDictionary<NSNumber *, NSArray<NSNumber *> *> *sizes;  // points w, h, scale
static NSString *statePath;

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
    d.terminationHandler = ^(id a, id b) { NSLog(@"display for screen %d terminated", sid); };
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
    NSLog(@"screen %d -> display %u (%ux%u points @%ux)", sid, v.displayID, w, h, scale);
    return v.displayID;
}
static NSString *listJSON(void) {
    NSMutableArray *a = [NSMutableArray array];
    for (NSNumber *k in [displays.allKeys sortedArrayUsingSelector:@selector(compare:)])
        [a addObject:@{@"screen": k, @"display": @(displays[k].displayID), @"w": @([sizes[k][0] intValue] * [sizes[k][2] intValue]),
                       @"h": @([sizes[k][1] intValue] * [sizes[k][2] intValue]), @"scale": sizes[k][2]}];
    return [[NSString alloc] initWithData:[NSJSONSerialization dataWithJSONObject:a options:0 error:nil] encoding:NSUTF8StringEncoding];
}
static int param(NSString *q, NSString *key) {
    for (NSString *kv in [q componentsSeparatedByString:@"&"]) {
        NSArray *p = [kv componentsSeparatedByString:@"="];
        if (p.count == 2 && [p[0] isEqualToString:key]) return [p[1] intValue];
    }
    return -1;
}
// Runs on the main queue (CGVirtualDisplay wants it).
static NSString *handle(NSString *method, NSString *target, int *code) {
    NSArray *parts = [target componentsSeparatedByString:@"?"];
    NSString *path = parts[0], *q = parts.count > 1 ? parts[1] : @"";
    *code = 200;
    if ([method isEqualToString:@"GET"] && [path isEqualToString:@"/displays"]) return listJSON();
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
        NSLog(@"screen %d removed", sid); return listJSON();
    }
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
int main(void) {
    @autoreleasepool {
        displays = [NSMutableDictionary dictionary]; sizes = [NSMutableDictionary dictionary];
        NSString *dir = [NSHomeDirectory() stringByAppendingPathComponent:@".local/share/pinpin-displays"];
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
        [[NSRunLoop mainRunLoop] run];
    }
    return 0;
}
