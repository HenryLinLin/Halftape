#include "mactap/source_control.hpp"
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <IOKit/hidsystem/ev_keymap.h>
#include <stdexcept>

namespace ws {
namespace {
bool scriptable(const std::string& bundle) { return bundle == "com.spotify.client" || bundle == "com.apple.Music"; }
void runScript(const std::string& bundle, NSString* verb, const char* failure) {
    NSString* identifier = [NSString stringWithUTF8String:bundle.c_str()];
    NSString* scriptText = [NSString stringWithFormat:@"tell application id \"%@\" to %@", identifier, verb];
    NSAppleScript* script = [[NSAppleScript alloc] initWithSource:scriptText];
    NSDictionary* details = nil;
    [script executeAndReturnError:&details];
    if (details) {
        NSString* reason = [details objectForKey:NSAppleScriptErrorMessage];
        throw std::runtime_error(reason ? [reason UTF8String] : failure);
    }
}
void mediaKey(int key) {
    if (!AXIsProcessTrusted())
        throw std::runtime_error("Enable Accessibility access for Halftape in System Settings > Privacy & Security > Accessibility to control this source app.");
    for (int phase : {0xA, 0xB}) {
        NSEvent* event = [NSEvent otherEventWithType:NSEventTypeSystemDefined
                                            location:NSZeroPoint
                                       modifierFlags:0xA00
                                           timestamp:0
                                        windowNumber:0
                                             context:nil
                                             subtype:8
                                               data1:((key << 16) | (phase << 8))
                                               data2:-1];
        if (!event || !event.CGEvent) throw std::runtime_error("Could not create the media key event.");
        CGEventPost(kCGHIDEventTap, event.CGEvent);
    }
}
}
void pauseSourceApp(const std::string& bundle) {
    if (scriptable(bundle)) { runScript(bundle, @"pause", "Could not pause the source app."); return; }
    mediaKey(NX_KEYTYPE_PLAY);
}
void transportSourceApp(const std::string& bundle, int command) {
    if (scriptable(bundle)) {
        NSString* verb = command == 0 ? @"previous track" : command == 2 ? @"next track" : @"playpause";
        runScript(bundle, verb, "Could not control the source app.");
        return;
    }
    mediaKey(command == 0 ? NX_KEYTYPE_PREVIOUS : command == 2 ? NX_KEYTYPE_NEXT : NX_KEYTYPE_PLAY);
}
}
