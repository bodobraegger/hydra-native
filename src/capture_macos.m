// The macOS backend of src/capture.h with ScreenCaptureKit (macOS 12.3 or newer). initScreen uses the system
// picker SCContentSharingPicker (macOS 14 or newer); monitor, area and window capture use SCShareableContent and
// need the Screen Recording permission. ScreenCaptureKit scales the content to width x height (keeping its aspect
// ratio) and delivers BGRA frames, which vImage turns into RGBA for the output. capture_system_audio() uses the
// audio of a stream of the main display (macOS 13 or newer) and writes it as mono samples.
#import <Accelerate/Accelerate.h>
#import <AppKit/AppKit.h>
#import <CoreMedia/CoreMedia.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include "capture.h"
#include "process.h"

#define FRAMES_PER_SECOND 60
#define QUEUE_DEPTH 3

static int output_stream;
static int frame_width, frame_height;
static uint8_t *frame;
// SCStream and its output, held for the life of the capture (typed id, so that no global needs an availability
// annotation).
static id capture_stream, capture_output;

// Ends the helper; the parent reads the end of the stream as the end of the video source.
static void finish(int status) {
    fflush(stderr);
    exit(status);
}

API_AVAILABLE(macos(12.3))
@interface CaptureOutput : NSObject <SCStreamOutput, SCStreamDelegate>
@end

@implementation CaptureOutput
- (void)stream:(SCStream *)stream didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer ofType:(SCStreamOutputType)type {
    if (type != SCStreamOutputTypeScreen || !CMSampleBufferIsValid(sampleBuffer)) return;
    // Frames without changes (SCFrameStatusIdle) carry no image.
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, false);
    NSDictionary *info = attachments && CFArrayGetCount(attachments) ? (__bridge NSDictionary *)CFArrayGetValueAtIndex(attachments, 0) : nil;
    NSNumber *status = info[SCStreamFrameInfoStatus];
    if (!status || status.integerValue != SCFrameStatusComplete) return;
    CVPixelBufferRef pixels = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (!pixels) return;
    CVPixelBufferLockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly);
    size_t width = CVPixelBufferGetWidth(pixels), height = CVPixelBufferGetHeight(pixels);
    vImage_Buffer source = { CVPixelBufferGetBaseAddress(pixels), MIN(height, (size_t)frame_height),
                             MIN(width, (size_t)frame_width), CVPixelBufferGetBytesPerRow(pixels) };
    vImage_Buffer destination = { frame, source.height, source.width, (size_t)frame_width * 4 };
    const uint8_t bgra_to_rgba[4] = { 2, 1, 0, 3 };
    vImagePermuteChannels_ARGB8888(&source, &destination, bgra_to_rgba, kvImageNoFlags);
    CVPixelBufferUnlockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly);
    if (!stream_write_all(output_stream, frame, (size_t)frame_width * (size_t)frame_height * 4)) finish(0);
}

- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error {
    fprintf(stderr, "capture: the stream stopped: %s\n", error.localizedDescription.UTF8String);
    finish(1);
}
@end

API_AVAILABLE(macos(12.3))
static void start_stream(SCContentFilter *filter, CGRect source_rect) {
    SCStreamConfiguration *configuration = [[SCStreamConfiguration alloc] init];
    configuration.width = (size_t)frame_width;
    configuration.height = (size_t)frame_height;
    configuration.pixelFormat = kCVPixelFormatType_32BGRA;
    configuration.minimumFrameInterval = CMTimeMake(1, FRAMES_PER_SECOND);
    configuration.queueDepth = QUEUE_DEPTH;
    configuration.showsCursor = YES;
    if (!CGRectIsNull(source_rect)) configuration.sourceRect = source_rect;
    CaptureOutput *output = [[CaptureOutput alloc] init];
    SCStream *stream = [[SCStream alloc] initWithFilter:filter configuration:configuration delegate:output];
    NSError *error = nil;
    dispatch_queue_t queue = dispatch_queue_create("hydra-native-capture", DISPATCH_QUEUE_SERIAL);
    if (![stream addStreamOutput:output type:SCStreamOutputTypeScreen sampleHandlerQueue:queue error:&error]) {
        fprintf(stderr, "capture: %s\n", error.localizedDescription.UTF8String);
        finish(1);
    }
    capture_stream = stream;
    capture_output = output;
    [stream startCaptureWithCompletionHandler:^(NSError *start_error) {
        if (start_error) {
            fprintf(stderr, "capture: %s\n", start_error.localizedDescription.UTF8String);
            finish(1);
        }
    }];
}

// ---------------------------------------------------------------- the picker (initScreen)

API_AVAILABLE(macos(14.0))
@interface PickerObserver : NSObject <SCContentSharingPickerObserver>
@end

@implementation PickerObserver
- (void)contentSharingPicker:(SCContentSharingPicker *)picker didUpdateWithFilter:(SCContentFilter *)filter forStream:(SCStream *)stream {
    if (capture_stream) return; // the first choice starts the stream; the helper captures one source
    picker.active = NO;
    start_stream(filter, CGRectNull);
}

- (void)contentSharingPicker:(SCContentSharingPicker *)picker didCancelForStream:(SCStream *)stream {
    fprintf(stderr, "capture: the picker was closed\n");
    finish(1);
}

- (void)contentSharingPickerStartDidFailWithError:(NSError *)error {
    fprintf(stderr, "capture: the picker failed: %s\n", error.localizedDescription.UTF8String);
    finish(1);
}
@end

static id picker_observer;

API_AVAILABLE(macos(14.0))
static void present_picker(void) {
    SCContentSharingPicker *picker = SCContentSharingPicker.sharedPicker;
    SCContentSharingPickerConfiguration *configuration = [[SCContentSharingPickerConfiguration alloc] init];
    configuration.allowedPickerModes = SCContentSharingPickerModeSingleWindow | SCContentSharingPickerModeSingleDisplay;
    picker.defaultConfiguration = configuration;
    picker.maximumStreamCount = @1;
    picker_observer = [[PickerObserver alloc] init];
    [picker addObserver:picker_observer];
    picker.active = YES;
    [picker present];
}

// ---------------------------------------------------------------- monitor, area and window

// The ID of the frontmost normal window (layer 0) of the screen; CGWindowListCopyWindowInfo lists the windows
// from front to back.
static CGWindowID front_window(void) {
    CFArrayRef windows = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
                                                    kCGNullWindowID);
    CGWindowID found = kCGNullWindowID;
    for (NSDictionary *window in (__bridge NSArray *)windows) {
        if ([window[(__bridge NSString *)kCGWindowLayer] integerValue] == 0) {
            found = (CGWindowID)[window[(__bridge NSString *)kCGWindowNumber] unsignedIntValue];
            break;
        }
    }
    if (windows) CFRelease(windows);
    return found;
}

API_AVAILABLE(macos(12.3))
static void capture_content(const struct capture_request *request, SCShareableContent *content) {
    if (request->source == CAPTURE_WINDOW) {
        CGWindowID wanted = front_window();
        for (SCWindow *window in content.windows) {
            if (window.windowID == wanted) {
                start_stream([[SCContentFilter alloc] initWithDesktopIndependentWindow:window], CGRectNull);
                return;
            }
        }
        fprintf(stderr, "capture: no window has the focus\n");
        finish(1);
    }
    NSArray<SCDisplay *> *displays = content.displays;
    if (request->source == CAPTURE_MONITOR) {
        if (request->monitor < 0 || (NSUInteger)request->monitor >= displays.count) {
            fprintf(stderr, "capture: no monitor %d, there are %lu\n", request->monitor, (unsigned long)displays.count);
            finish(1);
        }
        SCDisplay *display = displays[(NSUInteger)request->monitor];
        start_stream([[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]], CGRectNull);
        return;
    }
    // CAPTURE_AREA: x and y are global display coordinates in points (origin at the top left of the main display);
    // sourceRect is relative to the display that contains the top left corner of the area.
    CGPoint corner = CGPointMake(request->x, request->y);
    SCDisplay *display = displays.firstObject;
    for (SCDisplay *candidate in displays)
        if (CGRectContainsPoint(candidate.frame, corner)) display = candidate;
    if (!display) {
        fprintf(stderr, "capture: no display\n");
        finish(1);
    }
    CGRect area = CGRectMake(request->x - display.frame.origin.x, request->y - display.frame.origin.y, request->area_width,
                             request->area_height);
    start_stream([[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]], area);
}

// Ends the helper with a hint when the Screen Recording permission is missing, which also covers the audio.
static void require_permission(void) {
    if (CGPreflightScreenCaptureAccess()) return;
    CGRequestScreenCaptureAccess(); // shows the prompt of the system once
    fprintf(stderr, "capture: allow Screen Recording for hydra-native in System Settings, Privacy & Security, "
                    "then start the capture again\n");
    finish(1);
}

API_AVAILABLE(macos(12.3))
static void capture_without_picker(const struct capture_request *request) {
    require_permission();
    struct capture_request copy = *request;
    dispatch_time_t start = dispatch_time(DISPATCH_TIME_NOW, (int64_t)(request->delay_seconds * NSEC_PER_SEC));
    dispatch_after(start, dispatch_get_main_queue(), ^{
        [SCShareableContent getShareableContentExcludingDesktopWindows:YES onScreenWindowsOnly:YES
                                                     completionHandler:^(SCShareableContent *content, NSError *error) {
            if (!content) {
                fprintf(stderr, "capture: %s\n", error.localizedDescription.UTF8String);
                finish(1);
            }
            capture_content(&copy, content);
        }];
    });
}

int capture_run(const struct capture_request *request, int output) {
    const char *names[] = { [CAPTURE_PICKER] = "picker", [CAPTURE_MONITOR] = "monitor", [CAPTURE_AREA] = "area",
                            [CAPTURE_WINDOW] = "window" };
    if (request->dry_run) {
        printf("ScreenCaptureKit %s %dx%d\n", names[request->source], request->width, request->height);
        return 0;
    }
    output_stream = output;
    frame_width = request->width;
    frame_height = request->height;
    frame = calloc((size_t)frame_width * (size_t)frame_height, 4);
    @autoreleasepool {
        // An application object connects the helper to the window server, which the picker needs; the accessory
        // policy keeps it out of the Dock.
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        if (request->source == CAPTURE_PICKER) {
            if (@available(macOS 14.0, *)) {
                present_picker();
            } else {
                fprintf(stderr, "capture: the picker of initScreen needs macOS 14 or newer\n");
                return 1;
            }
        } else if (@available(macOS 12.3, *)) {
            capture_without_picker(request);
        } else {
            fprintf(stderr, "capture: screen capture needs macOS 12.3 or newer\n");
            return 1;
        }
        [NSApp run]; // finish() ends the process
    }
    return 0;
}

// ---------------------------------------------------------------- the system audio

API_AVAILABLE(macos(13.0))
@interface AudioOutput : NSObject <SCStreamOutput, SCStreamDelegate>
@end

@implementation AudioOutput
// The mean of the channels of each frame, interleaved or one buffer per channel, as 32-bit floats.
- (void)stream:(SCStream *)stream didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer ofType:(SCStreamOutputType)type {
    if (type != SCStreamOutputTypeAudio || !CMSampleBufferIsValid(sampleBuffer)) return;
    const AudioStreamBasicDescription *format =
        CMAudioFormatDescriptionGetStreamBasicDescription(CMSampleBufferGetFormatDescription(sampleBuffer));
    if (!format || !(format->mFormatFlags & kAudioFormatFlagIsFloat) || format->mBitsPerChannel != 32 ||
        format->mChannelsPerFrame == 0)
        return;
    size_t list_size = 0;
    CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(sampleBuffer, &list_size, NULL, 0, NULL, NULL, 0, NULL);
    AudioBufferList *list = malloc(list_size);
    CMBlockBufferRef block = NULL;
    OSStatus status = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
        sampleBuffer, NULL, list, list_size, NULL, NULL, kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment, &block);
    UInt32 channels = format->mChannelsPerFrame;
    bool planar = format->mFormatFlags & kAudioFormatFlagIsNonInterleaved;
    size_t frames = (size_t)CMSampleBufferGetNumSamples(sampleBuffer);
    float *mono = status == noErr && (!planar || list->mNumberBuffers >= channels) ? malloc(frames * sizeof(float)) : NULL;
    for (size_t i = 0; mono && i < frames; i++) {
        float sum = 0;
        for (UInt32 channel = 0; channel < channels; channel++)
            sum += planar ? ((const float *)list->mBuffers[channel].mData)[i]
                          : ((const float *)list->mBuffers[0].mData)[i * channels + channel];
        mono[i] = sum / (float)channels;
    }
    bool written = !mono || stream_write_all(output_stream, mono, frames * sizeof(float));
    free(mono);
    free(list);
    if (block) CFRelease(block);
    if (!written) finish(0);
}

- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error {
    fprintf(stderr, "capture: the audio stream stopped: %s\n", error.localizedDescription.UTF8String);
    finish(1);
}
@end

API_AVAILABLE(macos(13.0))
static void start_audio_stream(SCShareableContent *content) {
    SCDisplay *display = content.displays.firstObject;
    if (!display) {
        fprintf(stderr, "capture: no display\n");
        finish(1);
    }
    SCStreamConfiguration *configuration = [[SCStreamConfiguration alloc] init];
    configuration.capturesAudio = YES;
    configuration.excludesCurrentProcessAudio = YES;
    configuration.sampleRate = CAPTURE_AUDIO_SAMPLE_RATE;
    configuration.channelCount = 1;
    // The stream has no screen output; the smallest frames, once a second, keep its video work low.
    configuration.width = 2;
    configuration.height = 2;
    configuration.minimumFrameInterval = CMTimeMake(1, 1);
    AudioOutput *output = [[AudioOutput alloc] init];
    SCContentFilter *filter = [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];
    SCStream *stream = [[SCStream alloc] initWithFilter:filter configuration:configuration delegate:output];
    NSError *error = nil;
    dispatch_queue_t queue = dispatch_queue_create("hydra-native-capture audio", DISPATCH_QUEUE_SERIAL);
    if (![stream addStreamOutput:output type:SCStreamOutputTypeAudio sampleHandlerQueue:queue error:&error]) {
        fprintf(stderr, "capture: %s\n", error.localizedDescription.UTF8String);
        finish(1);
    }
    capture_stream = stream;
    capture_output = output;
    [stream startCaptureWithCompletionHandler:^(NSError *start_error) {
        if (start_error) {
            fprintf(stderr, "capture: %s\n", start_error.localizedDescription.UTF8String);
            finish(1);
        }
    }];
}

int capture_system_audio(int output) {
    output_stream = output;
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        if (@available(macOS 13.0, *)) {
            require_permission();
            [SCShareableContent getShareableContentExcludingDesktopWindows:YES onScreenWindowsOnly:YES
                                                         completionHandler:^(SCShareableContent *content, NSError *error) {
                if (!content) {
                    fprintf(stderr, "capture: %s\n", error.localizedDescription.UTF8String);
                    finish(1);
                }
                start_audio_stream(content);
            }];
        } else {
            fprintf(stderr, "capture: the capture of the system audio needs macOS 13 or newer\n");
            return 1;
        }
        [NSApp run]; // finish() ends the process
    }
    return 0;
}
