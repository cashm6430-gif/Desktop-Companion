#include "../src/CubismNeckBinding.h"
#include "../src/CubismPostureTransition.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QtTest/QTest>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace {
struct NativeModel {
    std::vector<unsigned char> mocStorage, modelStorage;
    Core::csmModel* model = nullptr;

    static void* aligned(std::vector<unsigned char>& storage, std::size_t size, std::size_t alignment) {
        storage.resize(size + alignment - 1);
        const auto address = reinterpret_cast<std::uintptr_t>(storage.data());
        return reinterpret_cast<void*>((address + alignment - 1) & ~(alignment - 1));
    }
    bool load(const QByteArray& bytes) {
        if (bytes.isEmpty()) return false;
        void* address = aligned(mocStorage, bytes.size(), Core::csmAlignofMoc);
        std::memcpy(address, bytes.constData(), bytes.size());
        // A malformed auxiliary export must never reach the Native revive
        // call; successful author export alone does not prove consistency.
        if (Core::csmHasMocConsistency(address, static_cast<unsigned int>(bytes.size())) != 1) return false;
        auto* moc = Core::csmReviveMocInPlace(address, static_cast<unsigned int>(bytes.size()));
        if (!moc) return false;
        const auto size = Core::csmGetSizeofModel(moc);
        if (!size) return false;
        model = Core::csmInitializeModelInPlace(moc, aligned(modelStorage, size, Core::csmAlignofModel), size);
        if (model) Core::csmUpdateModel(model);
        return model != nullptr;
    }
};

int drawable(Core::csmModel* model, const QJsonObject& metadata, const QString& source) {
    QString id;
    for (const auto& value : metadata.value(QStringLiteral("layers")).toArray()) {
        const auto layer = value.toObject();
        if (layer.value(QStringLiteral("source")).toString() == source) id = layer.value(QStringLiteral("drawable")).toString();
    }
    for (int i = 0; i < Core::csmGetDrawableCount(model); ++i) {
        if (QString::fromUtf8(Core::csmGetDrawableIds(model)[i]) == id) return i;
    }
    return -1;
}

bool setParameter(Core::csmModel* model, const char* name, float value) {
    const auto* ids = Core::csmGetParameterIds(model);
    for (int i = 0; i < Core::csmGetParameterCount(model); ++i) {
        if (std::strcmp(ids[i], name) == 0) {
            Core::csmGetParameterValues(model)[i] = std::clamp(value,
                Core::csmGetParameterMinimumValues(model)[i], Core::csmGetParameterMaximumValues(model)[i]);
            return true;
        }
    }
    return false;
}

template<class T> QByteArray bytes(const T* data, int count) {
    return QByteArray(reinterpret_cast<const char*>(data), count * qsizetype(sizeof(T)));
}

QByteArray snapshot(Core::csmModel* model) {
    QByteArray result = bytes(Core::csmGetParameterValues(model), Core::csmGetParameterCount(model));
    result += bytes(Core::csmGetPartOpacities(model), Core::csmGetPartCount(model));
    const int count = Core::csmGetDrawableCount(model);
    result += bytes(Core::csmGetDrawableOpacities(model), count);
    result += bytes(Core::csmGetDrawableMultiplyColors(model), count);
    result += bytes(Core::csmGetDrawableScreenColors(model), count);
    result += bytes(Core::csmGetDrawableDynamicFlags(model), count);
    result += bytes(Core::csmGetDrawableDrawOrders(model), count);
    result += bytes(Core::csmGetRenderOrders(model), count + Core::csmGetOffscreenCount(model));
    const auto* positions = Core::csmGetDrawableVertexPositions(model);
    const auto* vertexCounts = Core::csmGetDrawableVertexCounts(model);
    for (int mesh = 0; mesh < count; ++mesh) result += bytes(positions[mesh], vertexCounts[mesh]);
    return result;
}

QByteArray geometry(Core::csmModel* model) {
    QByteArray result;
    const auto* positions = Core::csmGetDrawableVertexPositions(model);
    const auto* counts = Core::csmGetDrawableVertexCounts(model);
    for (int mesh = 0; mesh < Core::csmGetDrawableCount(model); ++mesh) result += bytes(positions[mesh], counts[mesh]);
    return result;
}

struct Point { double x = 0, y = 0; };

Point sourcePoint(const Core::csmVector2& uv, const QJsonObject& ownership, const QString& source) {
    const auto size = ownership.value(QStringLiteral("atlas_size")).toArray();
    const auto offset = ownership.value(QStringLiteral("source_charts")).toObject()
        .value(source).toObject().value(QStringLiteral("offset")).toArray();
    return {uv.X * size[0].toDouble() + offset[0].toDouble(),
            (1.0 - uv.Y) * size[1].toDouble() + offset[1].toDouble()};
}

double area(Point a, Point b, Point c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

// Independently locate source pixels in UV triangles; do not use the binding's
// cached indices or weights to verify its endpoint and upper seam contracts.
Point sample(Core::csmModel* model, const QJsonObject& metadata, const QJsonObject& ownership,
             const QString& source, Point target, const Core::csmVector2* overridePositions = nullptr) {
    const int mesh = drawable(model, metadata, source);
    const auto* uvs = Core::csmGetDrawableVertexUvs(model)[mesh];
    const auto* positions = overridePositions ? overridePositions : Core::csmGetDrawableVertexPositions(model)[mesh];
    const auto* indices = Core::csmGetDrawableIndices(model)[mesh];
    const int indexCount = Core::csmGetDrawableIndexCounts(model)[mesh];
    for (int i = 0; i < indexCount; i += 3) {
        const int ai = indices[i], bi = indices[i + 1], ci = indices[i + 2];
        const Point a = sourcePoint(uvs[ai], ownership, source);
        const Point b = sourcePoint(uvs[bi], ownership, source);
        const Point c = sourcePoint(uvs[ci], ownership, source);
        const double determinant = area(a, b, c);
        if (std::abs(determinant) < 1e-8) continue;
        const double wa = area(target, b, c) / determinant;
        const double wb = area(a, target, c) / determinant;
        const double wc = 1 - wa - wb;
        if (std::min({wa, wb, wc}) < -1e-7) continue;
        return {wa * positions[ai].X + wb * positions[bi].X + wc * positions[ci].X,
                wa * positions[ai].Y + wb * positions[bi].Y + wc * positions[ci].Y};
    }
    return {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::quiet_NaN()};
}

Point sampleSurface(Core::csmModel* surface, const QJsonArray& sourcePoints,
                    Point target, const Core::csmVector2* positions) {
    const auto* indices = Core::csmGetDrawableIndices(surface)[0];
    const auto point = [&](int vertex) {
        const auto source = sourcePoints[vertex].toArray();
        return Point{source[0].toDouble(), source[1].toDouble()};
    };
    for (int i = 0; i < Core::csmGetDrawableIndexCounts(surface)[0]; i += 3) {
        const int ai = indices[i], bi = indices[i + 1], ci = indices[i + 2];
        const Point a = point(ai), b = point(bi), c = point(ci);
        const double determinant = area(a, b, c);
        if (std::abs(determinant) < 1e-8) continue;
        const double wa = area(target, b, c) / determinant;
        const double wb = area(a, target, c) / determinant;
        const double wc = 1 - wa - wb;
        if (std::min({wa, wb, wc}) < -1e-7) continue;
        return {wa * positions[ai].X + wb * positions[bi].X + wc * positions[ci].X,
                wa * positions[ai].Y + wb * positions[bi].Y + wc * positions[ci].Y};
    }
    return {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::quiet_NaN()};
}

void comparePoint(Point actual, Point expected) {
    QVERIFY(std::isfinite(actual.x));
    QVERIFY(std::isfinite(actual.y));
    QVERIFY2(std::abs(actual.x - expected.x) < 1e-6,
        qPrintable(QStringLiteral("X: %1 != %2").arg(actual.x, 0, 'g', 12).arg(expected.x, 0, 'g', 12)));
    QVERIFY2(std::abs(actual.y - expected.y) < 1e-6,
        qPrintable(QStringLiteral("Y: %1 != %2").arg(actual.y, 0, 'g', 12).arg(expected.y, 0, 'g', 12)));
}

void groundSeated(Core::csmModel* model, const QJsonObject& metadata) {
    const auto floor = [&](const QString& left, const QString& right) {
        float result = std::numeric_limits<float>::infinity();
        for (const auto& source : {left, right}) {
            const int mesh = drawable(model, metadata, source);
            const auto* vertices = Core::csmGetDrawableVertexPositions(model)[mesh];
            for (int i = 0; i < Core::csmGetDrawableVertexCounts(model)[mesh]; ++i) result = std::min(result, vertices[i].Y);
        }
        return result;
    };
    const float shift = floor(QStringLiteral("footwear-l"), QStringLiteral("footwear-r"))
        - floor(QStringLiteral("busy leg l"), QStringLiteral("busy leg r"));
    // This deliberately happens before binding.update, matching Canvas's
    // per-mesh sole correction rather than shifting the whole character.
    for (const auto& value : metadata.value(QStringLiteral("layers")).toArray()) {
        const auto source = value.toObject().value(QStringLiteral("source")).toString();
        if (!source.startsWith(QStringLiteral("busy "))) continue;
        const int mesh = drawable(model, metadata, source);
        auto* positions = const_cast<Core::csmVector2*>(Core::csmGetDrawableVertexPositions(model)[mesh]);
        for (int i = 0; i < Core::csmGetDrawableVertexCounts(model)[mesh]; ++i) positions[i].Y += shift;
    }
}

bool runtimePosture(Core::csmModel* model, const QJsonObject& metadata,
                    const QJsonObject& ownership, const QJsonObject& parameters,
                    double* material) {
    const auto transition = CubismPostureTransition::sample(
        parameters.value(QStringLiteral("ParamBusyLaptop")).toDouble(),
        parameters.value(QStringLiteral("ParamSitPose")).toDouble());
    if (!transition.valid || !material) return false;
    *material = transition.nativeMaterial();
    std::copy_n(Core::csmGetParameterDefaultValues(model), Core::csmGetParameterCount(model), Core::csmGetParameterValues(model));
    std::fill_n(Core::csmGetPartOpacities(model), Core::csmGetPartCount(model), 1.0f);
    for (auto it = parameters.constBegin(); it != parameters.constEnd(); ++it) {
        const auto name = it.key().toUtf8();
        if (!setParameter(model, name.constData(), static_cast<float>(it.value().toDouble()))) return false;
    }
    if (!setParameter(model, "ParamBusyLaptop", static_cast<float>(*material))
        || !setParameter(model, "ParamSitPose", static_cast<float>(transition.standingFold))) return false;
    int bodyPart = -1;
    for (int part = 0; part < Core::csmGetPartCount(model); ++part) {
        if (std::strcmp(Core::csmGetPartIds(model)[part], "PartBody") == 0) bodyPart = part;
    }
    if (bodyPart < 0) return false;
    Core::csmGetPartOpacities(model)[bodyPart] = static_cast<float>(1 - *material);
    Core::csmUpdateModel(model);
    groundSeated(model, metadata);
    float floor = std::numeric_limits<float>::infinity();
    for (const auto& source : {QStringLiteral("footwear-l"), QStringLiteral("footwear-r")}) {
        const int mesh = drawable(model, metadata, source);
        const auto* positions = Core::csmGetDrawableVertexPositions(model)[mesh];
        for (int i = 0; i < Core::csmGetDrawableVertexCounts(model)[mesh]; ++i) floor = std::min(floor, positions[i].Y);
    }
    const Point standing = sample(model, metadata, ownership, QStringLiteral("topwear"), {628, 588});
    const Point seated = sample(model, metadata, ownership, QStringLiteral("busy torso"), {628, 588});
    CubismPostureTransition::Point target;
    if (!CubismPostureTransition::collarTarget({standing.x, standing.y}, {seated.x, seated.y}, transition.seatedMix, &target)) return false;
    const CubismPostureTransition::FloorPinnedTransform standingMap{{standing.x, standing.y}, target, floor};
    const CubismPostureTransition::FloorPinnedTransform seatedMap{{seated.x, seated.y}, target, floor};
    if (!standingMap.valid() || !seatedMap.valid()) return false;
    const auto* parents = Core::csmGetDrawableParentPartIndices(model);
    for (const auto& value : metadata.value(QStringLiteral("layers")).toArray()) {
        const QString source = value.toObject().value(QStringLiteral("source")).toString();
        const int mesh = drawable(model, metadata, source);
        const bool seatedMesh = source.startsWith(QStringLiteral("busy "));
        if (!seatedMesh && parents[mesh] != bodyPart) continue;
        const auto& transform = seatedMesh ? seatedMap : standingMap;
        auto* positions = const_cast<Core::csmVector2*>(Core::csmGetDrawableVertexPositions(model)[mesh]);
        for (int vertex = 0; vertex < Core::csmGetDrawableVertexCounts(model)[mesh]; ++vertex) {
            CubismPostureTransition::Point mapped;
            if (!transform.apply({positions[vertex].X, positions[vertex].Y}, &mapped)) return false;
            positions[vertex] = {static_cast<float>(mapped.x), static_cast<float>(mapped.y)};
        }
    }
    return true;
}

// Clip each UV triangle to the measured neck band so orientation checks cover
// triangles that cross its seam even if no exported vertex lies in the band.
bool touchesNeckBand(std::vector<Point> polygon) {
    const std::array<double, 4> boundaries{555, 682, 568, 592};
    for (int side = 0; side < 4; ++side) {
        std::vector<Point> clipped;
        if (polygon.empty()) return false;
        const bool vertical = side < 2;
        const bool minimum = side % 2 == 0;
        const auto coordinate = [vertical](Point p) { return vertical ? p.x : p.y; };
        const auto inside = [&](Point p) { return minimum ? coordinate(p) >= boundaries[side] : coordinate(p) <= boundaries[side]; };
        Point previous = polygon.back();
        for (Point current : polygon) {
            if (inside(previous) != inside(current)) {
                const double t = (boundaries[side] - coordinate(previous)) / (coordinate(current) - coordinate(previous));
                clipped.push_back({previous.x + t * (current.x - previous.x), previous.y + t * (current.y - previous.y)});
            }
            if (inside(current)) clipped.push_back(current);
            previous = current;
        }
        polygon = std::move(clipped);
    }
    if (polygon.size() < 3) return false;
    double sum = 0;
    for (std::size_t i = 0; i < polygon.size(); ++i) {
        const auto a = polygon[i], b = polygon[(i + 1) % polygon.size()];
        sum += a.x * b.y - a.y * b.x;
    }
    return std::abs(sum) > 1e-6;
}
} // namespace

class CubismNeckBindingTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        const QDir directory(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("assets/live2d/whale-girl")));
        const auto read = [&](const QString& path) {
            QFile file(directory.filePath(path));
            return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        };
        moc_ = read(QStringLiteral("whale-girl-layered-draft.moc3"));
        QVERIFY(!moc_.isEmpty());
        metadata_ = QJsonDocument::fromJson(read(QStringLiteral("whale-girl-layered-draft.psd2live.json"))).object();
        const auto separation = metadata_.value(QStringLiteral("runtimeMaterialSeparation")).toObject();
        ownership_ = QJsonDocument::fromJson(read(separation.value(QStringLiteral("ownership")).toString())).object();
        QVERIFY(!ownership_.isEmpty());
        const auto declaration = metadata_.value(QStringLiteral("runtimeNeckConnection")).toObject();
        QCOMPARE(declaration.value(QStringLiteral("headSource")).toString(), QStringLiteral("face"));
        QCOMPARE(declaration.value(QStringLiteral("standingSource")).toString(), QStringLiteral("topwear"));
        QCOMPARE(declaration.value(QStringLiteral("seatedSource")).toString(), QStringLiteral("busy torso"));
        QCOMPARE(declaration.value(QStringLiteral("headAnchor")).toArray(), QJsonArray({628, 588}));
        QCOMPARE(declaration.value(QStringLiteral("standingAnchor")).toArray(), QJsonArray({628, 588}));
        QCOMPARE(declaration.value(QStringLiteral("seatedAnchor")).toArray(), QJsonArray({628, 588}));
        QCOMPARE(declaration.value(QStringLiteral("stretchStartY")).toDouble(), 570.0);
        const QString fixtureDirectory = qEnvironmentVariable("DESKTOP_COMPANION_NECK_TEST_FIXTURE");
        const auto component = declaration.value(QStringLiteral("independentSurface")).toObject();
        if (!fixtureDirectory.isEmpty()) {
            // An isolated, preflight-verified component can be tested before
            // adoption without copying it into the production asset folder.
            const QDir fixture(fixtureDirectory);
            surfaceModelPath_ = fixture.filePath(QStringLiteral("neck-surface.model3.json"));
            surfaceRigPath_ = fixture.filePath(QStringLiteral("neck-surface.rig.json"));
        } else if (!component.isEmpty()) {
            surfaceModelPath_ = directory.filePath(component.value(QStringLiteral("model")).toString());
            surfaceRigPath_ = directory.filePath(component.value(QStringLiteral("rig")).toString());
        }
    }

    void invalidConfigurationDisablesPreviousBinding() {
        NativeModel native;
        QVERIFY(native.load(moc_));
        CubismNeckBinding binding;
        QString error;
        QVERIFY(!binding.configure(nullptr, metadata_, ownership_, &error));
        QVERIFY(!error.isEmpty());
        for (int kind = 0; kind < 10; ++kind) {
            QVERIFY2(binding.configure(native.model, metadata_, ownership_, &error), qPrintable(error));
            QVERIFY(binding.update(1, &error));
            auto metadata = metadata_, ownership = ownership_;
            auto declaration = metadata.value(QStringLiteral("runtimeNeckConnection")).toObject();
            if (kind == 0) metadata.remove(QStringLiteral("runtimeNeckConnection"));
            if (kind == 1) declaration.insert(QStringLiteral("headSource"), QStringLiteral("unknown head"));
            if (kind == 2) declaration.insert(QStringLiteral("headAnchor"), QJsonArray{628});
            if (kind == 3) declaration.insert(QStringLiteral("headAnchor"), QJsonArray{-1000, 588});
            if (kind == 4) declaration.insert(QStringLiteral("stretchStartY"), 588);
            if (kind == 5) declaration.insert(QStringLiteral("stretchStartY"), QStringLiteral("570"));
            if (kind == 6) ownership.insert(QStringLiteral("atlas_size"), QJsonArray{0, 4096});
            if (kind == 7) {
                auto layers = metadata.value(QStringLiteral("layers")).toArray();
                for (const auto& layer : metadata_.value(QStringLiteral("layers")).toArray()) {
                    if (layer.toObject().value(QStringLiteral("source")).toString() == QStringLiteral("face")) layers.append(layer);
                }
                metadata.insert(QStringLiteral("layers"), layers);
            }
            if (kind == 8 || kind == 9) {
                auto charts = ownership.value(QStringLiteral("source_charts")).toObject();
                auto chart = charts.value(QStringLiteral("face")).toObject();
                if (kind == 8) chart.insert(QStringLiteral("drawable"), QStringLiteral("ArtMeshTopwear"));
                else chart.insert(QStringLiteral("offset"), QJsonArray{QStringLiteral("bad"), -866});
                charts.insert(QStringLiteral("face"), chart);
                ownership.insert(QStringLiteral("source_charts"), charts);
            }
            if (kind != 0) metadata.insert(QStringLiteral("runtimeNeckConnection"), declaration);
            const auto before = snapshot(native.model);
            QVERIFY(!binding.configure(native.model, metadata, ownership, &error));
            QVERIFY(!error.isEmpty());
            QCOMPARE(binding.faceDrawableIndex(), -1);
            QVERIFY(binding.positions().empty());
            QVERIFY(!binding.update(1, &error));
            QCOMPARE(snapshot(native.model), before);
        }
    }

    void endpointsStayAttachedAcrossGroundingAndHeadMotion_data() {
        QTest::addColumn<double>("mix");
        QTest::addColumn<float>("mirror");
        QTest::newRow("default-standing") << 0.0 << 0.0f;
        QTest::newRow("standing-turn-right") << 0.0 << 1.0f;
        QTest::newRow("standing-turn-left") << 0.0 << -1.0f;
        QTest::newRow("quarter-seated-right") << 0.25 << 1.0f;
        QTest::newRow("half-seated-left") << 0.5 << -1.0f;
        QTest::newRow("three-quarter-seated-right") << 0.75 << 1.0f;
        QTest::newRow("seated-default") << 1.0 << 0.0f;
        QTest::newRow("seated-turn-right") << 1.0 << 1.0f;
        QTest::newRow("seated-turn-left") << 1.0 << -1.0f;
    }

    void endpointsStayAttachedAcrossGroundingAndHeadMotion() {
        QFETCH(double, mix);
        QFETCH(float, mirror);
        NativeModel native;
        QVERIFY(native.load(moc_));
        CubismNeckBinding binding;
        QString error;
        QVERIFY2(binding.configure(native.model, metadata_, ownership_, &error), qPrintable(error));
        QVERIFY(setParameter(native.model, "ParamAngleX", mirror * 30));
        QVERIFY(setParameter(native.model, "ParamAngleY", mirror * 20));
        QVERIFY(setParameter(native.model, "ParamAngleZ", mirror * 12));
        QVERIFY(setParameter(native.model, "ParamBodyAngleX", mirror * 10));
        QVERIFY(setParameter(native.model, "ParamBodyAngleY", mirror * 6));
        QVERIFY(setParameter(native.model, "ParamBodyAngleZ", mirror * 5));
        QVERIFY(setParameter(native.model, "ParamShiftX", mirror * 25));
        QVERIFY(setParameter(native.model, "ParamBusyLaptop", static_cast<float>(mix)));
        QVERIFY(setParameter(native.model, "ParamSitPose", static_cast<float>(mix > 0 ? 0.93 + 0.07 * mix : 0)));
        Core::csmUpdateModel(native.model);
        groundSeated(native.model, metadata_);
        const auto before = snapshot(native.model);
        QVERIFY2(binding.update(mix, &error), qPrintable(error));
        QCOMPARE(snapshot(native.model), before);
        const int mesh = binding.faceDrawableIndex();
        QCOMPARE(mesh, drawable(native.model, metadata_, QStringLiteral("face")));
        QCOMPARE(binding.positions().size(), static_cast<std::size_t>(Core::csmGetDrawableVertexCounts(native.model)[mesh]));
        const Point standing = sample(native.model, metadata_, ownership_, QStringLiteral("topwear"), {628, 588});
        const Point seated = sample(native.model, metadata_, ownership_, QStringLiteral("busy torso"), {628, 588});
        const Point target{standing.x + mix * (seated.x - standing.x), standing.y + mix * (seated.y - standing.y)};
        comparePoint(sample(native.model, metadata_, ownership_, QStringLiteral("face"), {628, 588}, binding.positions().data()), target);
        for (double x : {575.0, 610.0, 628.0, 650.0}) {
            comparePoint(sample(native.model, metadata_, ownership_, QStringLiteral("face"), {x, 570}, binding.positions().data()),
                sample(native.model, metadata_, ownership_, QStringLiteral("face"), {x, 570}));
        }
        const auto first = bytes(binding.positions().data(), static_cast<int>(binding.positions().size()));
        QVERIFY(binding.update(mix, &error));
        QCOMPARE(bytes(binding.positions().data(), static_cast<int>(binding.positions().size())), first);
        QCOMPARE(snapshot(native.model), before);

        const auto* old = Core::csmGetDrawableVertexPositions(native.model)[mesh];
        const auto* uvs = Core::csmGetDrawableVertexUvs(native.model)[mesh];
        const auto* indices = Core::csmGetDrawableIndices(native.model)[mesh];
        int measuredTriangles = 0;
        for (int i = 0; i < Core::csmGetDrawableIndexCounts(native.model)[mesh]; i += 3) {
            const int ai = indices[i], bi = indices[i + 1], ci = indices[i + 2];
            if (!touchesNeckBand({sourcePoint(uvs[ai], ownership_, QStringLiteral("face")),
                    sourcePoint(uvs[bi], ownership_, QStringLiteral("face")), sourcePoint(uvs[ci], ownership_, QStringLiteral("face"))})) continue;
            const double oldArea = area({old[ai].X, old[ai].Y}, {old[bi].X, old[bi].Y}, {old[ci].X, old[ci].Y});
            const auto& positions = binding.positions();
            const double newArea = area({positions[ai].X, positions[ai].Y}, {positions[bi].X, positions[bi].Y}, {positions[ci].X, positions[ci].Y});
            QVERIFY2(std::abs(oldArea) > 1e-10, qPrintable(QStringLiteral("Original neck triangle %1 is degenerate").arg(i / 3)));
            QVERIFY2(newArea / oldArea > 0.02, qPrintable(QStringLiteral("Neck triangle %1 flips or collapses: area ratio=%2").arg(i / 3).arg(newArea / oldArea)));
            ++measuredTriangles;
        }
        QVERIFY(measuredTriangles > 0);
        qInfo() << "neck-band triangles checked:" << measuredTriangles;
    }

    void repeatedTransitionsDoNotAccumulateGeometry() {
        NativeModel native;
        QVERIFY(native.load(moc_));
        CubismNeckBinding binding;
        QString error;
        QVERIFY2(binding.configure(native.model, metadata_, ownership_, &error), qPrintable(error));
        QByteArray first;
        for (int frame = 0; frame <= 60; ++frame) {
            const double phase = frame * (2.0 * 3.14159265358979323846 / 60);
            const double mix = frame == 60 ? 0 : 0.5 * (1 - std::cos(phase));
            QVERIFY(setParameter(native.model, "ParamAngleX", static_cast<float>(frame == 60 ? 0 : 30 * std::sin(phase))));
            QVERIFY(setParameter(native.model, "ParamAngleZ", static_cast<float>(frame == 60 ? 0 : 12 * std::sin(phase))));
            QVERIFY(setParameter(native.model, "ParamBodyAngleX", static_cast<float>(frame == 60 ? 0 : 10 * std::sin(phase))));
            QVERIFY(setParameter(native.model, "ParamBusyLaptop", static_cast<float>(mix)));
            QVERIFY(setParameter(native.model, "ParamSitPose", static_cast<float>(mix)));
            Core::csmUpdateModel(native.model);
            groundSeated(native.model, metadata_);
            const auto before = snapshot(native.model);
            QVERIFY2(binding.update(mix, &error), qPrintable(error));
            QCOMPARE(snapshot(native.model), before);
            const auto actual = bytes(binding.positions().data(), static_cast<int>(binding.positions().size()));
            if (frame == 0) first = actual;
            if (frame == 60) QCOMPARE(actual, first);
            QVERIFY(binding.update(mix, &error));
            QCOMPARE(bytes(binding.positions().data(), static_cast<int>(binding.positions().size())), actual);
        }
        for (double invalid : {-0.01, 1.01, std::numeric_limits<double>::quiet_NaN()}) {
            const auto before = snapshot(native.model);
            QVERIFY(!binding.update(invalid, &error));
            QVERIFY(!error.isEmpty());
            QVERIFY(binding.positions().empty());
            QCOMPARE(snapshot(native.model), before);
        }
        QVERIFY(binding.update(1, &error));
        QVERIFY(!binding.positions().empty());
    }

    void postureMaterialsMeetWithoutGhostingOrGeometryDrift() {
        using TransitionPoint = CubismPostureTransition::Point;
        const int expectedStandingMeshes = 11, expectedSeatedMeshes = 8;
        for (float mirror : {0.0f, 1.0f, -1.0f}) {
            NativeModel native;
            QVERIFY(native.load(moc_));
            CubismNeckBinding binding;
            QString error;
            QVERIFY2(binding.configure(native.model, metadata_, ownership_, &error), qPrintable(error));
            int bodyPart = -1;
            for (int part = 0; part < Core::csmGetPartCount(native.model); ++part) {
                if (std::strcmp(Core::csmGetPartIds(native.model)[part], "PartBody") == 0) bodyPart = part;
            }
            QVERIFY(bodyPart >= 0);
            const int meshCount = Core::csmGetDrawableCount(native.model);
            const auto* parents = Core::csmGetDrawableParentPartIndices(native.model);
            const auto* counts = Core::csmGetDrawableVertexCounts(native.model);
            std::vector<bool> seatedMeshes(meshCount, false);
            for (const auto& value : metadata_.value(QStringLiteral("layers")).toArray()) {
                const auto source = value.toObject().value(QStringLiteral("source")).toString();
                if (source.startsWith(QStringLiteral("busy "))) seatedMeshes[drawable(native.model, metadata_, source)] = true;
            }
            const auto footFloor = [&](const QString& left, const QString& right) {
                float floor = std::numeric_limits<float>::infinity();
                for (const QString& source : {left, right}) {
                    const int mesh = drawable(native.model, metadata_, source);
                    const auto* positions = Core::csmGetDrawableVertexPositions(native.model)[mesh];
                    for (int vertex = 0; vertex < counts[mesh]; ++vertex) floor = std::min(floor, positions[vertex].Y);
                }
                return floor;
            };
            // Entry, early cancellation, both sides of the material decision,
            // fully folded exit, and the return to the original standing pose.
            const std::array<std::array<double, 2>, 10> poses{{
                {0, 0}, {0.55, 0.75}, {0.8, 0.8}, {0.834999, 0.834999},
                {0.835001, 0.835001}, {0.834999, 0.99}, {0.835001, 0.99},
                {0.9, 0.99}, {1, 1}, {0, 0}
            }};
            QByteArray originalStanding;
            for (std::size_t poseIndex = 0; poseIndex < poses.size(); ++poseIndex) {
                const auto transition = CubismPostureTransition::sample(poses[poseIndex][0], poses[poseIndex][1]);
                QVERIFY(transition.valid);
                QByteArray firstNative, firstAligned, firstNeck;
                for (int repeat = 0; repeat < 3; ++repeat) {
                    std::copy_n(Core::csmGetParameterDefaultValues(native.model), Core::csmGetParameterCount(native.model),
                                Core::csmGetParameterValues(native.model));
                    std::fill_n(Core::csmGetPartOpacities(native.model), Core::csmGetPartCount(native.model), 1.0f);
                    QVERIFY(setParameter(native.model, "ParamAngleX", mirror * 30));
                    QVERIFY(setParameter(native.model, "ParamAngleY", mirror * 20));
                    QVERIFY(setParameter(native.model, "ParamAngleZ", mirror * 12));
                    QVERIFY(setParameter(native.model, "ParamBodyAngleX", mirror * 10));
                    QVERIFY(setParameter(native.model, "ParamBodyAngleY", mirror * 6));
                    QVERIFY(setParameter(native.model, "ParamBodyAngleZ", mirror * 5));
                    QVERIFY(setParameter(native.model, "ParamShiftX", mirror * 25));
                    QVERIFY(setParameter(native.model, "ParamBusyLaptop", static_cast<float>(transition.nativeMaterial())));
                    QVERIFY(setParameter(native.model, "ParamSitPose", static_cast<float>(transition.standingFold)));
                    Core::csmGetPartOpacities(native.model)[bodyPart] = static_cast<float>(1 - transition.nativeMaterial());
                    Core::csmUpdateModel(native.model);
                    // A new Native update must discard all previous per-frame
                    // corrections, even after crossing the material boundary.
                    if (repeat == 0) firstNative = geometry(native.model);
                    else QCOMPARE(geometry(native.model), firstNative);
                    groundSeated(native.model, metadata_);
                    const float floor = footFloor(QStringLiteral("footwear-l"), QStringLiteral("footwear-r"));
                    QCOMPARE(footFloor(QStringLiteral("busy leg l"), QStringLiteral("busy leg r")), floor);
                    std::array<double, 2> standing{}, seated{};
                    const auto beforeRead = snapshot(native.model);
                    QVERIFY2(binding.collarPositions(&standing, &seated, &error), qPrintable(error));
                    QCOMPARE(snapshot(native.model), beforeRead);
                    comparePoint({standing[0], standing[1]}, sample(native.model, metadata_, ownership_, QStringLiteral("topwear"), {628, 588}));
                    comparePoint({seated[0], seated[1]}, sample(native.model, metadata_, ownership_, QStringLiteral("busy torso"), {628, 588}));
                    TransitionPoint target;
                    QVERIFY(CubismPostureTransition::collarTarget({standing[0], standing[1]}, {seated[0], seated[1]}, transition.seatedMix, &target));
                    const CubismPostureTransition::FloorPinnedTransform standingMap{{standing[0], standing[1]}, target, floor};
                    const CubismPostureTransition::FloorPinnedTransform seatedMap{{seated[0], seated[1]}, target, floor};
                    QVERIFY(standingMap.valid());
                    QVERIFY(seatedMap.valid());
                    std::vector<std::vector<Core::csmVector2>> before(meshCount);
                    int standingCount = 0, seatedCount = 0;
                    for (int mesh = 0; mesh < meshCount; ++mesh) {
                        auto* positions = const_cast<Core::csmVector2*>(Core::csmGetDrawableVertexPositions(native.model)[mesh]);
                        before[mesh].assign(positions, positions + counts[mesh]);
                        if (parents[mesh] != bodyPart && !seatedMeshes[mesh]) continue;
                        const auto& transform = seatedMeshes[mesh] ? seatedMap : standingMap;
                        if (seatedMeshes[mesh]) ++seatedCount;
                        else ++standingCount;
                        for (int vertex = 0; vertex < counts[mesh]; ++vertex) {
                            TransitionPoint mapped;
                            QVERIFY(transform.apply({positions[vertex].X, positions[vertex].Y}, &mapped));
                            positions[vertex] = {static_cast<float>(mapped.x), static_cast<float>(mapped.y)};
                        }
                    }
                    QCOMPARE(standingCount, expectedStandingMeshes);
                    QCOMPARE(seatedCount, expectedSeatedMeshes);
                    QCOMPARE(footFloor(QStringLiteral("footwear-l"), QStringLiteral("footwear-r")), floor);
                    QCOMPARE(footFloor(QStringLiteral("busy leg l"), QStringLiteral("busy leg r")), floor);
                    comparePoint(sample(native.model, metadata_, ownership_, QStringLiteral("topwear"), {628, 588}), {target.x, target.y});
                    comparePoint(sample(native.model, metadata_, ownership_, QStringLiteral("busy torso"), {628, 588}), {target.x, target.y});
                    for (int mesh = 0; mesh < meshCount; ++mesh) {
                        const auto* current = Core::csmGetDrawableVertexPositions(native.model)[mesh];
                        if (parents[mesh] != bodyPart && !seatedMeshes[mesh]) {
                            // Includes the face, both hair surfaces and the
                            // shared tail; none may inherit the torso warp.
                            QCOMPARE(bytes(current, counts[mesh]), bytes(before[mesh].data(), counts[mesh]));
                            continue;
                        }
                        const auto* indices = Core::csmGetDrawableIndices(native.model)[mesh];
                        int measuredTriangles = 0;
                        for (int i = 0; i < Core::csmGetDrawableIndexCounts(native.model)[mesh]; i += 3) {
                            const int ai = indices[i], bi = indices[i + 1], ci = indices[i + 2];
                            const auto& old = before[mesh];
                            const double oldArea = area({old[ai].X, old[ai].Y}, {old[bi].X, old[bi].Y}, {old[ci].X, old[ci].Y});
                            if (std::abs(oldArea) < 1e-10) continue;
                            const double newArea = area({current[ai].X, current[ai].Y}, {current[bi].X, current[bi].Y}, {current[ci].X, current[ci].Y});
                            QVERIFY2(newArea / oldArea > 0.02, qPrintable(QStringLiteral("Posture mesh %1 triangle %2 flips or collapses").arg(mesh).arg(i / 3)));
                            ++measuredTriangles;
                        }
                        QVERIFY(measuredTriangles > 0);
                    }
                    // The computer has its own visibility track; test the body
                    // torso/collar selectors independently of that held prop.
                    const auto* opacity = Core::csmGetDrawableOpacities(native.model);
                    for (const auto& source : {QStringLiteral("topwear"), QStringLiteral("bottomwear"),
                                              QStringLiteral("footwear-l"), QStringLiteral("footwear-r")}) {
                        QCOMPARE(opacity[drawable(native.model, metadata_, source)], static_cast<float>(1 - transition.nativeMaterial()));
                    }
                    for (const auto& source : {QStringLiteral("busy torso"), QStringLiteral("busy skirt"),
                                              QStringLiteral("busy leg l"), QStringLiteral("busy leg r"),
                                              QStringLiteral("busy sleeve l"), QStringLiteral("busy sleeve r")}) {
                        QCOMPARE(opacity[drawable(native.model, metadata_, source)], static_cast<float>(transition.nativeMaterial()));
                    }
                    const auto beforeNeck = snapshot(native.model);
                    QVERIFY2(binding.update(transition.nativeMaterial(), &error), qPrintable(error));
                    QCOMPARE(snapshot(native.model), beforeNeck);
                    const QString visibleCollar = transition.seatedMaterial ? QStringLiteral("busy torso") : QStringLiteral("topwear");
                    comparePoint(sample(native.model, metadata_, ownership_, QStringLiteral("face"), {628, 588}, binding.positions().data()),
                                 sample(native.model, metadata_, ownership_, visibleCollar, {628, 588}));
                    for (double x : {575.0, 610.0, 628.0, 650.0}) {
                        comparePoint(sample(native.model, metadata_, ownership_, QStringLiteral("face"), {x, 570}, binding.positions().data()),
                                     sample(native.model, metadata_, ownership_, QStringLiteral("face"), {x, 570}));
                    }
                    const auto aligned = geometry(native.model);
                    const auto neck = bytes(binding.positions().data(), static_cast<int>(binding.positions().size()));
                    if (repeat == 0) { firstAligned = aligned; firstNeck = neck; }
                    else { QCOMPARE(aligned, firstAligned); QCOMPARE(neck, firstNeck); }
                }
                if (poseIndex == 0) originalStanding = firstAligned;
                if (poseIndex == poses.size() - 1) QCOMPARE(firstAligned, originalStanding);
            }
        }
    }

    void independentSurfaceHasRealParameterFormsAndNeutralSourceMapping() {
        if (surfaceModelPath_.isEmpty()) QSKIP("No independent component selected; use DESKTOP_COMPANION_NECK_TEST_FIXTURE before adoption.");
        QByteArray surfaceMoc;
        QJsonObject rig;
        QVERIFY2(readSurfaceFixture(&surfaceMoc, &rig), qPrintable(surfaceModelPath_));
        NativeModel main, surface;
        QVERIFY(main.load(moc_));
        QVERIFY2(surface.load(surfaceMoc), "Independent MOC failed consistency/revival preflight.");
        QCOMPARE(Core::csmGetDrawableCount(surface.model), 1);
        QCOMPARE(Core::csmGetDrawableVertexCounts(surface.model)[0], 63);
        QCOMPARE(Core::csmGetDrawableIndexCounts(surface.model)[0], 288);
        const auto sourcePoints = rig.value(QStringLiteral("vertexSourcePoints")).toArray();
        QCOMPARE(sourcePoints.size(), 63);
        Core::csmVector2 canvas{}, origin{};
        float pixelsPerUnit = 0;
        Core::csmReadCanvasInfo(surface.model, &canvas, &origin, &pixelsPerUnit);
        QCOMPARE(pixelsPerUnit, 1254.0f);
        const auto beforeMain = snapshot(main.model);
        CubismNeckBinding binding;
        QString error;
        QVERIFY2(binding.configure(main.model, metadata_, ownership_, &error), qPrintable(error));
        QVERIFY2(binding.configureSurface(surface.model, rig, &error), qPrintable(error));
        QCOMPARE(snapshot(main.model), beforeMain);
        QCOMPARE(binding.surfaceDrawableIndex(), 0);
        const auto* neutral = Core::csmGetDrawableVertexPositions(surface.model)[0];
        const std::vector<Core::csmVector2> baseline(neutral, neutral + 63);
        QVERIFY(setParameter(surface.model, "ParamNeckBottomX", 20));
        QVERIFY(setParameter(surface.model, "ParamNeckBottomY", -10));
        QVERIFY(setParameter(surface.model, "ParamNeckCurve", 1));
        Core::csmUpdateModel(surface.model);
        const auto* deformed = Core::csmGetDrawableVertexPositions(surface.model)[0];
        bool middleCurves = false;
        for (int vertex = 0; vertex < sourcePoints.size(); ++vertex) {
            const auto source = sourcePoints[vertex].toArray();
            const double y = source[1].toDouble();
            const Point displacement{deformed[vertex].X - baseline[vertex].X,
                                     deformed[vertex].Y - baseline[vertex].Y};
            if (y <= 570) comparePoint(displacement, {0, 0});
            if (y >= 588) comparePoint(displacement, {20 / 1254.0, -10 / 1254.0});
            if (y == 578) {
                // The authored midpoint curve must affect real Core geometry
                // in addition to endpoint translation, with no raster swaps.
                const double t = (y - 570) / 18;
                const double linearContribution = (t * t * (3 - 2 * t)) * 20;
                QVERIFY(displacement.x * 1254 - linearContribution > 1.5);
                middleCurves = true;
            }
        }
        QVERIFY(middleCurves);
        QCOMPARE(snapshot(main.model), beforeMain);
        QVERIFY(setParameter(surface.model, "ParamNeckBottomX", 0));
        QVERIFY(setParameter(surface.model, "ParamNeckBottomY", 0));
        QVERIFY(setParameter(surface.model, "ParamNeckCurve", 0));
        Core::csmUpdateModel(surface.model);
        QCOMPARE(bytes(Core::csmGetDrawableVertexPositions(surface.model)[0], 63), bytes(baseline.data(), 63));
        // Attachment restores the real head surface at the upper rows even
        // after the auxiliary model was exercised through nonzero keyforms.
        QVERIFY2(binding.update(0, &error), qPrintable(error));
        QCOMPARE(binding.positions().size(), std::size_t(63));
        for (int vertex = 0; vertex < sourcePoints.size(); ++vertex) {
            const auto source = sourcePoints[vertex].toArray();
            if (source[1].toDouble() > 570) continue;
            const auto& actual = binding.positions()[vertex];
            comparePoint({actual.X, actual.Y}, sample(main.model, metadata_, ownership_, QStringLiteral("face"),
                {source[0].toDouble(), source[1].toDouble()}));
        }
        QCOMPARE(snapshot(main.model), beforeMain);
    }

    void independentSurfaceFollowsMotionKeyposesAndMaterialSwitch_data() {
        QTest::addColumn<QJsonObject>("parameters");
        const QDir motions(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("assets/motions")));
        int authoredPoses = 0;
        for (const auto& name : motions.entryList({QStringLiteral("*.motion.json")}, QDir::Files, QDir::Name)) {
            QFile file(motions.filePath(name));
            QVERIFY(file.open(QIODevice::ReadOnly));
            const auto document = QJsonDocument::fromJson(file.readAll()).object();
            const auto constants = document.value(QStringLiteral("constants")).toObject();
            const auto keyframes = document.value(QStringLiteral("keyframes")).toArray();
            for (int frame = 0; frame < keyframes.size(); ++frame) {
                QJsonObject parameters = constants;
                const auto values = keyframes[frame].toObject().value(QStringLiteral("parameters")).toObject();
                for (auto it = values.constBegin(); it != values.constEnd(); ++it) parameters.insert(it.key(), it.value());
                const auto label = QStringLiteral("authored-%1-%2").arg(name).arg(frame).toUtf8();
                QTest::newRow(label.constData()) << parameters;
                ++authoredPoses;
            }
        }
        QVERIFY(authoredPoses >= 67);
        for (double busy : {0.834999, 0.835001}) {
            for (double mirror : {-1.0, 1.0}) {
                const auto label = QStringLiteral("switch-%1-mirror%2").arg(busy, 0, 'f', 6).arg(mirror).toUtf8();
                QTest::newRow(label.constData()) << QJsonObject{
                    {"ParamBusyLaptop", busy}, {"ParamSitPose", 0.9}, {"ParamAngleX", 18 * mirror},
                    {"ParamAngleZ", 12 * mirror}, {"ParamBodyAngleZ", -8 * mirror}};
            }
        }
        // Combined moderate corners exercise the supported interaction range.
        // Full XYZ Native extremes can put the jaw below the collar and are a
        // separate physical contract, not silently declared valid here.
        for (bool seated : {false, true}) {
            for (double x : {-30.0, 0.0, 30.0}) for (double y : {-20.0, 0.0, 20.0}) for (double z : {-12.0, 0.0, 12.0}) {
                const double body = z < 0 ? 8 : -8;
                const auto label = QStringLiteral("moderate-seat%1-x%2-y%3-z%4").arg(seated).arg(x).arg(y).arg(z).toUtf8();
                QTest::newRow(label.constData()) << QJsonObject{
                    {"ParamBusyLaptop", seated ? 1 : 0}, {"ParamSitPose", seated ? 1 : 0},
                    {"ParamAngleX", x}, {"ParamAngleY", y}, {"ParamAngleZ", z},
                    {"ParamBodyAngleX", -body}, {"ParamBodyAngleZ", body}};
            }
        }
    }

    void independentSurfaceFollowsMotionKeyposesAndMaterialSwitch() {
        if (surfaceModelPath_.isEmpty()) QSKIP("No independent component selected; use DESKTOP_COMPANION_NECK_TEST_FIXTURE before adoption.");
        QFETCH(QJsonObject, parameters);
        QByteArray surfaceMoc;
        QJsonObject rig;
        QVERIFY2(readSurfaceFixture(&surfaceMoc, &rig), qPrintable(surfaceModelPath_));
        NativeModel main, surface;
        QVERIFY(main.load(moc_));
        QVERIFY2(surface.load(surfaceMoc), "Independent MOC failed consistency/revival preflight.");
        CubismNeckBinding binding;
        QString error;
        QVERIFY2(binding.configure(main.model, metadata_, ownership_, &error), qPrintable(error));
        QVERIFY2(binding.configureSurface(surface.model, rig, &error), qPrintable(error));
        const auto sourcePoints = rig.value(QStringLiteral("vertexSourcePoints")).toArray();
        QCOMPARE(sourcePoints.size(), 63);
        QByteArray firstMain, firstNeck;
        for (int repeat = 0; repeat < 3; ++repeat) {
            double material = 0;
            QVERIFY(runtimePosture(main.model, metadata_, ownership_, parameters, &material));
            const auto before = snapshot(main.model);
            QVERIFY2(binding.update(material, &error), qPrintable(error));
            QCOMPARE(snapshot(main.model), before);
            QCOMPARE(binding.positions().size(), std::size_t(63));
            std::vector<Point> registered;
            for (int vertex = 0; vertex < sourcePoints.size(); ++vertex) {
                const auto source = sourcePoints[vertex].toArray();
                const Point expected = sample(main.model, metadata_, ownership_, QStringLiteral("face"),
                    {source[0].toDouble(), source[1].toDouble()});
                registered.push_back(expected);
                if (source[1].toDouble() <= 570) {
                    const auto& actual = binding.positions()[vertex];
                    comparePoint({actual.X, actual.Y}, expected);
                }
            }
            const auto tip = sampleSurface(surface.model, sourcePoints, {628, 588}, binding.positions().data());
            const QString visibleCollar = material >= 0.5 ? QStringLiteral("busy torso") : QStringLiteral("topwear");
            const auto collar = sample(main.model, metadata_, ownership_, visibleCollar, {628, 588});
            QVERIFY2(std::abs(tip.x - collar.x) * 1254 < 0.1,
                qPrintable(QStringLiteral("Independent neck X attachment error %1 source px").arg((tip.x - collar.x) * 1254)));
            QVERIFY2(std::abs(tip.y - collar.y) * 1254 < 0.1,
                qPrintable(QStringLiteral("Independent neck Y attachment error %1 source px").arg((tip.y - collar.y) * 1254)));
            const auto* indices = Core::csmGetDrawableIndices(surface.model)[0];
            for (int i = 0; i < Core::csmGetDrawableIndexCounts(surface.model)[0]; i += 3) {
                const int ai = indices[i], bi = indices[i + 1], ci = indices[i + 2];
                const auto& actual = binding.positions();
                const double previousArea = area(registered[ai], registered[bi], registered[ci]);
                const double actualArea = area({actual[ai].X, actual[ai].Y}, {actual[bi].X, actual[bi].Y}, {actual[ci].X, actual[ci].Y});
                QVERIFY(std::abs(previousArea) > 1e-10);
                QVERIFY2(actualArea / previousArea > 0.02,
                    qPrintable(QStringLiteral("Independent triangle %1 flips/collapses: area ratio %2").arg(i / 3).arg(actualArea / previousArea)));
            }
            const auto neck = bytes(binding.positions().data(), static_cast<int>(binding.positions().size()));
            if (repeat == 0) { firstMain = geometry(main.model); firstNeck = neck; }
            else { QCOMPARE(geometry(main.model), firstMain); QCOMPARE(neck, firstNeck); }
            // Calling attach twice without a new main Update must also be
            // stable, and must not add the previous differential a second time.
            QVERIFY2(binding.update(material, &error), qPrintable(error));
            QCOMPARE(bytes(binding.positions().data(), 63), neck);
            QCOMPARE(snapshot(main.model), before);
        }
    }

private:
    bool readSurfaceFixture(QByteArray* moc, QJsonObject* rig) const {
        const auto read = [](const QString& path) {
            QFile file(path);
            return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        };
        const auto model = QJsonDocument::fromJson(read(surfaceModelPath_)).object();
        const auto references = model.value(QStringLiteral("FileReferences")).toObject();
        const auto mocName = references.value(QStringLiteral("Moc")).toString();
        if (mocName.isEmpty()) return false;
        *moc = read(QDir(QFileInfo(surfaceModelPath_).absolutePath()).filePath(mocName));
        *rig = QJsonDocument::fromJson(read(surfaceRigPath_)).object();
        return !moc->isEmpty() && !rig->isEmpty();
    }

    QByteArray moc_;
    QJsonObject metadata_, ownership_;
    QString surfaceModelPath_, surfaceRigPath_;
};

QTEST_GUILESS_MAIN(CubismNeckBindingTest)
#include "CubismNeckBindingTest.moc"
