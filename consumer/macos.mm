#import <AppKit/AppKit.h>
#include "smoke.hpp"
int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: desktop_consumer PACK [REPORT]\n";
    return 2;
  }
  @autoreleasepool {
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    [NSApp finishLaunching];
    NSWindow* window = [[NSWindow alloc] initWithContentRect:NSMakeRect(80, 80, 760, 460)
                                                   styleMask:NSWindowStyleMaskTitled
                                                     backing:NSBackingStoreBuffered
                                                       defer:NO];
    window.releasedWhenClosed = NO;
    window.title = @"Weapp Container SDK installed consumer";
    [window orderFront:nil];
    Host host;
    host.parent = (__bridge void*)window.contentView;
    host.spin = [](double seconds) {
      NSDate* end = [NSDate dateWithTimeIntervalSinceNow:seconds];
      while (end.timeIntervalSinceNow > 0)
        [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode beforeDate:end];
    };
    host.click = [](View& view, double x, double y) {
      NSView* native = (__bridge NSView*)view.native_handle();
      NSPoint p = [native convertPoint:NSMakePoint(x, y) toView:nil];
      NSEvent* down = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDown
                                         location:p
                                    modifierFlags:0
                                        timestamp:0
                                     windowNumber:native.window.windowNumber
                                          context:nil
                                      eventNumber:1
                                       clickCount:1
                                         pressure:1];
      NSEvent* up = [NSEvent mouseEventWithType:NSEventTypeLeftMouseUp
                                       location:p
                                  modifierFlags:0
                                      timestamp:0
                                   windowNumber:native.window.windowNumber
                                        context:nil
                                    eventNumber:2
                                     clickCount:1
                                       pressure:0];
      [native mouseDown:down];
      [native mouseUp:up];
    };
    host.drag = [](View& view, double x, double from, double to) {
      NSView* native = (__bridge NSView*)view.native_handle();
      for (int step = 0; step <= 7; ++step) {
        const double y = step == 7 ? to : from + (to - from) * step / 6;
        NSEventType type = step == 0 ? NSEventTypeLeftMouseDown : step == 7 ? NSEventTypeLeftMouseUp : NSEventTypeLeftMouseDragged;
        NSEvent* event = [NSEvent mouseEventWithType:type location:[native convertPoint:NSMakePoint(x,y) toView:nil]
                                    modifierFlags:0 timestamp:step * .02 windowNumber:native.window.windowNumber context:nil
                                      eventNumber:step clickCount:1 pressure:1];
        if (step == 0) [native mouseDown:event];
        else if (step == 7) [native mouseUp:event];
        else [native mouseDragged:event];
      }
    };
    host.key = [](View& view) {
      NSView* native = (__bridge NSView*)view.native_handle();
      NSEvent* event = [NSEvent keyEventWithType:NSEventTypeKeyDown
                                        location:NSZeroPoint
                                   modifierFlags:0
                                       timestamp:0
                                    windowNumber:native.window.windowNumber
                                         context:nil
                                      characters:@"a"
                     charactersIgnoringModifiers:@"a"
                                       isARepeat:NO
                                         keyCode:0];
      [native keyDown:event];
    };
    host.window_visible = [window](bool visible) {
      if (visible)
        [window orderFront:nil];
      else
        [window orderOut:nil];
    };
    int result = smoke(argv[1], host, argc > 2 ? argv[2] : "");
    [window orderOut:nil];
    [window close];
    return result;
  }
}
