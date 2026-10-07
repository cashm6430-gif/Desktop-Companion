#pragma once

#include <QSize>
#include <functional>
#include <type_traits>
#include <utility>

class QPainter;

// Per-composition context. Carries values that vary per call (the live player
// and the offline review sequences composite with different bubble pulses).
struct FrameContext {
    double bubblePulse = 0.0;
};

// One compositable window-layer graphic drawn over the rendered pet frame.
//
// active() and draw() are deliberately declared side by side: the composer
// asks every layer whether it would draw before it decides to copy the base
// frame at all. A new layer therefore cannot be registered without its
// visibility guard -- the old hand-written early return in
// PetWindow::frameWithBubble had to be updated in lockstep with each new draw
// call, and forgetting it once left the sleep pillow permanently invisible.
class FrameOverlay {
public:
    virtual ~FrameOverlay() = default;
    // True when this layer would change the frame under `ctx`.
    virtual bool active(const FrameContext& ctx) const = 0;
    virtual void draw(QPainter& painter, const QSize& size, const FrameContext& ctx) const = 0;
};

// Adapter that turns two callables into a FrameOverlay, so PetWindow can
// register its layers inline next to the state they read.
class FunctionOverlay final : public FrameOverlay {
public:
    using ActiveFn = std::function<bool(const FrameContext&)>;
    using DrawFn = std::function<void(QPainter&, const QSize&, const FrameContext&)>;

    FunctionOverlay(ActiveFn active, DrawFn draw)
        : active_(std::move(active)), draw_(std::move(draw)) {}
    bool active(const FrameContext& ctx) const override { return active_(ctx); }
    void draw(QPainter& painter, const QSize& size, const FrameContext& ctx) const override
    {
        draw_(painter, size, ctx);
    }

private:
    ActiveFn active_;
    DrawFn draw_;
};
