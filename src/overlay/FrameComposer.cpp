#include "FrameComposer.h"

#include "FrameOverlay.h"

#include <QPainter>

void FrameComposer::add(std::shared_ptr<FrameOverlay> layer)
{
    layers_.push_back(std::move(layer));
}

bool FrameComposer::active(const FrameContext& ctx) const
{
    for (const auto& layer : layers_)
        if (layer->active(ctx)) return true;
    return false;
}

QImage FrameComposer::compose(const QImage& base, const FrameContext& ctx) const
{
    if (base.isNull() || !active(ctx)) return base;
    // QImage is copy-on-write, so this copies only when a layer is actually
    // being drawn over the frame.
    QImage composed = base;
    QPainter painter(&composed);
    for (const auto& layer : layers_)
        if (layer->active(ctx)) layer->draw(painter, composed.size(), ctx);
    return composed;
}
