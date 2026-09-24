/* devcap - film a USB-connected iPhone's screen from this Mac, for the motion
 * tool (shared/tools/motion), with no QuickTime and no hands.
 *
 *   devcap list [--wait S]
 *       Every capture device AVFoundation sees once screen-capture devices
 *       are allowed, one per line: "<uniqueID>\t<model>\t<name>\t<types>".
 *       An iPhone on USB (unlocked, "Trust This Computer" answered) shows up
 *       as a muxed device a second or two after the opt-in.
 *   devcap record OUT.mov [--seconds S] [--device ID] [--wait S]
 *       Records the first iOS screen (or ID) into OUT.mov at the phone's own
 *       frame rate, until S seconds pass or SIGINT (the rig's recordVideo
 *       convention). Every composited frame keeps its own timestamp, which is
 *       what `motion_take.sh` reads.
 *
 * WHY A TOOL OF ITS OWN: an iPhone's screen is only offered to a process that
 * set CoreMediaIO's kCMIOHardwarePropertyAllowScreenCaptureDevices itself -
 * the opt-in is per process, so ffmpeg's avfoundation input never sees the
 * phone, and QuickTime is a GUI. Fixed-layout text out, no JSON.
 *
 * Objective-C only where AVFoundation needs it; the rest is C. */
#import <AVFoundation/AVFoundation.h>
#import <CoreMediaIO/CMIOHardware.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile sig_atomic_t stop_now;
static void on_sigint(int s) { (void)s; stop_now = 1; }

static int allow_screens(void) {
    CMIOObjectPropertyAddress a = {kCMIOHardwarePropertyAllowScreenCaptureDevices,
                                   kCMIOObjectPropertyScopeGlobal, kCMIOObjectPropertyElementMain};
    UInt32 yes = 1;
    OSStatus e = CMIOObjectSetPropertyData(kCMIOObjectSystemObject, &a, 0, NULL, sizeof yes, &yes);
    if (e != 0) { fprintf(stderr, "devcap: CMIO opt-in failed (%d)\n", (int)e); return 0; }
    return 1;
}

static NSArray<AVCaptureDevice *> *devices(void) {
    AVCaptureDeviceDiscoverySession *s = [AVCaptureDeviceDiscoverySession
        discoverySessionWithDeviceTypes:@[AVCaptureDeviceTypeExternal, AVCaptureDeviceTypeBuiltInWideAngleCamera]
                              mediaType:nil
                               position:AVCaptureDevicePositionUnspecified];
    return s.devices;
}

/* An iPhone/iPad screen: muxed, and its model says iOS. */
static int is_ios_screen(AVCaptureDevice *d) {
    return [d hasMediaType:AVMediaTypeMuxed] &&
           ([d.modelID rangeOfString:@"iOS"].location != NSNotFound ||
            [d.localizedName rangeOfString:@"iPhone"].location != NSNotFound ||
            [d.localizedName rangeOfString:@"iPad"].location != NSNotFound);
}

/* Devices appear asynchronously after the opt-in: spin the run loop until
 * an iOS screen shows up or `wait` seconds pass. */
static NSArray<AVCaptureDevice *> *settle(double wait) {
    NSDate *until = [NSDate dateWithTimeIntervalSinceNow:wait];
    NSArray<AVCaptureDevice *> *ds = devices();
    while ([until timeIntervalSinceNow] > 0) {
        for (AVCaptureDevice *d in ds) if (is_ios_screen(d)) return ds;
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.2]];
        ds = devices();
    }
    return ds;
}

static int list_main(double wait) {
    NSArray<AVCaptureDevice *> *ds = settle(wait);
    int n = 0;
    for (AVCaptureDevice *d in ds) {
        NSMutableArray *types = [NSMutableArray array];
        if ([d hasMediaType:AVMediaTypeMuxed]) [types addObject:@"muxed"];
        if ([d hasMediaType:AVMediaTypeVideo]) [types addObject:@"video"];
        printf("%s\t%s\t%s\t%s%s\n", d.uniqueID.UTF8String, d.modelID.UTF8String, d.localizedName.UTF8String,
               [types componentsJoinedByString:@","].UTF8String, is_ios_screen(d) ? "\tios-screen" : "");
        n++;
    }
    fprintf(stderr, "devcap: %d devices\n", n);
    return 0;
}

@interface Rec : NSObject <AVCaptureFileOutputRecordingDelegate>
@property int done;
@property int ok;
@end
@implementation Rec
- (void)captureOutput:(AVCaptureFileOutput *)o didFinishRecordingToOutputFileAtURL:(NSURL *)u
      fromConnections:(NSArray *)c error:(NSError *)e {
    (void)o; (void)c;
    /* a recording stopped on purpose reports an "error" with success set */
    BOOL fine = e == nil || [e.userInfo[AVErrorRecordingSuccessfullyFinishedKey] boolValue];
    if (!fine) fprintf(stderr, "devcap: %s\n", e.localizedDescription.UTF8String);
    else printf("%s\n", u.path.UTF8String);
    self.ok = fine;
    self.done = 1;
}
@end

/* CAMERA PRIVACY covers a phone's screen too: the process (the terminal
 * that runs this) must be allowed, or the session starts and records
 * nothing ("Cannot Record"). Asks once; says what to grant when denied. */
static int authorized(NSString *type) {
    AVAuthorizationStatus st = [AVCaptureDevice authorizationStatusForMediaType:type];
    if (st == AVAuthorizationStatusNotDetermined) {
        __block int answered = 0, granted = 0;
        [AVCaptureDevice requestAccessForMediaType:type completionHandler:^(BOOL g) { granted = g; answered = 1; }];
        NSDate *until = [NSDate dateWithTimeIntervalSinceNow:60];
        while (!answered && [until timeIntervalSinceNow] > 0)
            [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.1]];
        return granted;
    }
    return st == AVAuthorizationStatusAuthorized;
}

static int record_main(const char *out, double seconds, const char *want, double wait) {
    if (!authorized(AVMediaTypeVideo)) {
        fprintf(stderr, "devcap: camera access is off for this terminal - System Settings > Privacy & Security"
                        " > Camera, allow the terminal app, then run again\n");
        return 1;
    }
    NSArray<AVCaptureDevice *> *ds = settle(wait);
    AVCaptureDevice *dev = nil;
    for (AVCaptureDevice *d in ds) {
        if (want ? !strcmp(d.uniqueID.UTF8String, want) : is_ios_screen(d)) { dev = d; break; }
    }
    if (!dev) {
        fprintf(stderr, "devcap: no %s; is the phone on USB, unlocked and trusting this Mac? (devcap list)\n",
                want ? want : "iOS screen");
        return 1;
    }
    NSError *e = nil;
    AVCaptureDeviceInput *in = [AVCaptureDeviceInput deviceInputWithDevice:dev error:&e];
    if (!in) { fprintf(stderr, "devcap: %s\n", e.localizedDescription.UTF8String); return 1; }
    AVCaptureSession *s = [[AVCaptureSession alloc] init];
    if (![s canAddInput:in]) { fprintf(stderr, "devcap: the session refused the device\n"); return 1; }
    [s addInput:in];
    AVCaptureMovieFileOutput *mo = [[AVCaptureMovieFileOutput alloc] init];
    if (![s canAddOutput:mo]) { fprintf(stderr, "devcap: the session refused the movie output\n"); return 1; }
    [s addOutput:mo];
    [s startRunning];
    /* frames flow a moment after the session starts; a recording begun
     * before the first one fails */
    NSDate *warm = [NSDate dateWithTimeIntervalSinceNow:1.0];
    while ([warm timeIntervalSinceNow] > 0)
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
    if (!s.running) { fprintf(stderr, "devcap: the capture session did not start\n"); return 1; }
    NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:out]];
    [[NSFileManager defaultManager] removeItemAtURL:url error:nil];
    Rec *r = [[Rec alloc] init];
    [mo startRecordingToOutputFileURL:url recordingDelegate:r];
    fprintf(stderr, "devcap: recording %s (%s)%s\n", dev.localizedName.UTF8String, dev.uniqueID.UTF8String,
            seconds > 0 ? "" : " until ^C");
    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);
    NSDate *until = seconds > 0 ? [NSDate dateWithTimeIntervalSinceNow:seconds] : [NSDate distantFuture];
    while (!stop_now && [until timeIntervalSinceNow] > 0)
        [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
    [mo stopRecording];
    while (!r.done) [[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
    [s stopRunning];
    return r.ok ? 0 : 1;
}

static int usage(void) {
    fprintf(stderr, "usage: devcap list [--wait S]\n"
                    "       devcap record OUT.mov [--seconds S] [--device ID] [--wait S]\n");
    return 2;
}

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc < 2) return usage();
        double wait = 3, seconds = 0;
        const char *dev = NULL, *out = NULL;
        int i = 2;
        if (!strcmp(argv[1], "record")) { if (argc < 3) return usage(); out = argv[2]; i = 3; }
        for (; i < argc; i++) {
            if (!strcmp(argv[i], "--wait") && i + 1 < argc) wait = atof(argv[++i]);
            else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
            else if (!strcmp(argv[i], "--device") && i + 1 < argc) dev = argv[++i];
            else return usage();
        }
        if (!allow_screens()) return 1;
        if (!strcmp(argv[1], "list")) return list_main(wait);
        if (out) return record_main(out, seconds, dev, wait);
        return usage();
    }
}
