#include "CubismMaterialTexture.h"

#include <algorithm>
#include <utility>
#include <vector>

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

bool removeCubismBackdropIslands(const QImage& source, const QRect& chart,
                                 QImage* isolated, QString* error, int keepComponents) {
    if (error) error->clear();
    const auto fail = [error](const QString& reason) {
        if (error) *error = reason;
        return false;
    };
    if (!isolated) {
        return fail(QStringLiteral("Backdrop island removal requires an output image."));
    }
    if (keepComponents < 1) {
        return fail(QStringLiteral("Backdrop island removal requires a positive component count."));
    }
    if (source.isNull() || source.format() != QImage::Format_RGBA8888_Premultiplied) {
        return fail(QStringLiteral("Backdrop artwork must use nonempty RGBA8888_Premultiplied."));
    }
    if (chart.width() <= 0 || chart.height() <= 0 || chart.x() < 0 || chart.y() < 0
        || chart.width() > source.width() || chart.height() > source.height()
        || chart.x() > source.width() - chart.width()
        || chart.y() > source.height() - chart.height()) {
        return fail(QStringLiteral("Backdrop chart must have positive size and stay inside its source."));
    }

    const int width = chart.width(), height = chart.height();
    const std::size_t count = static_cast<std::size_t>(width) * height;
    std::vector<uchar> alpha(count);
    std::vector<int> component(count, -1);
    for (int y = 0; y < height; ++y) {
        const auto* row = source.constScanLine(chart.y() + y);
        for (int x = 0; x < width; ++x)
            alpha[static_cast<std::size_t>(y) * width + x] = row[4 * (chart.x() + x) + 3];
    }
    const auto visitNeighbours = [width, height](std::size_t index, const auto& visit) {
        const int x = static_cast<int>(index % width), y = static_cast<int>(index / width);
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                if (dx == 0 && dy == 0) continue;
                const int nx = x + dx, ny = y + dy;
                if (nx >= 0 && nx < width && ny >= 0 && ny < height)
                    visit(static_cast<std::size_t>(ny) * width + nx);
            }
        }
    };

    // Select ownership using opaque-enough cores. A faint antialias fringe
    // must not turn a small fragment into one of the retained artwork islands.
    std::vector<std::size_t> areas, queue;
    for (std::size_t seed = 0; seed < count; ++seed) {
        if (alpha[seed] < 16 || component[seed] >= 0) continue;
        const int label = static_cast<int>(areas.size());
        queue.clear();
        queue.push_back(seed);
        component[seed] = label;
        for (std::size_t head = 0; head < queue.size(); ++head) {
            visitNeighbours(queue[head], [&](std::size_t neighbour) {
                if (alpha[neighbour] < 16 || component[neighbour] >= 0) return;
                component[neighbour] = label;
                queue.push_back(neighbour);
            });
        }
        areas.push_back(queue.size());
    }
    if (areas.size() < static_cast<std::size_t>(keepComponents)) {
        return fail(QStringLiteral("Backdrop chart requires at least %1 strong-alpha components.")
            .arg(keepComponents));
    }
    std::vector<std::size_t> ranked(areas.size());
    for (std::size_t i = 0; i < ranked.size(); ++i) ranked[i] = i;
    std::partial_sort(ranked.begin(), ranked.begin() + keepComponents, ranked.end(),
        [&areas](std::size_t left, std::size_t right) {
            return areas[left] != areas[right] ? areas[left] > areas[right] : left < right;
        });
    std::vector<uchar> selected(areas.size(), 0);
    for (int i = 0; i < keepComponents; ++i) selected[ranked[i]] = 1;

    // Expand from the chosen cores through all painted alpha. This retains
    // their full 8-connected antialias edges, including alpha=1 pixels.
    std::vector<uchar> keep(count, 0);
    queue.clear();
    for (std::size_t i = 0; i < count; ++i) {
        if (component[i] < 0 || !selected[component[i]]) continue;
        keep[i] = 1;
        queue.push_back(i);
    }
    for (std::size_t head = 0; head < queue.size(); ++head) {
        visitNeighbours(queue[head], [&](std::size_t neighbour) {
            if (!alpha[neighbour] || keep[neighbour]) return;
            keep[neighbour] = 1;
            queue.push_back(neighbour);
        });
    }

    QImage cleaned = source.copy();
    if (cleaned.isNull()) {
        return fail(QStringLiteral("Could not allocate the isolated backdrop texture."));
    }
    for (int y = 0; y < height; ++y) {
        auto* row = cleaned.scanLine(chart.y() + y);
        for (int x = 0; x < width; ++x) {
            if (keep[static_cast<std::size_t>(y) * width + x]) continue;
            auto* pixel = row + 4 * (chart.x() + x);
            std::fill_n(pixel, 4, uchar(0));
        }
    }
    *isolated = std::move(cleaned);
    return true;
}
