#include "CubismMaterialTexture.h"

#include <algorithm>
#include <utility>

bool splitCubismMaterialTexture(const QImage& atlas, const QImage& bodyMask,
                                const QImage& eraseMask, QImage* visible,
                                QImage* clothing, QString* error) {
    if (error) error->clear();
    const auto fail = [error](const QString& reason) {
        if (error) *error = reason;
        return false;
    };
    if (!visible || !clothing || visible == clothing) {
        return fail(QStringLiteral("Material textures require two separate outputs."));
    }
    if (atlas.isNull() || bodyMask.isNull() || eraseMask.isNull()) {
        return fail(QStringLiteral("Material atlas and ownership masks must not be empty."));
    }
    if (bodyMask.size() != atlas.size() || eraseMask.size() != atlas.size()) {
        return fail(QStringLiteral("Material ownership mask dimensions must match the atlas."));
    }
    if (bodyMask.format() != QImage::Format_Grayscale8
        || eraseMask.format() != QImage::Format_Grayscale8) {
        return fail(QStringLiteral("Material ownership masks must use Grayscale8."));
    }

    // Split straight alpha, then premultiply once for the Cubism GL renderer.
    // Cutting already-premultiplied alpha alone leaves coloured edge halos.
    const QImage source = atlas.convertToFormat(QImage::Format_RGBA8888);
    QImage head = source.copy();
    QImage body = source.copy();
    if (source.isNull() || head.isNull() || body.isNull()) {
        return fail(QStringLiteral("Could not allocate material texture images."));
    }
    for (int y = 0; y < source.height(); ++y) {
        const auto* sourceRow = source.constScanLine(y);
        const auto* bodyRow = bodyMask.constScanLine(y);
        const auto* eraseRow = eraseMask.constScanLine(y);
        auto* headRow = head.scanLine(y);
        auto* clothingRow = body.scanLine(y);
        for (int x = 0; x < source.width(); ++x) {
            const int alpha = sourceRow[4 * x + 3];
            const int bodyCoverage = bodyRow[x];
            const int removedCoverage = std::max(bodyCoverage, int(eraseRow[x]));
            clothingRow[4 * x + 3] = static_cast<uchar>((alpha * bodyCoverage + 127) / 255);
            headRow[4 * x + 3] = static_cast<uchar>((alpha * (255 - removedCoverage) + 127) / 255);
        }
    }
    head = head.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    body = body.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    if (head.isNull() || body.isNull()) {
        return fail(QStringLiteral("Could not premultiply material texture images."));
    }
    // Stage both results before assignment so an output may safely be the
    // original atlas variable. Errors never replace a caller's images.
    *visible = std::move(head);
    *clothing = std::move(body);
    return true;
}

bool splitCubismNeckTexture(const QImage& atlas, const QImage& neckMask,
                           const QImage& neckEraseMask, QImage* visible,
                           QImage* neck, QString* error) {
    if (error) error->clear();
    const auto fail = [error](const QString& reason) {
        if (error) *error = reason;
        return false;
    };
    if (!visible || !neck || visible == neck) {
        return fail(QStringLiteral("Neck textures require two separate outputs."));
    }
    if (atlas.isNull() || neckMask.isNull() || neckEraseMask.isNull() || visible->isNull()) {
        return fail(QStringLiteral("Neck atlas, masks and main material must not be empty."));
    }
    if (neckMask.size() != atlas.size() || neckEraseMask.size() != atlas.size()
        || visible->size() != atlas.size()) {
        return fail(QStringLiteral("Neck masks and main material dimensions must match the atlas."));
    }
    if (neckMask.format() != QImage::Format_Grayscale8
        || neckEraseMask.format() != QImage::Format_Grayscale8) {
        return fail(QStringLiteral("Neck ownership masks must use Grayscale8."));
    }
    if (visible->format() != QImage::Format_RGBA8888_Premultiplied) {
        return fail(QStringLiteral("The main neck material must already use RGBA8888_Premultiplied."));
    }

    // Stage both images before writing either output. The neck output may
    // alias the original atlas variable without losing its source pixels.
    const QImage source = atlas.convertToFormat(QImage::Format_RGBA8888);
    QImage head = visible->copy();
    QImage flexible = source.copy();
    if (source.isNull() || head.isNull() || flexible.isNull()) {
        return fail(QStringLiteral("Could not allocate neck texture images."));
    }
    for (int y = 0; y < source.height(); ++y) {
        const auto* sourceRow = source.constScanLine(y);
        const auto* neckRow = neckMask.constScanLine(y);
        const auto* eraseRow = neckEraseMask.constScanLine(y);
        auto* headRow = head.scanLine(y);
        auto* flexibleRow = flexible.scanLine(y);
        for (int x = 0; x < source.width(); ++x) {
            const int kept = 255 - eraseRow[x];
            for (int channel = 0; channel < 4; ++channel) {
                headRow[4 * x + channel] = static_cast<uchar>(
                    (int(headRow[4 * x + channel]) * kept + 127) / 255);
            }
            flexibleRow[4 * x + 3] = static_cast<uchar>(
                (int(sourceRow[4 * x + 3]) * neckRow[x] + 127) / 255);
        }
    }
    flexible = flexible.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    if (flexible.isNull()) {
        return fail(QStringLiteral("Could not premultiply the neck texture image."));
    }
    *visible = std::move(head);
    *neck = std::move(flexible);
    return true;
}
