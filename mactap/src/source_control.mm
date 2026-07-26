#include "mactap/source_control.hpp"
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <IOKit/hidsystem/ev_keymap.h>
#include <stdexcept>

namespace ws {
void pauseSourceApp(const std::string& bundle) {
    if (bundle == "com.spotify.client" || bundle == "com.apple.Music") {
        NSString* identifier = [NSString stringWithUTF8String:bundle.c_str()];
        NSString* scriptText = [NSString stringWithFormat:
            @"tell application id \"%@\" to pause", identifier];
        NSAppleScript* script = [[NSAppleScript alloc] initWithSource:scriptText];
        NSDictionary* details = nil;
        [script executeAndReturnError:&details];
        if (details) {
            NSString* reason = [details objectForKey:NSAppleScriptErrorMessage];
            throw std::runtime_error(reason ? [reason UTF8String] : "Could not pause the source app.");
        }
        return;
    }

    if (!AXIsProcessTrusted())
        throw std::runtime_error("Enable Accessibility access for Halftape in System Settings > Privacy & Security > Accessibility to pause this source app.");

    NSEvent* down = [NSEvent otherEventWithType:NSEventTypeSystemDefined
                                      location:NSZeroPoint
                                 modifierFlags:0xA00
                                     timestamp:0
                                  windowNumber:0
                                       context:nil
                                       subtype:8
                                         data1:((NX_KEYTYPE_PLAY << 16) | (0xA << 8))
                                         data2:-1];
    if (!down || !down.CGEvent)
        throw std::runtime_error("Could not create the media play/pause event.");
    CGEventPost(kCGHIDEventTap, down.CGEvent);

    NSEvent* up = [NSEvent otherEventWithType:NSEventTypeSystemDefined
                                    location:NSZeroPoint
                               modifierFlags:0xA00
                                   timestamp:0
                                windowNumber:0
                                     context:nil
                                     subtype:8
                                       data1:((NX_KEYTYPE_PLAY << 16) | (0xB << 8))
                                       data2:-1];
    if (!up || !up.CGEvent)
        throw std::runtime_error("Could not create the media play/pause release event.");
    CGEventPost(kCGHIDEventTap, up.CGEvent);
}
}
