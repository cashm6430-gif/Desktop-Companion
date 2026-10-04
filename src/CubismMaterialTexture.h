#pragma once

#include <QImage>
#include <QString>

// Separate body-owned artwork and erase duplicate backing pixels without
// changing their painted colours. Masks use source-atlas coordinates.
bool splitCubismMaterialTexture(const QImage& atlas, const QImage& bodyMask,
                                const QImage& eraseMask, QImage* visible,
                                QImage* clothing, QString* error = nullptr);

// Move the original skin into the flexible neck pass. visible already holds
// the premultiplied main material; cutting all four channels avoids another
// unpremultiply/premultiply cycle at the photographed jaw edges. The neck is
// cut from the original atlas, with an explicit jaw overlap mask.
bool splitCubismNeckTexture(const QImage& atlas, const QImage& neckMask,
                           const QImage& neckEraseMask, QImage* visible,
                           QImage* neck, QString* error = nullptr);
