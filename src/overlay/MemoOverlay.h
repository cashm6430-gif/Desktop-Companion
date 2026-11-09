#pragma once

#include <QRectF>
#include <QString>

class QPainter;

namespace memo_overlay {
enum class Mark { Note, Duration, Done };

QRectF cardRect(const QSizeF& canvas, double rise, const QString& text);
QRectF badgeRect(const QSizeF& canvas, bool desk);
void drawCard(QPainter& painter, const QSizeF& canvas, const QRectF& rect,
              const QString& text, Mark mark);
void drawBadge(QPainter& painter, const QRectF& rect, Mark mark);
}
