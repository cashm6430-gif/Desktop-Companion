#include "MemoOverlay.h"

#include <QFontMetrics>
#include <QLinearGradient>
#include <QPainter>
#include <QTextLayout>
#include <algorithm>

namespace memo_overlay {
namespace {
struct Palette {
    QColor top, bottom, border, accent;
};

Palette palette(Mark mark) {
    switch (mark) {
    case Mark::Duration: return {0xfffcff, 0xf2f0fc, 0xcdc9e6, 0x5f5a8d};
    case Mark::Done: return {0xf8fdfb, 0xeaf8f2, 0xbcdfd2, 0x397a66};
    default: return {0xfcfdff, 0xecf4ff, 0xbacde8, 0x426797};
    }
}

struct Type {
    QFont body, caption;
    double padding, lineStep, headerHeight, sectionGap;
};

Type typography(const QSizeF& canvas) {
    const double unit = qMin(canvas.width(), canvas.height());
    const int bodyPx = std::clamp(qRound(unit * 0.045), 11, 19);
    const int captionPx = std::clamp(qRound(bodyPx * 0.8), 9, 14);
    const double spacing = std::clamp(unit / 280.0, 0.7, 1.0);
    Type type{QFont(QStringLiteral("Microsoft YaHei UI")), QFont(QStringLiteral("Microsoft YaHei UI")),
              std::clamp(unit * 0.029, 5.0, 13.0),
              bodyPx + 4 * spacing, captionPx + 3 * spacing, 3 * spacing};
    type.body.setPixelSize(bodyPx);
    type.caption.setPixelSize(captionPx);
    type.caption.setWeight(QFont::DemiBold);
    return type;
}

void drawSurface(QPainter& painter, const QRectF& rect, double radius, const Palette& colors) {
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    for (int i = 2; i >= 0; --i) {
        const double blur = 0.75 * i;
        painter.setBrush(QColor(22, 45, 83, 8 + (2 - i) * 6));
        painter.drawRoundedRect(rect.translated(0, 1.25).adjusted(-blur, -blur, blur, blur),
                                radius + blur, radius + blur);
    }
    QLinearGradient fill(rect.topLeft(), rect.bottomLeft());
    fill.setColorAt(0.0, colors.top);
    fill.setColorAt(1.0, colors.bottom);
    painter.setBrush(fill);
    painter.setPen(QPen(colors.border, 1.0));
    painter.drawRoundedRect(rect.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
}

void drawMark(QPainter& painter, const QRectF& rect, Mark mark, const QColor& color) {
    painter.save();
    painter.translate(rect.topLeft());
    painter.scale(rect.width() / 16.0, rect.height() / 16.0);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(color, 1.45, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    if (mark == Mark::Note) {
        painter.drawRoundedRect(QRectF(3.1, 1.8, 9.8, 12.4), 1.8, 1.8);
        painter.drawLine(QLineF(5.6, 6.3, 10.4, 6.3));
        painter.drawLine(QLineF(5.6, 9.5, 9.2, 9.5));
    } else if (mark == Mark::Duration) {
        painter.drawEllipse(QRectF(2, 2, 12, 12));
        painter.drawPolyline(QPolygonF{{8, 4.7}, {8, 8}, {10.4, 9.4}});
    } else {
        painter.drawPolyline(QPolygonF{{3.1, 8.1}, {6.5, 11.4}, {12.9, 4.7}});
    }
    painter.restore();
}
} // namespace

QRectF cardRect(const QSizeF& canvas, double rise, const QString& text) {
    const auto type = typography(canvas);
    const double margin = type.padding + 1.0;
    const double limit = std::clamp(canvas.width() * 0.47, 92.0, 224.0);
    const double minimum = std::clamp(canvas.width() * 0.29, 82.0, 126.0);
    const double textWidth = QFontMetricsF(type.body).horizontalAdvance(text.simplified());
    const double width = std::clamp(textWidth + 2 * type.padding + 1.0, minimum, limit);
    const double usableWidth = width - 2 * type.padding;
    const int lines = textWidth > usableWidth ? 2 : 1;
    const double height = 2 * type.padding + type.headerHeight + type.sectionGap + lines * type.lineStep;
    const double top = margin + canvas.height() * 0.041 * (1.0 - rise);
    return {canvas.width() - margin - width, top, width, height};
}

QRectF badgeRect(const QSizeF& canvas, bool desk) {
    const double side = std::clamp(canvas.width() * 0.09, 22.0, 40.0);
    const double margin = std::clamp(canvas.width() * 0.035, 7.0, 14.0);
    return {canvas.width() - margin - side, canvas.height() * (desk ? 0.49 : 0.55), side, side};
}

void drawCard(QPainter& painter, const QSizeF& canvas, const QRectF& rect,
              const QString& text, Mark mark) {
    const auto colors = palette(mark);
    const auto type = typography(canvas);
    const QRectF inside = rect.adjusted(type.padding, type.padding, -type.padding, -type.padding);
    painter.save();
    drawSurface(painter, rect, std::clamp(rect.width() * 0.071, 7.0, 14.0), colors);
    const double iconSize = type.headerHeight - 1;
    drawMark(painter, QRectF(inside.topLeft(), QSizeF(iconSize, iconSize)), mark, colors.accent);
    painter.setFont(type.caption);
    painter.setPen(colors.accent);
    painter.drawText(QRectF(inside.left() + iconSize + 4, inside.top(),
                            inside.width() - iconSize - 4, type.headerHeight),
                     Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
                     mark == Mark::Duration ? QStringLiteral("回合耗时") : QStringLiteral("便笺"));

    const QString summary = text.simplified();
    QTextLayout layout(summary, type.body);
    QTextOption options;
    options.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(options);
    const QPointF bodyOrigin(inside.left(), inside.top() + type.headerHeight + type.sectionGap);
    const int lineLimit = qRound((inside.bottom() - bodyOrigin.y()) / type.lineStep);
    layout.beginLayout();
    for (int i = 0; i < lineLimit; ++i) {
        auto line = layout.createLine();
        if (!line.isValid()) break;
        line.setLineWidth(inside.width());
        line.setPosition(QPointF(0, i * type.lineStep));
    }
    layout.endLayout();
    painter.setFont(type.body);
    painter.setPen(QColor(0x263b55));
    for (int i = 0; i < layout.lineCount(); ++i) {
        const auto line = layout.lineAt(i);
        if (i + 1 == lineLimit && line.textStart() + line.textLength() < summary.size()) {
            const auto remainder = summary.mid(line.textStart());
            const QString shortened = QFontMetricsF(type.body).elidedText(remainder, Qt::ElideRight, inside.width());
            painter.drawText(bodyOrigin + line.position() + QPointF(0, line.ascent()), shortened);
        } else {
            line.draw(&painter, bodyOrigin);
        }
    }
    painter.restore();
}

void drawBadge(QPainter& painter, const QRectF& rect, Mark mark) {
    const auto colors = palette(mark);
    painter.save();
    drawSurface(painter, rect, rect.width() * 0.32, colors);
    const double inset = rect.width() * 0.22;
    drawMark(painter, rect.adjusted(inset, inset, -inset, -inset), mark, colors.accent);
    painter.restore();
}
} // namespace memo_overlay
