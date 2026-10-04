#include "../src/CubismMaterialTexture.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest/QTest>
#include <algorithm>
#include <cmath>

namespace {
QImage mask(QSize size, uchar value = 0) {
    QImage result(size, QImage::Format_Grayscale8);
    result.fill(value);
    return result;
}

QImage sampleAtlas() {
    QImage result(4, 1, QImage::Format_RGBA8888);
    result.setPixelColor(0, 0, QColor(216, 150, 80, 255));
    result.setPixelColor(1, 0, QColor(80, 140, 230, 173));
    result.setPixelColor(2, 0, QColor(249, 184, 159, 128));
    result.setPixelColor(3, 0, QColor(250, 120, 80, 0));
    return result;
}

int storedAlpha(const QImage& image, int x) { return image.constScanLine(0)[4 * x + 3]; }

void verifyPremultiplied(const QImage& image) {
    QCOMPARE(image.format(), QImage::Format_RGBA8888_Premultiplied);
    for (int y = 0; y < image.height(); ++y) {
        const auto* row = image.constScanLine(y);
        for (int x = 0; x < image.width(); ++x) {
            const int alpha = row[4 * x + 3];
            for (int channel = 0; channel < 3; ++channel) {
                if (row[4 * x + channel] > alpha) {
                    QFAIL("Premultiplied material RGB exceeds its alpha, including transparent edge pixels.");
                }
            }
        }
    }
}
} // namespace

class CubismMaterialTextureTest final : public QObject {
    Q_OBJECT
private slots:
    void binaryOwnershipIsExclusive();
    void duplicatesAreErasedOnlyFromVisibleMaterial();
    void edgeCoverageDoesNotAddOpacityOrColouredHalos();
    void untouchedAtlasAndAliasedOutputArePreserved();
    void invalidMasksLeaveOutputsIntact();
    void neckJawOverlapAndPremultipliedMainArePreserved();
    void neckOutputMayAliasOriginalAtlas();
    void invalidNeckMasksLeaveOutputsIntact();
    void exportedWhaleGirlHasOneNeckAndSeparateClothing();
};

void CubismMaterialTextureTest::binaryOwnershipIsExclusive() {
    const auto atlas = sampleAtlas();
    auto bodyMask = mask(atlas.size());
    bodyMask.scanLine(0)[1] = 255;
    bodyMask.scanLine(0)[3] = 255;
    QImage visible, clothing;
    QString error;
    QVERIFY2(splitCubismMaterialTexture(atlas, bodyMask, mask(atlas.size()),
        &visible, &clothing, &error), qPrintable(error));
    const auto expected = atlas.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    for (int x = 0; x < atlas.width(); ++x) {
        const bool body = bodyMask.constScanLine(0)[x] != 0;
        const auto* kept = (body ? clothing : visible).constScanLine(0) + 4 * x;
        const auto* original = expected.constScanLine(0) + 4 * x;
        for (int channel = 0; channel < 4; ++channel) QCOMPARE(kept[channel], original[channel]);
        QCOMPARE(storedAlpha(body ? visible : clothing, x), 0);
    }
    verifyPremultiplied(visible);
    verifyPremultiplied(clothing);
}

void CubismMaterialTextureTest::duplicatesAreErasedOnlyFromVisibleMaterial() {
    const auto atlas = sampleAtlas();
    auto bodyMask = mask(atlas.size());
    auto eraseMask = mask(atlas.size());
    eraseMask.scanLine(0)[0] = 255; // A redundant static neck must disappear.
    bodyMask.scanLine(0)[1] = 255;
    eraseMask.scanLine(0)[1] = 255; // An erase mark cannot delete body-owned cloth.
    QImage visible, clothing;
    QVERIFY(splitCubismMaterialTexture(atlas, bodyMask, eraseMask, &visible, &clothing));
    QCOMPARE(storedAlpha(visible, 0), 0);
    QCOMPARE(storedAlpha(clothing, 0), 0);
    QCOMPARE(storedAlpha(visible, 1), 0);
    QCOMPARE(storedAlpha(clothing, 1), atlas.pixelColor(1, 0).alpha());
    QCOMPARE(visible.pixelColor(2, 0),
        atlas.convertToFormat(QImage::Format_RGBA8888_Premultiplied).pixelColor(2, 0));
}

void CubismMaterialTextureTest::edgeCoverageDoesNotAddOpacityOrColouredHalos() {
    const auto atlas = sampleAtlas();
    auto bodyMask = mask(atlas.size(), 128);
    auto eraseMask = mask(atlas.size());
    eraseMask.scanLine(0)[2] = 220;
    QImage visible, clothing;
    QVERIFY(splitCubismMaterialTexture(atlas, bodyMask, eraseMask, &visible, &clothing));
    for (int x = 0; x < atlas.width(); ++x) {
        const int total = storedAlpha(visible, x) + storedAlpha(clothing, x);
        QVERIFY(total <= atlas.pixelColor(x, 0).alpha() + 1);
        if (x != 2) QVERIFY(std::abs(total - atlas.pixelColor(x, 0).alpha()) <= 1);
    }
    QVERIFY(storedAlpha(visible, 2) < storedAlpha(clothing, 2));
    verifyPremultiplied(visible);
    verifyPremultiplied(clothing);
    // The transferred blue cloth keeps the painted hue after alpha cutting;
    // a second premultiplication would darken its RGB substantially.
    const auto colour = clothing.pixelColor(1, 0);
    const auto original = atlas.pixelColor(1, 0);
    QVERIFY(std::abs(colour.red() - original.red()) <= 2);
    QVERIFY(std::abs(colour.green() - original.green()) <= 2);
    QVERIFY(std::abs(colour.blue() - original.blue()) <= 2);
}

void CubismMaterialTextureTest::untouchedAtlasAndAliasedOutputArePreserved() {
    auto atlas = sampleAtlas();
    const auto original = atlas;
    const auto empty = mask(atlas.size());
    QImage clothing;
    QVERIFY(splitCubismMaterialTexture(atlas, empty, empty, &atlas, &clothing));
    QCOMPARE(atlas, original.convertToFormat(QImage::Format_RGBA8888_Premultiplied));
    for (int x = 0; x < clothing.width(); ++x) QCOMPARE(storedAlpha(clothing, x), 0);
    verifyPremultiplied(clothing);
    // Qt-loaded premultiplied input must produce the same untouched result.
    QImage visible;
    QVERIFY(splitCubismMaterialTexture(atlas, empty, empty, &visible, &clothing));
    QCOMPARE(visible, atlas);
}

void CubismMaterialTextureTest::invalidMasksLeaveOutputsIntact() {
    const auto atlas = sampleAtlas();
    const auto correct = mask(atlas.size());
    QImage visible(1, 1, QImage::Format_RGBA8888);
    visible.fill(QColor(20, 30, 40, 255));
    const auto sentinel = visible;
    QImage clothing = visible;
    QString error;
    const auto verifyFailure = [&] (const QImage& input, const QImage& body, const QImage& erase) {
        error.clear();
        QVERIFY(!splitCubismMaterialTexture(input, body, erase, &visible, &clothing, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(visible, sentinel);
        QCOMPARE(clothing, sentinel);
    };
    verifyFailure(QImage(), correct, correct);
    verifyFailure(atlas, QImage(), correct);
    verifyFailure(atlas, correct, QImage());
    verifyFailure(atlas, mask(QSize(3, 1)), correct);
    verifyFailure(atlas, correct, mask(QSize(4, 2)));
    verifyFailure(atlas, correct.convertToFormat(QImage::Format_RGB32), correct);
    verifyFailure(atlas, correct, correct.convertToFormat(QImage::Format_Indexed8));
    QVERIFY(!splitCubismMaterialTexture(atlas, correct, correct, nullptr, &clothing, &error));
    QVERIFY(!splitCubismMaterialTexture(atlas, correct, correct, &visible, nullptr, &error));
    QVERIFY(!splitCubismMaterialTexture(atlas, correct, correct, &visible, &visible, &error));
    QCOMPARE(visible, sentinel);
    QCOMPARE(clothing, sentinel);
}

void CubismMaterialTextureTest::neckJawOverlapAndPremultipliedMainArePreserved() {
    QImage atlas(5, 1, QImage::Format_RGBA8888);
    atlas.setPixelColor(0, 0, QColor(249, 184, 159, 128)); // Neck, moved from main.
    atlas.setPixelColor(1, 0, QColor(247, 187, 166, 193)); // Jaw overlap, retained in both.
    atlas.setPixelColor(2, 0, QColor(80, 140, 230, 173)); // Fractional antialias coverage.
    atlas.setPixelColor(3, 0, QColor(40, 50, 130, 255)); // Previously transferred cloth.
    atlas.setPixelColor(4, 0, QColor(211, 140, 130, 217)); // Unchanged main artwork.
    auto bodyMask = mask(atlas.size());
    bodyMask.scanLine(0)[3] = 255;
    QImage visible, clothing;
    QVERIFY(splitCubismMaterialTexture(atlas, bodyMask, mask(atlas.size()), &visible, &clothing));
    const auto previousMain = visible;
    auto neckMask = mask(atlas.size());
    auto neckErase = mask(atlas.size());
    neckMask.scanLine(0)[0] = neckMask.scanLine(0)[1] = 255;
    neckMask.scanLine(0)[2] = 128;
    neckErase.scanLine(0)[0] = 255;
    neckErase.scanLine(0)[2] = 220;
    QImage neck;
    QString error;
    QVERIFY2(splitCubismNeckTexture(atlas, neckMask, neckErase, &visible, &neck, &error), qPrintable(error));
    verifyPremultiplied(visible);
    verifyPremultiplied(neck);
    const auto original = atlas.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    QCOMPARE(storedAlpha(visible, 0), 0);
    QCOMPARE(neck.pixel(0, 0), original.pixel(0, 0));
    QCOMPARE(visible.pixel(1, 0), previousMain.pixel(1, 0));
    QCOMPARE(neck.pixel(1, 0), original.pixel(1, 0));
    // Cut all four already-premultiplied bytes directly. Unpremultiplication
    // followed by a second premultiplication would change these edge samples.
    const auto* oldEdge = previousMain.constScanLine(0) + 8;
    const auto* edge = visible.constScanLine(0) + 8;
    for (int channel = 0; channel < 4; ++channel) {
        QCOMPARE(int(edge[channel]), (int(oldEdge[channel]) * 35 + 127) / 255);
    }
    QCOMPARE(storedAlpha(neck, 2), 87);
    const auto edgeColour = neck.pixelColor(2, 0);
    const auto painted = atlas.pixelColor(2, 0);
    QVERIFY(std::abs(edgeColour.red() - painted.red()) <= 2);
    QVERIFY(std::abs(edgeColour.green() - painted.green()) <= 2);
    QVERIFY(std::abs(edgeColour.blue() - painted.blue()) <= 2);
    QCOMPARE(visible.pixel(3, 0), previousMain.pixel(3, 0)); // Do not restore erased body pixels.
    QCOMPARE(storedAlpha(neck, 3), 0);
    QCOMPARE(visible.pixel(4, 0), previousMain.pixel(4, 0));
    QCOMPARE(storedAlpha(neck, 4), 0);
}

void CubismMaterialTextureTest::neckOutputMayAliasOriginalAtlas() {
    auto atlas = sampleAtlas();
    const auto original = atlas;
    QImage visible = atlas.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    const auto entire = mask(atlas.size(), 255);
    QVERIFY(splitCubismNeckTexture(atlas, entire, entire, &visible, &atlas));
    QCOMPARE(atlas, original.convertToFormat(QImage::Format_RGBA8888_Premultiplied));
    for (int x = 0; x < visible.width(); ++x) QCOMPARE(storedAlpha(visible, x), 0);
    verifyPremultiplied(visible);
    verifyPremultiplied(atlas);
}

void CubismMaterialTextureTest::invalidNeckMasksLeaveOutputsIntact() {
    const auto atlas = sampleAtlas();
    const auto correct = mask(atlas.size());
    QImage visible = atlas.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    const auto visibleSentinel = visible;
    QImage neck(1, 1, QImage::Format_RGBA8888);
    neck.fill(QColor(20, 30, 40, 255));
    const auto neckSentinel = neck;
    QString error;
    const auto verifyFailure = [&] (const QImage& input, const QImage& kept, const QImage& erased) {
        error.clear();
        QVERIFY(!splitCubismNeckTexture(input, kept, erased, &visible, &neck, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(visible, visibleSentinel);
        QCOMPARE(neck, neckSentinel);
    };
    verifyFailure(QImage(), correct, correct);
    verifyFailure(atlas, QImage(), correct);
    verifyFailure(atlas, correct, QImage());
    verifyFailure(atlas, mask(QSize(3, 1)), correct);
    verifyFailure(atlas, correct, mask(QSize(4, 2)));
    verifyFailure(atlas, correct.convertToFormat(QImage::Format_RGB32), correct);
    verifyFailure(atlas, correct, correct.convertToFormat(QImage::Format_Indexed8));
    QVERIFY(!splitCubismNeckTexture(atlas, correct, correct, nullptr, &neck, &error));
    QVERIFY(!splitCubismNeckTexture(atlas, correct, correct, &visible, nullptr, &error));
    QVERIFY(!splitCubismNeckTexture(atlas, correct, correct, &visible, &visible, &error));
    QCOMPARE(visible, visibleSentinel);
    QCOMPARE(neck, neckSentinel);
    for (const auto badMain : {QImage(), QImage(3, 1, QImage::Format_RGBA8888_Premultiplied),
                              visible.convertToFormat(QImage::Format_RGBA8888)}) {
        visible = badMain;
        QVERIFY(!splitCubismNeckTexture(atlas, correct, correct, &visible, &neck, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(visible, badMain);
        QCOMPARE(neck, neckSentinel);
    }
}

void CubismMaterialTextureTest::exportedWhaleGirlHasOneNeckAndSeparateClothing() {
    const QDir directory(QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("assets/live2d/whale-girl")));
    const auto read = [&directory](const QString& relative) {
        QFile file(directory.filePath(relative));
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    };
    const auto metadataBytes = read(QStringLiteral("whale-girl-layered-draft.psd2live.json"));
    const auto modelBytes = read(QStringLiteral("whale-girl-layered-draft.model3.json"));
    QVERIFY(!metadataBytes.isEmpty());
    QVERIFY(!modelBytes.isEmpty());
    const auto separation = QJsonDocument::fromJson(metadataBytes).object()
        .value(QStringLiteral("runtimeMaterialSeparation")).toObject();
    const auto references = QJsonDocument::fromJson(modelBytes).object()
        .value(QStringLiteral("FileReferences")).toObject();
    QVERIFY(!separation.isEmpty());
    const auto ownershipBytes = read(separation.value(QStringLiteral("ownership")).toString());
    QVERIFY(!ownershipBytes.isEmpty());
    const auto ownership = QJsonDocument::fromJson(ownershipBytes).object();
    const auto neckPath = ownership.value(QStringLiteral("neck_mask")).toString();
    const auto neckErasePath = ownership.value(QStringLiteral("neck_erase_mask")).toString();
    QVERIFY(!neckPath.isEmpty() && !neckErasePath.isEmpty());
    const auto neckBytes = read(neckPath);
    const auto neckEraseBytes = read(neckErasePath);
    QVERIFY(!neckBytes.isEmpty() && !neckEraseBytes.isEmpty());
    const auto textures = references.value(QStringLiteral("Textures")).toArray();
    const int textureIndex = separation.value(QStringLiteral("textureIndex")).toInt(-1);
    QVERIFY(textureIndex >= 0 && textureIndex < textures.size());
    const auto atlasBytes = read(textures[textureIndex].toString());
    const auto bodyBytes = read(separation.value(QStringLiteral("bodyMask")).toString());
    const auto eraseBytes = read(separation.value(QStringLiteral("duplicateMask")).toString());
    const auto mocBytes = read(references.value(QStringLiteral("Moc")).toString());
    const auto underpaintPath = separation.value(QStringLiteral("underpaint")).toString();
    QVERIFY(!underpaintPath.isEmpty());
    const auto underpaintBytes = read(underpaintPath);
    QVERIFY(!atlasBytes.isEmpty() && !bodyBytes.isEmpty() && !eraseBytes.isEmpty() && !mocBytes.isEmpty());
    QVERIFY(!underpaintBytes.isEmpty());
    const auto hash = [](const QByteArray& bytes) {
        return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    };
    // The source-coordinate charts belong to this exact exported model and
    // atlas. A re-export must regenerate masks before this regression passes.
    QCOMPARE(hash(mocBytes), ownership.value(QStringLiteral("moc_sha256")).toString());
    QCOMPARE(hash(atlasBytes), ownership.value(QStringLiteral("atlas_sha256")).toString());
    QCOMPARE(hash(bodyBytes), ownership.value(QStringLiteral("body_mask_sha256")).toString());
    QCOMPARE(hash(eraseBytes), ownership.value(QStringLiteral("duplicate_mask_sha256")).toString());
    QCOMPARE(hash(underpaintBytes), ownership.value(QStringLiteral("underpaint_sha256")).toString());
    QCOMPARE(hash(neckBytes), ownership.value(QStringLiteral("neck_mask_sha256")).toString());
    QCOMPARE(hash(neckEraseBytes), ownership.value(QStringLiteral("neck_erase_mask_sha256")).toString());
    const auto atlas = QImage::fromData(atlasBytes).convertToFormat(QImage::Format_RGBA8888);
    const auto bodyMask = QImage::fromData(bodyBytes).convertToFormat(QImage::Format_Grayscale8);
    const auto eraseMask = QImage::fromData(eraseBytes).convertToFormat(QImage::Format_Grayscale8);
    const auto underpaint = QImage::fromData(underpaintBytes).convertToFormat(QImage::Format_RGBA8888);
    const auto neckMask = QImage::fromData(neckBytes).convertToFormat(QImage::Format_Grayscale8);
    const auto neckEraseMask = QImage::fromData(neckEraseBytes).convertToFormat(QImage::Format_Grayscale8);
    QVERIFY(!atlas.isNull() && !bodyMask.isNull() && !eraseMask.isNull());
    QVERIFY(!underpaint.isNull());
    const auto atlasSize = ownership.value(QStringLiteral("atlas_size")).toArray();
    QCOMPARE(atlasSize.size(), 2);
    QCOMPARE(atlas.size(), QSize(atlasSize[0].toInt(), atlasSize[1].toInt()));
    QCOMPARE(underpaint.size(), atlas.size());
    QCOMPARE(neckMask.size(), atlas.size());
    QCOMPARE(neckEraseMask.size(), atlas.size());
    verifyPremultiplied(underpaint.convertToFormat(QImage::Format_RGBA8888_Premultiplied));
    QImage visible, clothing;
    QString error;
    QVERIFY2(splitCubismMaterialTexture(atlas, bodyMask, eraseMask,
        &visible, &clothing, &error), qPrintable(error));
    const auto beforeNeck = visible;
    QImage neck;
    QVERIFY2(splitCubismNeckTexture(atlas, neckMask, neckEraseMask,
        &visible, &neck, &error), qPrintable(error));
    verifyPremultiplied(visible);
    verifyPremultiplied(neck);
    const auto original = atlas.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
    const auto charts = ownership.value(QStringLiteral("source_charts")).toObject();
    const auto atlasPoint = [&charts](const QString& source, QPoint point) {
        const auto offset = charts.value(source).toObject().value(QStringLiteral("offset")).toArray();
        return QPoint(point.x() - offset[0].toInt(), point.y() - offset[1].toInt());
    };
    const auto verifySample = [&](const QString& source, QPoint sourcePoint, int visibleOwner,
                                  int bodyOwner, int neckOwner = 0) {
        QVERIFY(charts.contains(source));
        const auto chart = charts.value(source).toObject();
        QCOMPARE(chart.value(QStringLiteral("offset")).toArray().size(), 2);
        const auto point = atlasPoint(source, sourcePoint);
        const auto rect = chart.value(QStringLiteral("atlas")).toArray();
        QCOMPARE(rect.size(), 4);
        QVERIFY(point.x() >= rect[0].toInt() && point.x() < rect[2].toInt());
        QVERIFY(point.y() >= rect[1].toInt() && point.y() < rect[3].toInt());
        const int alpha = original.pixelColor(point).alpha();
        QVERIFY2(alpha > 0, qPrintable(source + QStringLiteral(" regression sample must contain painted pixels")));
        QCOMPARE(visible.pixelColor(point).alpha(), visibleOwner ? alpha : 0);
        QCOMPARE(clothing.pixelColor(point).alpha(), bodyOwner ? alpha : 0);
        QCOMPARE(neck.pixelColor(point).alpha(), neckOwner ? alpha : 0);
        if (visibleOwner) QCOMPARE(visible.pixel(point), original.pixel(point));
        if (bodyOwner) QCOMPARE(clothing.pixel(point), original.pixel(point));
        if (neckOwner) QCOMPARE(neck.pixel(point), original.pixel(point));
    };
    verifySample(QStringLiteral("face"), QPoint(628, 550), 1, 0); // Moving chin over hidden cloth support.
    verifySample(QStringLiteral("face"), QPoint(628, 578), 0, 0, 1); // Skin owns the flexible neck only.
    verifySample(QStringLiteral("face"), QPoint(628, 568), 1, 0, 1); // Jaw anchor overlap, same head endpoint.
    verifySample(QStringLiteral("face"), QPoint(628, 569), 1, 0, 1);
    verifySample(QStringLiteral("face"), QPoint(628, 591), 0, 0, 1); // Preserve the dark collar-tip contour.
    verifySample(QStringLiteral("face"), QPoint(555, 565), 1, 0); // Moving dark jaw contour.
    verifySample(QStringLiteral("face"), QPoint(632, 603), 0, 1); // Body-owned collar/brooch.
    verifySample(QStringLiteral("busy torso"), QPoint(628, 575), 0, 0); // Redundant static neck.
    verifySample(QStringLiteral("front hair"), QPoint(560, 580), 0, 0); // Navy cloth misclassified as hair.
    verifySample(QStringLiteral("arm backing"), QPoint(555, 560), 0, 0); // Redundant jaw backing.
    verifySample(QStringLiteral("arm backing"), QPoint(560, 580), 0, 0); // Redundant moving collar.
    verifySample(QStringLiteral("busy torso"), QPoint(621, 617), 1, 0); // Chest below the seated bow.
    const auto faceRect = charts.value(QStringLiteral("face")).toObject()
        .value(QStringLiteral("atlas")).toArray();
    QCOMPARE(faceRect.size(), 4);
    int neckMaskSamples = 0;
    int neckEraseSamples = 0;
    int overlappingSkinSamples = 0;
    int neckWarmSamples = 0;
    for (int y = 0; y < neckMask.height(); ++y) {
        const auto* kept = neckMask.constScanLine(y);
        const auto* erased = neckEraseMask.constScanLine(y);
        const auto* sourceRow = atlas.constScanLine(y);
        for (int x = 0; x < neckMask.width(); ++x) {
            if (!kept[x] && !erased[x]) continue;
            QVERIFY(kept[x] == 0 || kept[x] == 255);
            QVERIFY(erased[x] == 0 || erased[x] == 255);
            QVERIFY(!erased[x] || kept[x] == 255);
            QVERIFY(x >= faceRect[0].toInt() && x < faceRect[2].toInt());
            QVERIFY(y >= faceRect[1].toInt() && y < faceRect[3].toInt());
            const auto offset = charts.value(QStringLiteral("face")).toObject()
                .value(QStringLiteral("offset")).toArray();
            const int sourceY = y + offset[1].toInt();
            QVERIFY(sourceY >= 568 && sourceY <= 591);
            if (erased[x]) QVERIFY(sourceY >= 570);
            const auto* painted = sourceRow + 4 * x;
            if (!painted[3]) continue;
            if (kept[x]) ++neckMaskSamples;
            if (erased[x]) ++neckEraseSamples;
            if (kept[x] && !erased[x]) {
                ++overlappingSkinSamples;
                QVERIFY(sourceY == 568 || sourceY == 569);
                QCOMPARE(visible.pixel(x, y), beforeNeck.pixel(x, y));
                QCOMPARE(neck.pixel(x, y), original.pixel(x, y));
            }
            if (painted[0] > 200 && painted[1] > 140 && painted[2] > 115
                && int(painted[0]) > int(painted[2]) + 18) ++neckWarmSamples;
        }
    }
    const auto flexible = ownership.value(QStringLiteral("flexible_neck")).toObject();
    const auto flexibleValidation = flexible.value(QStringLiteral("validation")).toObject();
    QCOMPARE(neckMaskSamples, flexible.value(QStringLiteral("visible_pixel_count")).toInt());
    QCOMPARE(neckEraseSamples, flexible.value(QStringLiteral("erased_from_main")).toObject()
        .value(QStringLiteral("visible_pixel_count")).toInt());
    QCOMPARE(overlappingSkinSamples, flexibleValidation.value(QStringLiteral("jaw_overlap_visible_pixel_count")).toInt());
    QCOMPARE(neckWarmSamples, flexibleValidation.value(QStringLiteral("original_warm_skin_pixel_count")).toInt());
    QVERIFY(neckMaskSamples > 500 && neckEraseSamples > 500 && overlappingSkinSamples > 100);
    QVERIFY(neckWarmSamples > 400);
    int underpaintSamples = 0;
    for (int y = 0; y < underpaint.height(); ++y) {
        const auto* row = underpaint.constScanLine(y);
        for (int x = 0; x < underpaint.width(); ++x) {
            const auto* pixel = row + 4 * x;
            if (!pixel[3]) continue;
            ++underpaintSamples;
            QVERIFY(x >= faceRect[0].toInt() && x < faceRect[2].toInt());
            QVERIFY(y >= faceRect[1].toInt() && y < faceRect[3].toInt());
            // The support surface is sampled navy fabric, never another
            // stationary cheek/neck underneath the real moving skin.
            QVERIFY(!(pixel[0] > 200 && pixel[1] > 140 && pixel[2] > 115
                && pixel[0] > pixel[2] + 18));
        }
    }
    QVERIFY(underpaintSamples > 1000);
    for (const auto sourcePoint : {QPoint(628, 550), QPoint(628, 578), QPoint(628, 605)}) {
        const auto support = underpaint.pixelColor(atlasPoint(QStringLiteral("face"), sourcePoint));
        QCOMPARE(support.alpha(), 255);
        QVERIFY(support.blue() > support.red() + 20);
        QVERIFY(support.blue() > support.green());
    }
    int skinSamples = 0;
    for (int y = 540; y <= 589; ++y) {
        for (int x = 510; x <= 742; ++x) {
            const auto point = atlasPoint(QStringLiteral("face"), QPoint(x, y));
            const auto colour = atlas.pixelColor(point);
            if (colour.alpha() > 0 && colour.red() > 200 && colour.green() > 140
                && colour.blue() > 115 && colour.red() > colour.blue() + 18) {
                ++skinSamples;
                QCOMPARE(clothing.pixelColor(point).alpha(), 0);
                const bool flexibleOwner = neckMask.constScanLine(point.y())[point.x()] != 0;
                const bool mainErased = neckEraseMask.constScanLine(point.y())[point.x()] != 0;
                QCOMPARE(neck.pixelColor(point).alpha(), flexibleOwner ? colour.alpha() : 0);
                if (mainErased) QCOMPARE(visible.pixelColor(point).alpha(), 0);
                else QCOMPARE(visible.pixel(point), original.pixel(point));
                if (flexibleOwner) QCOMPARE(neck.pixel(point), original.pixel(point));
            }
        }
    }
    QVERIFY(skinSamples > 1000);
    // Above the two-row jaw overlap, the entire face/eyes/side-hair chart
    // stays exactly as it was before the neck pass was separated. This also
    // proves that a broad rectangular cut did not eat the cheek or blue hair.
    const auto faceOffset = charts.value(QStringLiteral("face")).toObject()
        .value(QStringLiteral("offset")).toArray();
    int preservedFaceSamples = 0;
    for (int y = faceRect[1].toInt(); y < faceRect[3].toInt(); ++y) {
        if (y + faceOffset[1].toInt() >= 568) continue;
        for (int x = faceRect[0].toInt(); x < faceRect[2].toInt(); ++x) {
            QCOMPARE(neckMask.constScanLine(y)[x], uchar(0));
            QCOMPARE(neckEraseMask.constScanLine(y)[x], uchar(0));
            QCOMPARE(neck.pixelColor(x, y).alpha(), 0);
            QCOMPARE(visible.pixel(x, y), beforeNeck.pixel(x, y));
            if (original.pixelColor(x, y).alpha()) ++preservedFaceSamples;
        }
    }
    QVERIFY(preservedFaceSamples > 10000);
    // The erase boundary must preserve the entire seated chest under the bow,
    // including warm painted skin that resembles the duplicated neck colours.
    const auto busy = charts.value(QStringLiteral("busy torso")).toObject();
    const auto busyRect = busy.value(QStringLiteral("atlas")).toArray();
    const auto busyOffset = busy.value(QStringLiteral("offset")).toArray();
    int chestSamples = 0;
    for (int y = busyRect[1].toInt(); y < busyRect[3].toInt(); ++y) {
        if (y + busyOffset[1].toInt() < 607) continue;
        for (int x = busyRect[0].toInt(); x < busyRect[2].toInt(); ++x) {
            QCOMPARE(clothing.pixelColor(x, y).alpha(), 0);
            QCOMPARE(neck.pixelColor(x, y).alpha(), 0);
            QCOMPARE(visible.pixel(x, y), original.pixel(x, y));
            if (original.pixelColor(x, y).alpha() > 0) ++chestSamples;
        }
    }
    QVERIFY(chestSamples > 1000);
}

QTEST_GUILESS_MAIN(CubismMaterialTextureTest)
#include "CubismMaterialTextureTest.moc"
