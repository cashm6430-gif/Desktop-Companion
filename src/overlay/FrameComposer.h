#pragma once

#include <QImage>
#include <memory>
#include <vector>

#include "FrameOverlay.h"

class FrameOverlay;

// Composites an ordered list of window-layer overlays onto the rendered pet
// frame. Owns the single copy decision and the painter boilerplate: callers
// hand in a base frame and receive either the identical frame (no layer
// active, no copy) or the composited result. Layer order = registration
// order = paint order.
class FrameComposer {
public:
    void add(std::shared_ptr<FrameOverlay> layer);
    // True when any registered layer would draw under `ctx`.
    bool active(const FrameContext& ctx) const;
    // Returns `base` unchanged when no layer is active.
    QImage compose(const QImage& base, const FrameContext& ctx) const;

private:
    std::vector<std::shared_ptr<FrameOverlay>> layers_;
};
