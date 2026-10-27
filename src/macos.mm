#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <cmath>
#include <mutex>
#include "internal.hpp"

namespace weapp::desktop::detail {
class MacPresenter;
}
@interface WCDesktopNativeView : NSView
@property(nonatomic, assign) weapp::desktop::detail::MacPresenter* owner;
@end

namespace weapp::desktop::detail {
const char* platform_name() { return "macos"; }
class MacLoop final : public Loop {
 public:
  explicit MacLoop(std::function<void()> tick) {
    timer_ = [NSTimer timerWithTimeInterval:1.0 / 60.0
                                    repeats:YES
                                      block:^(NSTimer*) {
                                        tick();
                                      }];
    [[NSRunLoop mainRunLoop] addTimer:timer_ forMode:NSRunLoopCommonModes];
  }
  ~MacLoop() override { [timer_ invalidate]; }

 private:
  NSTimer* timer_;
};
std::unique_ptr<Loop> create_loop(std::function<void()> tick, Status& status) {
  if (![NSThread isMainThread]) {
    status = Status::error("ERR_WRONG_THREAD", "macOS SDK requires the AppKit main thread");
    return {};
  }
  status = {};
  return std::make_unique<MacLoop>(std::move(tick));
}
class MacPresenter final : public Presenter {
 public:
  explicit MacPresenter(Core& c) : Presenter(c) {}
  ~MacPresenter() override {
    core_.report(detach());
    view_.owner = nullptr;
    [view_ removeFromSuperview];
  }
  Status initialize(void* parent, const Rect& r) override {
    device_ = MTLCreateSystemDefaultDevice();
    if (!device_)
      return Status::error("ERR_METAL_UNAVAILABLE", "this macOS adapter requires a Metal device");
    queue_ = [device_ newCommandQueue];
    if (!queue_)
      return Status::error("ERR_METAL_UNAVAILABLE", "failed to create the Metal command queue");
    view_ = [[WCDesktopNativeView alloc]
        initWithFrame:NSMakeRect(r.x, r.y, std::max(0.0, r.width), std::max(0.0, r.height))];
    layer_ = [CAMetalLayer layer];
    layer_.device = device_;
    layer_.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer_.framebufferOnly = NO;
    layer_.opaque = YES;
    layer_.maximumDrawableCount = 3;
    view_.wantsLayer = YES;
    view_.layer = layer_;
    view_.owner = this;
    if (parent) [(__bridge NSView*)parent addSubview:view_];
    update();
    return {};
  }
  void* native_handle() const override { return (__bridge void*)view_; }
  Status set_bounds(const Rect& r) override {
    if (!std::isfinite(r.width) || !std::isfinite(r.height) || !std::isfinite(r.x) ||
        !std::isfinite(r.y))
      return Status::error("ERR_INVALID_ARGUMENT", "view bounds must be finite");
    [view_ setFrame:NSMakeRect(r.x, r.y, std::max(0.0, r.width), std::max(0.0, r.height))];
    update();
    return {};
  }
  Status set_visible(bool visible) override {
    view_.hidden = !visible;
    update();
    return {};
  }
  Status attach() override {
    if (attached_) return {};
    Buffer b;
    auto s = b.read(weapp_container_sdk_metal_attach(core_.handle, (__bridge void*)device_,
                                                     (__bridge void*)queue_, &b.value));
    if (s) {
      attached_ = true;
      ++epoch_;
      dirty_ = true;
      // Attach accepts a fresh presentation revision, including restoration
      // after a failed candidate load. Expose its exact stamp immediately.
      return core_.read_state();
    }
    return s;
  }
  Status detach() override {
    if (!attached_) return {};
    ++epoch_;
    // Same queue barrier covers Skia submissions and all drawable presentation.
    id<MTLCommandBuffer> barrier = [queue_ commandBuffer];
    if (!barrier)
      return Status::error("ERR_METAL",
                           "cannot allocate the GPU completion barrier; surface remains attached");
    [barrier commit];
    [barrier waitUntilCompleted];
    Buffer b;
    auto status = b.read(weapp_container_sdk_metal_detach(core_.handle, &b.value));
    if (!status) return status;
    attached_ = false;
    last_ = nil;
    dirty_ = true;
    return {};
  }
  Status present() override {
    update();
    if (auto status = complete_presentations(); !status) return status;
    if (!attached_ || !core_.surface_visible ||
        (!dirty_ && revision_ == core_.state.revision && generation_ == core_.state.generation))
      return {};
    @autoreleasepool {
      id<CAMetalDrawable> drawable = [layer_ nextDrawable];
      if (!drawable) return {};
      const auto generation = core_.state.generation, revision = core_.state.revision,
                 epoch = epoch_;
      Buffer b;
      auto s = b.read(weapp_container_sdk_metal_present(
          core_.handle, (__bridge void*)drawable.texture, uint32_t(drawable.texture.width),
          uint32_t(drawable.texture.height), &b.value));
      if (!s) return s;
      id<MTLCommandBuffer> command = [queue_ commandBuffer];
      if (!command)
        return Status::error("ERR_METAL", "failed to allocate presentation command buffer");
      [command presentDrawable:drawable];
      // Capture the drawable and a lifetime-safe mailbox. Only the session's
      // owner thread consumes receipts and calls back into the runtime.
      auto completions = completions_;
      [command addCompletedHandler:^(id<MTLCommandBuffer> buffer) {
        (void)drawable.texture;
        const bool success = buffer.status == MTLCommandBufferStatusCompleted;
        std::string message = success ? "" :
            (buffer.error.localizedDescription.UTF8String ?: "Metal presentation failed");
        std::lock_guard<std::mutex> lock(completions->mutex);
        completions->pending.push_back({generation, revision, epoch, success, std::move(message)});
      }];
      [command commit];
      last_ = command;
      revision_ = revision;
      generation_ = generation;
      dirty_ = false;
    }
    return {};
  }
  void refresh() override { update(); }
  void update() {
    if (!view_) return;
    const NSSize size = view_.bounds.size;
    const bool positive = size.width >= 1 && size.height >= 1;
    const bool visible = positive && view_.window != nil && !view_.isHiddenOrHasHiddenAncestor &&
                         view_.window.visible;
    if (visible != core_.surface_visible) { ++epoch_; dirty_ = true; }
    visibility(visible);
    if (!positive) return;
    NSScreen* screen = view_.window.screen ?: [NSScreen mainScreen];
    if (!screen) return;
    double scale = view_.window ? view_.window.backingScaleFactor : screen.backingScaleFactor;
    Metrics m;
    m.width = uint32_t(std::lround(size.width));
    m.height = uint32_t(std::lround(size.height));
    m.pixel_ratio = scale;
    m.screen_width = uint32_t(std::lround(screen.frame.size.width));
    m.screen_height = uint32_t(std::lround(screen.frame.size.height));
    if (view_.window) m.screen_top = NSMaxY(screen.frame) - NSMaxY(view_.window.frame);
    auto physical = CGSizeMake(std::lround(m.width * scale), std::lround(m.height * scale));
    if (!CGSizeEqualToSize(layer_.drawableSize, physical)) {
      layer_.drawableSize = physical;
      layer_.contentsScale = scale;
      ++epoch_;
      dirty_ = true;
    }
    if (core_.state.status != SessionState::Closed && core_.state.status != SessionState::Closing)
      core_.report(metrics(m));
  }
  void pointer(NSEvent* e, PointerAction action) {
    NSPoint p = [view_ convertPoint:e.locationInWindow fromView:nil];
    core_.report(core_.pointer({action, 0, p.x, p.y}));
  }
  void wheel(NSEvent* e) {
    NSPoint p = [view_ convertPoint:e.locationInWindow fromView:nil];
    double factor = e.hasPreciseScrollingDeltas ? 1 : 20;
    core_.report(core_.wheel({p.x, p.y, -e.scrollingDeltaX * factor, -e.scrollingDeltaY * factor}));
  }
  void key(NSEvent* e, bool down) {
    NSString* characters = e.charactersIgnoringModifiers ?: @"";
    std::string key = characters.UTF8String ?: "";
    if (characters.length) switch ([characters characterAtIndex:0]) {
        case NSLeftArrowFunctionKey:
          key = "ArrowLeft";
          break;
        case NSRightArrowFunctionKey:
          key = "ArrowRight";
          break;
        case NSUpArrowFunctionKey:
          key = "ArrowUp";
          break;
        case NSDownArrowFunctionKey:
          key = "ArrowDown";
          break;
        case 0x1b:
          key = "Escape";
          break;
        case 0x0d:
          key = "Enter";
          break;
        case 0x7f:
          key = "Backspace";
          break;
        case 0x09:
          key = "Tab";
          break;
      }
    auto f = e.modifierFlags;
    core_.report(
        core_.key({down, bool(e.ARepeat), key, "Native" + std::to_string(e.keyCode),
                   bool(f & NSEventModifierFlagShift), bool(f & NSEventModifierFlagControl),
                   bool(f & NSEventModifierFlagOption), bool(f & NSEventModifierFlagCommand)}));
  }
  void cancel() { core_.cancel_touches(); }

 private:
  struct Completion {
    uint64_t generation, revision, epoch;
    bool success;
    std::string message;
  };
  struct Completions {
    std::mutex mutex;
    std::vector<Completion> pending;
  };
  Status complete_presentations() {
    std::vector<Completion> completed;
    {
      std::lock_guard<std::mutex> lock(completions_->mutex);
      completed.swap(completions_->pending);
    }
    for (const auto& item : completed) {
      if (!attached_ || !core_.surface_visible || item.epoch != epoch_) continue;
      if (!item.success) {
        dirty_ = true;
        return Status::error("ERR_METAL", item.message);
      }
      auto result = core_.acknowledge_presentation(item.generation, item.revision);
      if (!result) {
        dirty_ = true;
        if (result.code != "ERR_CANCELLED") return result;
      }
    }
    return {};
  }
  WCDesktopNativeView* view_ = nil;
  CAMetalLayer* layer_ = nil;
  id<MTLDevice> device_ = nil;
  id<MTLCommandQueue> queue_ = nil;
  id<MTLCommandBuffer> last_ = nil;
  bool attached_ = false, dirty_ = true;
  uint64_t revision_ = 0, generation_ = 0, epoch_ = 1;
  std::shared_ptr<Completions> completions_ = std::make_shared<Completions>();
};
std::unique_ptr<Presenter> create_presenter(Core& core) {
  return std::make_unique<MacPresenter>(core);
}
}  // namespace weapp::desktop::detail

@implementation WCDesktopNativeView
- (BOOL)isFlipped {
  return YES;
}
- (BOOL)acceptsFirstResponder {
  return YES;
}
- (BOOL)resignFirstResponder {
  if (self.owner) self.owner->cancel();
  return [super resignFirstResponder];
}
- (void)viewDidMoveToWindow {
  [super viewDidMoveToWindow];
  if (self.owner) self.owner->update();
}
- (void)viewDidChangeBackingProperties {
  [super viewDidChangeBackingProperties];
  if (self.owner) self.owner->update();
}
- (void)setFrameSize:(NSSize)size {
  [super setFrameSize:size];
  if (self.owner) self.owner->update();
}
- (void)setHidden:(BOOL)value {
  [super setHidden:value];
  if (self.owner) self.owner->update();
}
- (void)mouseDown:(NSEvent*)e {
  [self.window makeFirstResponder:self];
  if (self.owner) self.owner->pointer(e, weapp::desktop::PointerAction::Down);
}
- (void)mouseDragged:(NSEvent*)e {
  if (self.owner) self.owner->pointer(e, weapp::desktop::PointerAction::Move);
}
- (void)mouseUp:(NSEvent*)e {
  if (self.owner) self.owner->pointer(e, weapp::desktop::PointerAction::Up);
}
- (void)scrollWheel:(NSEvent*)e {
  if (self.owner) self.owner->wheel(e);
}
- (void)keyDown:(NSEvent*)e {
  if (self.owner) self.owner->key(e, true);
}
- (void)keyUp:(NSEvent*)e {
  if (self.owner) self.owner->key(e, false);
}
@end
