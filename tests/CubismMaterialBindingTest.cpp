#include "../src/CubismMaterialBinding.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QtTest/QTest>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {
const char* const headParameters[] = {
    "ParamAngleX", "ParamAngleY", "ParamAngleZ", "ParamHairFront", "ParamHairBack", "ParamCheek"};

// The revived moc and model storage remain alive for the entire Core instance.
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
        auto* moc = Core::csmReviveMocInPlace(address, static_cast<unsigned int>(bytes.size()));
        if (!moc) return false;
        const auto size = Core::csmGetSizeofModel(moc);
        if (!size) return false;
        model = Core::csmInitializeModelInPlace(moc, aligned(modelStorage, size, Core::csmAlignofModel), size);
        if (model) Core::csmUpdateModel(model);
        return model != nullptr;
    }
};

int parameterIndex(Core::csmModel* model, const char* name) {
    const auto* ids = Core::csmGetParameterIds(model);
    for (int i = 0; i < Core::csmGetParameterCount(model); ++i) {
        if (std::strcmp(ids[i], name) == 0) return i;
    }
    return -1;
}

bool setParameter(Core::csmModel* model, const char* name, float value) {
    const int index = parameterIndex(model, name);
    if (index < 0) return false;
    Core::csmGetParameterValues(model)[index] = std::clamp(value,
        Core::csmGetParameterMinimumValues(model)[index], Core::csmGetParameterMaximumValues(model)[index]);
    return true;
}

void evaluateReference(Core::csmModel* reference, Core::csmModel* visible) {
    std::copy_n(Core::csmGetParameterValues(visible), Core::csmGetParameterCount(visible),
                Core::csmGetParameterValues(reference));
    std::copy_n(Core::csmGetPartOpacities(visible), Core::csmGetPartCount(visible),
                Core::csmGetPartOpacities(reference));
    for (const auto* name : headParameters) setParameter(reference, name, 0);
    Core::csmUpdateModel(reference);
}

template<class T> QByteArray bytes(const T* data, int count) {
    return QByteArray(reinterpret_cast<const char*>(data), count * qsizetype(sizeof(T)));
}

struct Snapshot {
    QByteArray parameters, parts, vertices, opacity, multiply, screen, flags;
};

Snapshot snapshot(Core::csmModel* model) {
    Snapshot result;
    result.parameters = bytes(Core::csmGetParameterValues(model), Core::csmGetParameterCount(model));
    result.parts = bytes(Core::csmGetPartOpacities(model), Core::csmGetPartCount(model));
    const int count = Core::csmGetDrawableCount(model);
    result.opacity = bytes(Core::csmGetDrawableOpacities(model), count);
    result.multiply = bytes(Core::csmGetDrawableMultiplyColors(model), count);
    result.screen = bytes(Core::csmGetDrawableScreenColors(model), count);
    result.flags = bytes(Core::csmGetDrawableDynamicFlags(model), count);
    const auto* positions = Core::csmGetDrawableVertexPositions(model);
    const auto* vertexCounts = Core::csmGetDrawableVertexCounts(model);
    for (int mesh = 0; mesh < count; ++mesh) result.vertices += bytes(positions[mesh], vertexCounts[mesh]);
    return result;
}

void compareSnapshots(const Snapshot& actual, const Snapshot& expected, bool compareFlags = true) {
    QCOMPARE(actual.parameters, expected.parameters);
    QCOMPARE(actual.parts, expected.parts);
    QCOMPARE(actual.vertices, expected.vertices);
    QCOMPARE(actual.opacity, expected.opacity);
    QCOMPARE(actual.multiply, expected.multiply);
    QCOMPARE(actual.screen, expected.screen);
    if (compareFlags) QCOMPARE(actual.flags, expected.flags);
}
} // namespace

class CubismMaterialBindingTest : public QObject {
    Q_OBJECT
private slots:
    void initTestCase() {
        const QDir directory(QDir(QCoreApplication::applicationDirPath())
            .filePath(QStringLiteral("assets/live2d/whale-girl")));
        QFile moc(directory.filePath(QStringLiteral("whale-girl-layered-draft.moc3")));
        QVERIFY2(moc.open(QIODevice::ReadOnly), qPrintable(moc.fileName()));
        mocBytes_ = moc.readAll();
        QFile metadataFile(directory.filePath(QStringLiteral("whale-girl-layered-draft.psd2live.json")));
        QVERIFY2(metadataFile.open(QIODevice::ReadOnly), qPrintable(metadataFile.fileName()));
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(metadataFile.readAll(), &error);
        QCOMPARE(error.error, QJsonParseError::NoError);
        QVERIFY(document.isObject());
        metadata_ = document.object();
        const auto separation = metadata_.value(QStringLiteral("runtimeMaterialSeparation")).toObject();
        QCOMPARE(separation.value(QStringLiteral("bodySource")).toString(), QStringLiteral("face"));
        QSet<QString> expected, actual;
        for (const auto* name : headParameters) expected.insert(QString::fromLatin1(name));
        for (const auto& value : separation.value(QStringLiteral("headOnlyParameters")).toArray()) {
            actual.insert(value.toString());
        }
        QCOMPARE(actual, expected);
    }

    void rejectsAliasedModelsAndInvalidDeclarations() {
        NativeModel visible, body;
        QVERIFY(visible.load(mocBytes_));
        QVERIFY(body.load(mocBytes_));
        CubismMaterialBinding binding;
        QString error;
        QVERIFY(!binding.configure(visible.model, visible.model, metadata_, &error));
        QVERIFY(!error.isEmpty());
        QCOMPARE(binding.bodyDrawableIndex(), -1);
        QVERIFY(!binding.configure(nullptr, body.model, metadata_, &error));

        const auto original = metadata_.value(QStringLiteral("runtimeMaterialSeparation")).toObject();
        for (int kind = 0; kind < 5; ++kind) {
            QVERIFY2(binding.configure(visible.model, body.model, metadata_, &error), qPrintable(error));
            auto invalid = original;
            auto metadata = metadata_;
            if (kind == 0) invalid.insert(QStringLiteral("bodySource"), QStringLiteral("missing material"));
            if (kind == 1) invalid.insert(QStringLiteral("headOnlyParameters"), QJsonArray{"UnknownHeadId"});
            if (kind == 2) invalid.insert(QStringLiteral("headOnlyParameters"), QJsonArray{"ParamAngleX", "ParamAngleX"});
            if (kind == 3) invalid.insert(QStringLiteral("headOnlyParameters"), QJsonArray{});
            if (kind == 4) {
                auto layers = metadata.value(QStringLiteral("layers")).toArray();
                for (const auto& layer : metadata.value(QStringLiteral("layers")).toArray()) {
                    if (layer.toObject().value(QStringLiteral("source")).toString() == QStringLiteral("face")) {
                        layers.append(layer);
                        break;
                    }
                }
                metadata.insert(QStringLiteral("layers"), layers);
            }
            metadata.insert(QStringLiteral("runtimeMaterialSeparation"), invalid);
            QVERIFY(!binding.configure(visible.model, body.model, metadata, &error));
            QVERIFY(!error.isEmpty());
            QCOMPARE(binding.bodyDrawableIndex(), -1);
            const auto before = snapshot(visible.model);
            const auto bodyBefore = snapshot(body.model);
            binding.apply();
            compareSnapshots(snapshot(visible.model), before);
            compareSnapshots(snapshot(body.model), bodyBefore);
        }
    }

    void bodyEvaluationKeepsVisibleNativeStateAndAllNonHeadParameters_data() {
        QTest::addColumn<bool>("seated");
        QTest::newRow("standing-with-grass") << false;
        QTest::newRow("seated-with-laptop") << true;
    }

    void bodyEvaluationKeepsVisibleNativeStateAndAllNonHeadParameters() {
        QFETCH(bool, seated);
        NativeModel visible, body, reference;
        QVERIFY(visible.load(mocBytes_));
        QVERIFY(body.load(mocBytes_));
        QVERIFY(reference.load(mocBytes_));
        CubismMaterialBinding binding;
        QString error;
        QVERIFY2(binding.configure(visible.model, body.model, metadata_, &error), qPrintable(error));
        const int face = binding.bodyDrawableIndex();
        QVERIFY(face >= 0);
        QCOMPARE(QString::fromUtf8(Core::csmGetDrawableIds(visible.model)[face]), QStringLiteral("ArtMeshFace"));
        // The secondary renderer draws only this material: no hidden clipping
        // dependency may require rendering another drawable as a mask.
        QCOMPARE(Core::csmGetDrawableMaskCounts(body.model)[face], 0);

        const std::pair<const char*, float> pose[] = {
            {"ParamAngleX", 35}, {"ParamAngleY", -22}, {"ParamAngleZ", 20},
            {"ParamHairFront", 0.8f}, {"ParamHairBack", -0.7f}, {"ParamCheek", 1},
            {"ParamBodyAngleX", 12}, {"ParamBodyAngleY", -7}, {"ParamBodyAngleZ", 5},
            {"ParamShiftX", 24}, {"ParamShiftY", -10}, {"ParamArmLA", -16},
            {"ParamArmRA", 52}, {"ParamElbowLA", -8}, {"ParamElbowRA", 18},
            {"ParamWristRA", -12}, {"ParamHandRGrip", 1},
            {"ParamGrassVisible", seated ? 0.0f : 1.0f}, {"ParamGrassReach", 0.6f},
            {"ParamGrassSwing", -0.7f}, {"ParamGrassTipBend", 0.8f},
            {"ParamBusyLaptop", seated ? 1.0f : 0.0f}, {"ParamSitPose", seated ? 1.0f : 0.0f},
            {"ParamLaptopVisible", seated ? 1.0f : 0.0f}, {"ParamBusyTypingL", 0.8f},
            {"ParamBusyTypingR", 0.5f}, {"ParamLaptopRock", 0.3f}, {"ParamBreath", 0.6f}};
        for (const auto& value : pose) QVERIFY2(setParameter(visible.model, value.first, value.second), value.first);
        Core::csmGetPartOpacities(visible.model)[0] = 0.43f;
        Core::csmUpdateModel(visible.model);
        Core::csmResetDrawableDynamicFlags(visible.model);
        const auto before = snapshot(visible.model);
        evaluateReference(reference.model, visible.model);
        binding.apply();
        compareSnapshots(snapshot(visible.model), before);
        compareSnapshots(snapshot(body.model), snapshot(reference.model), false);
        const auto* ids = Core::csmGetParameterIds(visible.model);
        for (int parameter = 0; parameter < Core::csmGetParameterCount(visible.model); ++parameter) {
            const bool head = std::any_of(std::begin(headParameters), std::end(headParameters),
                [&](const char* id) { return std::strcmp(ids[parameter], id) == 0; });
            QCOMPARE(Core::csmGetParameterValues(body.model)[parameter],
                head ? 0.0f : Core::csmGetParameterValues(visible.model)[parameter]);
        }
        QVERIFY(bytes(Core::csmGetDrawableVertexPositions(visible.model)[face],
            Core::csmGetDrawableVertexCounts(visible.model)[face])
            != bytes(Core::csmGetDrawableVertexPositions(body.model)[face],
                Core::csmGetDrawableVertexCounts(body.model)[face]));
    }

    void repeatedUpdatesDoNotAccumulateGeometryChanges() {
        NativeModel visible, body, reference;
        QVERIFY(visible.load(mocBytes_));
        QVERIFY(body.load(mocBytes_));
        QVERIFY(reference.load(mocBytes_));
        CubismMaterialBinding binding;
        QString error;
        QVERIFY2(binding.configure(visible.model, body.model, metadata_, &error), qPrintable(error));
        Snapshot firstBody;
        const auto count = Core::csmGetParameterCount(visible.model);
        const auto* minima = Core::csmGetParameterMinimumValues(visible.model);
        const auto* maxima = Core::csmGetParameterMaximumValues(visible.model);
        for (int frame = 0; frame < 60; ++frame) {
            const bool repeat = frame == 0 || frame == 1 || frame == 59;
            const float phase = repeat ? 0.65f : frame * 0.11f;
            for (int parameter = 0; parameter < count; ++parameter) {
                Core::csmGetParameterValues(visible.model)[parameter] = minima[parameter]
                    + (maxima[parameter] - minima[parameter]) * (0.5f + 0.4f * std::sin(phase + parameter));
            }
            Core::csmUpdateModel(visible.model);
            const auto before = snapshot(visible.model);
            evaluateReference(reference.model, visible.model);
            binding.apply();
            compareSnapshots(snapshot(visible.model), before);
            compareSnapshots(snapshot(body.model), snapshot(reference.model), false);
            if (frame == 0) firstBody = snapshot(body.model);
            if (frame == 1 || frame == 59) compareSnapshots(snapshot(body.model), firstBody, false);
            const auto bodyBefore = snapshot(body.model);
            binding.apply();
            compareSnapshots(snapshot(body.model), bodyBefore, false);
            compareSnapshots(snapshot(visible.model), before);
        }
    }

private:
    QByteArray mocBytes_;
    QJsonObject metadata_;
};

QTEST_GUILESS_MAIN(CubismMaterialBindingTest)
#include "CubismMaterialBindingTest.moc"
