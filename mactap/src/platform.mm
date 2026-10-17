#include "mactap/platform.hpp"
#import <AppKit/AppKit.h>
#import <CoreAudio/CoreAudio.h>
#import <CoreAudio/AudioHardwareTapping.h>
#import <CoreAudio/CATapDescription.h>
#import <AudioUnit/AudioUnit.h>
#include <unistd.h>
namespace ws {
static AudioObjectPropertyAddress address(AudioObjectPropertySelector selector) {
    return {selector, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
}
static void check(OSStatus status, const char* operation) {
    if (status) throw std::runtime_error(std::string(operation) + " (" + std::to_string(status) + ")");
}
template<class T> static T property(AudioObjectID id, AudioObjectPropertySelector selector) {
    T value{}; UInt32 size = sizeof(T); auto a = address(selector);
    check(AudioObjectGetPropertyData(id, &a, 0, nullptr, &size, &value), "Read audio device");
    return value;
}
static UInt32 frames(AudioObjectID id, AudioObjectPropertySelector selector, AudioObjectPropertyScope scope) {
    UInt32 value = 0, size = sizeof(value);
    AudioObjectPropertyAddress a{selector, scope, kAudioObjectPropertyElementMain};
    return AudioObjectGetPropertyData(id, &a, 0, nullptr, &size, &value) ? 0 : value;
}
static double deviceLatency(AudioObjectID device, AudioObjectPropertyScope scope) {
    UInt32 total = frames(device, kAudioDevicePropertyLatency, scope) + frames(device, kAudioDevicePropertySafetyOffset, scope) +
                   frames(device, kAudioDevicePropertyBufferFrameSize, scope);
    AudioObjectPropertyAddress streams{kAudioDevicePropertyStreams, scope, kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (!AudioObjectGetPropertyDataSize(device, &streams, 0, nullptr, &size) && size >= sizeof(AudioObjectID)) {
        std::vector<AudioObjectID> ids(size / sizeof(AudioObjectID));
        if (!AudioObjectGetPropertyData(device, &streams, 0, nullptr, &size, ids.data()))
            total += frames(ids[0], kAudioStreamPropertyLatency, kAudioObjectPropertyScopeGlobal);
    }
    Float64 sampleRate = 0; UInt32 rateSize = sizeof(sampleRate); auto a = address(kAudioDevicePropertyNominalSampleRate);
    if (AudioObjectGetPropertyData(device, &a, 0, nullptr, &rateSize, &sampleRate) || sampleRate <= 0) return 0;
    return total / sampleRate;
}
// Returns the previous buffer size when the device was changed, otherwise zero.
static UInt32 shrinkBuffer(AudioObjectID device, UInt32 wanted) {
    auto a = address(kAudioDevicePropertyBufferFrameSize);
    UInt32 previous = 0, size = sizeof(previous);
    if (AudioObjectGetPropertyData(device, &a, 0, nullptr, &size, &previous)) return 0;
    AudioValueRange range{}; size = sizeof(range); auto r = address(kAudioDevicePropertyBufferFrameSizeRange);
    if (AudioObjectGetPropertyData(device, &r, 0, nullptr, &size, &range)) return 0;
    auto target = UInt32(std::clamp(double(wanted), range.mMinimum, range.mMaximum));
    if (target >= previous) return 0;
    return AudioObjectSetPropertyData(device, &a, 0, nullptr, sizeof(target), &target) ? 0 : previous;
}
std::vector<Source> sources() {
    @autoreleasepool {
        auto a = address(kAudioHardwarePropertyProcessObjectList); UInt32 size = 0;
        check(AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, nullptr, &size), "List audio apps");
        std::vector<AudioObjectID> ids(size / sizeof(AudioObjectID));
        check(AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &size, ids.data()), "List audio apps");
        std::vector<Source> result;
        for (auto id : ids) {
            try {
                auto pid = property<pid_t>(id, kAudioProcessPropertyPID);
                if (pid == getpid()) continue;
                auto bundle = property<CFStringRef>(id, kAudioProcessPropertyBundleID);
                NSString* bid = CFBridgingRelease(bundle);
                auto app = [NSRunningApplication runningApplicationWithProcessIdentifier:pid];
                NSString* name = app.localizedName ?: bid ?: @"Audio app";
                bool active = property<UInt32>(id, kAudioProcessPropertyIsRunningOutput);
                result.push_back({id, pid, name.UTF8String, bid ? bid.UTF8String : "", active});
            } catch (...) {}
        }
        std::stable_sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
            auto score = [](const auto& s) { return int(s.active) * 4 + int(s.bundle == "com.spotify.client" || s.bundle == "com.apple.Music") * 2; };
            return score(a) > score(b);
        });
        return result;
    }
}
struct Tap::Impl {
    AudioObjectID tap = 0, aggregate = 0; AudioDeviceIOProcID proc = nullptr; bool interleaved = true;
    Feed feed = nullptr; void* context = nullptr; double latency = 0;
};
Tap::Tap() : impl(std::make_unique<Impl>()) {}
Tap::~Tap() { stop(); }
void Tap::stop() {
    if (impl->proc) { AudioDeviceStop(impl->aggregate, impl->proc); AudioDeviceDestroyIOProcID(impl->aggregate, impl->proc); impl->proc = nullptr; }
    if (impl->aggregate) { AudioHardwareDestroyAggregateDevice(impl->aggregate); impl->aggregate = 0; }
    if (impl->tap) { AudioHardwareDestroyProcessTap(impl->tap); impl->tap = 0; }
    impl->feed = nullptr; impl->context = nullptr; impl->latency = 0;
    Frame frame; while (queue.pop(frame)) {}
}
double Tap::latency() const { return impl->latency; }
double Tap::start(uint32_t source, bool mute, Feed feed, void* context) {
    stop();
    impl->feed = feed; impl->context = context;
    @autoreleasepool {
        try {
            NSMutableArray<NSNumber*>* members = [NSMutableArray arrayWithObject:@(source)];
            auto list = sources();
            auto selected = std::find_if(list.begin(), list.end(), [source](const auto& x) { return x.id == source; });
            if (selected != list.end() && !selected->bundle.empty()) for (auto& s : list)
                if (s.id != source && (s.bundle == selected->bundle || s.bundle.starts_with(selected->bundle + "."))) [members addObject:@(s.id)];
            CATapDescription* desc = [[CATapDescription alloc] initStereoMixdownOfProcesses:members];
            desc.name = @"Halftape capture"; desc.privateTap = YES;
            desc.muteBehavior = mute ? CATapMutedWhenTapped : CATapUnmuted;
            check(AudioHardwareCreateProcessTap(desc, &impl->tap), "Allow Halftape in System Settings → Privacy & Security → Screen & System Audio Recording");
            auto format = property<AudioStreamBasicDescription>(impl->tap, kAudioTapPropertyFormat);
            if (format.mFormatID != kAudioFormatLinearPCM || !(format.mFormatFlags & kAudioFormatFlagIsFloat) || format.mBitsPerChannel != 32 || format.mChannelsPerFrame != 2)
                throw std::runtime_error("The source must provide stereo float audio");
            impl->interleaved = !(format.mFormatFlags & kAudioFormatFlagIsNonInterleaved);
            auto output = property<AudioObjectID>(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice);
            NSString* uid = CFBridgingRelease(property<CFStringRef>(output, kAudioDevicePropertyDeviceUID));
            NSDictionary* aggregate = @{
                @kAudioAggregateDeviceNameKey: @"Halftape private capture",
                @kAudioAggregateDeviceUIDKey: NSUUID.UUID.UUIDString,
                @kAudioAggregateDeviceIsPrivateKey: @YES,
                @kAudioAggregateDeviceMainSubDeviceKey: uid,
                @kAudioAggregateDeviceSubDeviceListKey: @[@{@kAudioSubDeviceUIDKey: uid}],
                @kAudioAggregateDeviceTapListKey: @[@{@kAudioSubTapUIDKey: desc.UUID.UUIDString, @kAudioSubTapDriftCompensationKey: @YES}],
                @kAudioAggregateDeviceTapAutoStartKey: @YES
            };
            check(AudioHardwareCreateAggregateDevice((__bridge CFDictionaryRef)aggregate, &impl->aggregate), "Create private capture device");
            if (mute) shrinkBuffer(impl->aggregate, 128);
            check(AudioDeviceCreateIOProcID(impl->aggregate, [](AudioObjectID, const AudioTimeStamp*, const AudioBufferList* input, const AudioTimeStamp*, AudioBufferList*, const AudioTimeStamp*, void* context) -> OSStatus {
                auto& tap = *static_cast<Tap*>(context);
                if (!input || !input->mNumberBuffers) return noErr;
                // The tap is appended after the hardware device's input streams.
                unsigned last = input->mNumberBuffers - 1;
                auto& right = input->mBuffers[last];
                auto& left = input->mBuffers[tap.impl->interleaved || !last ? last : last - 1];
                if (!left.mData || !right.mData) return noErr;
                auto* l = static_cast<const float*>(left.mData); auto* r = static_cast<const float*>(right.mData);
                unsigned channels = std::max(1u, left.mNumberChannels);
                unsigned n = left.mDataByteSize / sizeof(float) / channels;
                for (unsigned i = 0; i < n; ++i) {
                    Frame f = tap.impl->interleaved ? Frame{l[i * channels], l[i * channels + (channels > 1)]} : Frame{l[i], r[i]};
                    if (!tap.queue.push(f)) tap.dropped.fetch_add(1, std::memory_order_relaxed);
                    if (tap.impl->feed) tap.impl->feed(tap.impl->context, f);
                }
                return noErr;
            }, this, &impl->proc), "Create capture callback");
            check(AudioDeviceStart(impl->aggregate, impl->proc), "Start capture");
            impl->latency = deviceLatency(impl->aggregate, kAudioObjectPropertyScopeInput);
            return format.mSampleRate;
        } catch (...) { stop(); throw; }
    }
}
struct Output::Impl {
    AudioUnit unit = nullptr; Render render = nullptr; void* context = nullptr; id activity = nil;
    AudioObjectID device = 0; UInt32 restoreBuffer = 0; double latency = 0;
};
Output::Output() : impl(std::make_unique<Impl>()) {}
Output::~Output() { stop(); }
void Output::stop() {
    if (impl->unit) { AudioOutputUnitStop(impl->unit); AudioUnitUninitialize(impl->unit); AudioComponentInstanceDispose(impl->unit); impl->unit = nullptr; }
    if (impl->activity) { [NSProcessInfo.processInfo endActivity:impl->activity]; impl->activity = nil; }
    if (impl->restoreBuffer) {
        auto a = address(kAudioDevicePropertyBufferFrameSize);
        AudioObjectSetPropertyData(impl->device, &a, 0, nullptr, sizeof(impl->restoreBuffer), &impl->restoreBuffer);
        impl->restoreBuffer = 0;
    }
    impl->device = 0; impl->latency = 0;
}
double Output::latency() const { return impl->latency; }
void Output::start(Render render, void* context, bool live) {
    // TODO: follow output device changes without restarting transport.
    stop(); impl->render = render; impl->context = context;
    try {
        if (live) impl->activity = [NSProcessInfo.processInfo beginActivityWithOptions:NSActivityUserInitiatedAllowingIdleSystemSleep | NSActivityLatencyCritical reason:@"Live audio playback"];
        AudioComponentDescription description{kAudioUnitType_Output, kAudioUnitSubType_HALOutput, kAudioUnitManufacturer_Apple, 0, 0};
        check(AudioComponentInstanceNew(AudioComponentFindNext(nullptr, &description), &impl->unit), "Create playback output");
        auto device = property<AudioObjectID>(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice);
        impl->device = device;
        if (live) impl->restoreBuffer = shrinkBuffer(device, 128);
        check(AudioUnitSetProperty(impl->unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &device, sizeof(device)), "Choose playback device");
        AudioStreamBasicDescription format{44100, kAudioFormatLinearPCM, kAudioFormatFlagsNativeFloatPacked | kAudioFormatFlagIsNonInterleaved, 4, 1, 4, 2, 32, 0};
        check(AudioUnitSetProperty(impl->unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &format, sizeof(format)), "Set playback format");
        UInt32 maxFrames = 4096;
        check(AudioUnitSetProperty(impl->unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0, &maxFrames, sizeof(maxFrames)), "Set playback block size");
        AURenderCallbackStruct callback{[](void* c, AudioUnitRenderActionFlags*, const AudioTimeStamp*, UInt32, UInt32 n, AudioBufferList* data) -> OSStatus {
            auto* state = static_cast<Impl*>(c);
            if (data->mNumberBuffers < 2) return kAudio_ParamError;
            state->render(state->context, static_cast<float*>(data->mBuffers[0].mData), static_cast<float*>(data->mBuffers[1].mData), n);
            return noErr;
        }, impl.get()};
        check(AudioUnitSetProperty(impl->unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &callback, sizeof(callback)), "Set playback callback");
        check(AudioUnitInitialize(impl->unit), "Initialize playback");
        check(AudioOutputUnitStart(impl->unit), "Start playback");
        impl->latency = deviceLatency(device, kAudioObjectPropertyScopeOutput);
    } catch (...) { stop(); throw; }
}
}
