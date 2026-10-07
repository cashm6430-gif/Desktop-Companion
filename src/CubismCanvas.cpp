#include <GL/glew.h>

#include "CubismCanvas.h"
#include "CubismMaterialBinding.h"
#include "CubismMaterialTexture.h"
#include "CubismNeckBinding.h"
#include "CubismPostureTransition.h"

#include <CubismFramework.hpp>
#include <Id/CubismId.hpp>
#include <Id/CubismIdManager.hpp>
#include <Math/CubismMatrix44.hpp>
#include <Math/CubismModelMatrix.hpp>
#include <Model/CubismModel.hpp>
#include <Model/CubismModelMultiplyAndScreenColor.hpp>
#include <Model/CubismUserModel.hpp>
#include <Live2DCubismCore.hpp>
#include <Physics/CubismPhysics.hpp>
#include <Rendering/OpenGL/CubismOffscreenManager_OpenGLES2.hpp>
#include <Rendering/OpenGL/CubismRenderer_OpenGLES2.hpp>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>
#include <QSurfaceFormat>
#include <QPainterPathStroker>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>
#include <utility>
#include <algorithm>
#include <limits>
#include <cmath>
#include <type_traits>

namespace Csm = Live2D::Cubism::Framework;

namespace {
class CubismAllocator final : public Csm::ICubismAllocator {
public:
    void* Allocate(Csm::csmSizeType size) override { return std::malloc(size); }
    void Deallocate(void* memory) override { std::free(memory); }
    void* AllocateAligned(Csm::csmSizeType size, Csm::csmUint32 alignment) override {
        void* raw = std::malloc(size + alignment - 1 + sizeof(void*));
        if (!raw) return nullptr;
        const auto start = reinterpret_cast<uintptr_t>(raw) + sizeof(void*);
        const auto aligned = (start + alignment - 1) & ~static_cast<uintptr_t>(alignment - 1);
        reinterpret_cast<void**>(aligned)[-1] = raw;
        return reinterpret_cast<void*>(aligned);
    }
    void DeallocateAligned(void* memory) override {
        if (memory) std::free(reinterpret_cast<void**>(memory)[-1]);
    }
};

class PetCubismModel final : public Csm::CubismUserModel {
public:
    void evaluatePhysics(float seconds) {
        if (_physics) _physics->Evaluate(_model, seconds);
    }
};

QByteArray readFile(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
void frameworkLog(const char* message) { qWarning().noquote() << message; }
Csm::csmByte* loadShaderBytes(const std::string path, Csm::csmSizeInt* outSize) {
    const QString resolved = QDir(QCoreApplication::applicationDirPath()).filePath(
        QString::fromUtf8(path.c_str()));
    const QByteArray bytes = readFile(resolved);
    if (bytes.isEmpty()) {
        qWarning() << "Missing Cubism shader:" << QString::fromStdString(path);
        return nullptr;
    }
    *outSize = static_cast<Csm::csmSizeInt>(bytes.size());
    auto* result = new Csm::csmByte[bytes.size()];
    std::memcpy(result, bytes.constData(), bytes.size());
    return result;
}
void releaseShaderBytes(Csm::csmByte* bytes) { delete[] bytes; }

bool alignDrawable(Live2D::Cubism::Core::csmModel* model, int drawable,
                   const CubismPostureTransition::FloorPinnedTransform& transform) {
    auto* vertices = const_cast<Live2D::Cubism::Core::csmVector2*>(
        Live2D::Cubism::Core::csmGetDrawableVertexPositions(model)[drawable]);
    const int count = Live2D::Cubism::Core::csmGetDrawableVertexCounts(model)[drawable];
    for (int i = 0; i < count; ++i) {
        CubismPostureTransition::Point point;
        if (!transform.apply({vertices[i].X, vertices[i].Y}, &point)) return false;
        vertices[i].X = static_cast<float>(point.x);
        vertices[i].Y = static_cast<float>(point.y);
    }
    return true;
}
}

struct CubismCanvas::Impl {
    CubismAllocator allocator;
    Csm::CubismFramework::Option frameworkOption{};
    std::unique_ptr<PetCubismModel> model;
    std::unique_ptr<PetCubismModel> bodyModel;
    std::unique_ptr<PetCubismModel> neckModel;
    CubismMaterialBinding materialBinding;
    CubismNeckBinding neckBinding;
    std::vector<GLuint> textures;
    std::vector<GLuint> bodyTextures;
    std::vector<GLuint> neckTextures;
    GLuint bodyUnderpaintTexture = 0;
    GLuint neckTexture = 0;
    int bodyTextureIndex = -1;
    QHash<QString, int> parameterIndices;
    std::vector<int> standingFeet;
    std::vector<int> seatedFeet;
    std::vector<int> headMeshes;
    int faceMesh = -1;
    int grassMesh = -1;
    float grassTipMinV = 0, grassTipMaxU = 0;
    // The bite (gape) art drawable. The exported moc3 lost the switch-opacity
    // keyforms that should tie this mesh's visibility to ParamMouthGape, so
    // the layer rides fully opaque and only MouthOpenY's 25%-sliver closed
    // form keeps it unnoticeable -- which still reads as a phantom slit under
    // the resting omega and a second mouth during the bite. The render loop
    // patches this drawable's opacity directly (see paintGL) because the
    // Framework exposes no setter and PSD2Live cannot re-export right now.
    int gapeDrawable = -1;
    // Every mesh painted for the seated variant. It is registered chin-to-head
    // instead of sole-to-sole, so the soles sit ~64 px above the standing
    // shoes; they are shifted onto the standing floor each frame.
    std::vector<int> seatedMeshes;
    std::vector<int> standingMeshes;
    float standingFloor = 0;
    bool frameworkStarted = false;
    bool rawExportReview = false;
    bool authoredSitPose = false;
    std::vector<int> deskDrawables;
    std::vector<int> deskLegacyMaterials;
    int hairSupportDrawable = -1;
    float hairSupportScaleX = 1.0f;
    float hairSupportScaleY = 1.0f;
    int skirtSpreadParameter = -1;
    int handGroundParameter = -1;

    bool hasNeck() const { return neckTexture != 0 || neckModel != nullptr; }

    float footFloor(const std::vector<int>& feet) const {
        float floor = std::numeric_limits<float>::infinity();
        const auto* native = model->GetModel();
        for (int index : feet) {
            const auto* vertices = native->GetDrawableVertexPositions(index);
            for (int i = 0; i < native->GetDrawableVertexCount(index); ++i)
                floor = std::min(floor, vertices[i].Y);
        }
        return floor;
    }

    void bindTextures() {
        auto* renderer = model->GetRenderer<Csm::Rendering::CubismRenderer_OpenGLES2>();
        for (size_t i = 0; i < textures.size(); ++i)
            renderer->BindTexture(static_cast<Csm::csmUint32>(i), textures[i]);
        renderer->IsPremultipliedAlpha(true);
        if (bodyModel) {
            auto* bodyRenderer = bodyModel->GetRenderer<Csm::Rendering::CubismRenderer_OpenGLES2>();
            for (size_t i = 0; i < bodyTextures.size(); ++i)
                bodyRenderer->BindTexture(static_cast<Csm::csmUint32>(i), bodyTextures[i]);
            bodyRenderer->IsPremultipliedAlpha(true);
        }
        if (neckModel) {
            auto* neckRenderer = neckModel->GetRenderer<Csm::Rendering::CubismRenderer_OpenGLES2>();
            for (size_t i = 0; i < neckTextures.size(); ++i)
                neckRenderer->BindTexture(static_cast<Csm::csmUint32>(i), neckTextures[i]);
            neckRenderer->IsPremultipliedAlpha(true);
        }
    }
};

CubismCanvas::CubismCanvas(const ParameterMotion* motion, QWidget* parent)
    : QOpenGLWidget(parent), impl_(std::make_unique<Impl>()), motion_(motion) {
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAutoFillBackground(false);
    QSurfaceFormat surfaceFormat = format();
    surfaceFormat.setAlphaBufferSize(8);
    surfaceFormat.setSamples(4);
    setFormat(surfaceFormat);
}

CubismCanvas::~CubismCanvas() {
    if (context()) makeCurrent();
    impl_->model.reset();
    impl_->bodyModel.reset();
    impl_->neckModel.reset();
    if (!impl_->textures.empty())
        glDeleteTextures(static_cast<GLsizei>(impl_->textures.size()), impl_->textures.data());
    if (!impl_->bodyTextures.empty())
        glDeleteTextures(static_cast<GLsizei>(impl_->bodyTextures.size()), impl_->bodyTextures.data());
    if (!impl_->neckTextures.empty())
        glDeleteTextures(static_cast<GLsizei>(impl_->neckTextures.size()), impl_->neckTextures.data());
    if (impl_->bodyUnderpaintTexture)
        glDeleteTextures(1, &impl_->bodyUnderpaintTexture);
    if (impl_->neckTexture)
        glDeleteTextures(1, &impl_->neckTexture);
    if (impl_->frameworkStarted) {
        Csm::CubismFramework::Dispose();
        Csm::CubismFramework::CleanUp();
    }
    if (context()) doneCurrent();
}

void CubismCanvas::advance(double seconds) {
    frameSeconds_ += seconds;
    update();
}

QJsonObject CubismCanvas::parameterRanges() const {
    QJsonObject ranges;
    if (!ready_ || !impl_->model) return ranges;
    auto* model = impl_->model->GetModel();
    for (int i = 0; i < model->GetParameterCount(); ++i) {
        const auto id = QString::fromUtf8(model->GetParameterId(i)->GetString().GetRawString());
        ranges.insert(id, QJsonObject{
            {QStringLiteral("min"), model->GetParameterMinimumValue(i)},
            {QStringLiteral("max"), model->GetParameterMaximumValue(i)},
            {QStringLiteral("default"), model->GetParameterDefaultValue(i)}});
    }
    return ranges;
}

void CubismCanvas::initializeGL() {
    glewExperimental = GL_TRUE;
    if (const GLenum glewError = glewInit(); glewError != GLEW_OK) {
        error_ = QStringLiteral("GLEW init failed: %1").arg(reinterpret_cast<const char*>(glewGetErrorString(glewError)));
        return;
    }
    glGetError(); // GLEW may query an unsupported legacy extension.
    impl_->frameworkOption.LogFunction = frameworkLog;
    impl_->frameworkOption.LoggingLevel = Csm::CubismFramework::Option::LogLevel_Warning;
    impl_->frameworkOption.LoadFileFunction = loadShaderBytes;
    impl_->frameworkOption.ReleaseBytesFunction = releaseShaderBytes;
    if (!Csm::CubismFramework::StartUp(&impl_->allocator, &impl_->frameworkOption)) {
        error_ = QStringLiteral("Cubism Framework failed to start");
        return;
    }
    impl_->frameworkStarted = true;
    Csm::CubismFramework::Initialize();

    const QString reviewModel = QCoreApplication::instance()->property(
        "desktopCompanionReviewModelPath").toString();
    const QString settingsPath = reviewModel.isEmpty()
        ? QDir(QCoreApplication::applicationDirPath()).filePath(
            QStringLiteral("assets/live2d/whale-girl/whale-girl-layered-draft.model3.json"))
        : reviewModel;
    const QString directory = QFileInfo(settingsPath).absolutePath();
    impl_->rawExportReview = QCoreApplication::instance()->property(
        "desktopCompanionReviewRawExport").toBool();
    const auto settings = QJsonDocument::fromJson(readFile(settingsPath));
    const QJsonObject refs = settings.object().value(QStringLiteral("FileReferences")).toObject();
    const QString mocName = refs.value(QStringLiteral("Moc")).toString();
    const QByteArray moc = readFile(QDir(directory).filePath(mocName));
    if (mocName.isEmpty() || moc.isEmpty()) {
        error_ = QStringLiteral("Cubism model manifest or MOC3 is missing");
        return;
    }

    impl_->model = std::make_unique<PetCubismModel>();
    impl_->model->LoadModel(reinterpret_cast<const Csm::csmByte*>(moc.constData()),
                            static_cast<Csm::csmSizeInt>(moc.size()));
    auto* model = impl_->model->GetModel();
    if (!model) {
        error_ = QStringLiteral("Cubism Core could not load the MOC3");
        impl_->model.reset();
        return;
    }

    const QString physicsName = refs.value(QStringLiteral("Physics")).toString();
    if (!physicsName.isEmpty()) {
        const QByteArray physics = readFile(QDir(directory).filePath(physicsName));
        if (!physics.isEmpty())
            impl_->model->LoadPhysics(reinterpret_cast<const Csm::csmByte*>(physics.constData()),
                                      static_cast<Csm::csmSizeInt>(physics.size()));
    }

    for (int i = 0; i < model->GetParameterCount(); ++i) {
        const auto id = model->GetParameterId(i)->GetString().GetRawString();
        impl_->parameterIndices.insert(QString::fromUtf8(id), i);
    }
    for (int i = 0; i < model->GetDrawableCount(); ++i) {
        if (model->GetDrawableId(i)->GetString().GetRawString() == QStringLiteral("ArtMeshMouthOpen"))
            impl_->gapeDrawable = i;
    }

    QString metadataName = QFileInfo(settingsPath).fileName();
    metadataName.chop(QStringLiteral("model3.json").size());
    metadataName += QStringLiteral("psd2live.json");
    const auto metadata = impl_->rawExportReview ? QJsonDocument(QJsonObject{})
        : QJsonDocument::fromJson(readFile(QDir(directory).filePath(metadataName)));
    QRect backdropChart;
    QByteArray backdropAtlasSha;
    QImage hairBacking;
    QList<QRect> hairBackingSources;
    const QList<QRect> hairBackingTargets{QRect(2959, 8, 315, 350), QRect(3323, 8, 315, 350)};
    if (metadata.object().contains(QStringLiteral("runtimeBackdropCleanup"))) {
        const auto cleanup = metadata.object().value(QStringLiteral("runtimeBackdropCleanup")).toObject();
        const auto expected = cleanup.value(QStringLiteral("mocSha256")).toString().toLatin1();
        backdropAtlasSha = cleanup.value(QStringLiteral("atlasSha256")).toString().toLatin1();
        const int hair = model->GetDrawableIndex(Csm::CubismFramework::GetIdManager()->GetId("ArtMeshBackHair2"));
        const int backing = model->GetDrawableIndex(Csm::CubismFramework::GetIdManager()->GetId("ArtMeshBackHair"));
        if (cleanup.value(QStringLiteral("version")).toInt() != 1
            || cleanup.value(QStringLiteral("backHairOnly")).toString() != QStringLiteral("ArtMeshBackHair2")
            || cleanup.value(QStringLiteral("backingMaterial")).toString() != QStringLiteral("ArtMeshBackHair")
            || expected.size() != 64 || backdropAtlasSha.size() != 64
            || QCryptographicHash::hash(moc, QCryptographicHash::Sha256).toHex() != expected
            || cleanup.value(QStringLiteral("keepStrongComponents")).toInt() != 3
            || hair < 0 || backing < 0 || Core::csmGetDrawableTextureIndices(model->GetModel())[hair] != 0) {
            error_ = QStringLiteral("Backdrop cleanup material identity is invalid");
            return;
        }
        const auto* uv = model->GetDrawableVertexUvs(hair);
        double left = 4096, top = 4096, right = 0, bottom = 0;
        for (int i = 0; i < model->GetDrawableVertexCount(hair); ++i) {
            left = std::min(left, uv[i].X * 4096.0); right = std::max(right, uv[i].X * 4096.0);
            top = std::min(top, (1.0 - uv[i].Y) * 4096.0); bottom = std::max(bottom, (1.0 - uv[i].Y) * 4096.0);
        }
        backdropChart = QRect(QPoint(static_cast<int>(std::floor(left)) - 2, static_cast<int>(std::floor(top)) - 2),
                              QPoint(static_cast<int>(std::ceil(right)) + 2, static_cast<int>(std::ceil(bottom)) + 2));
        const auto material = cleanup.value(QStringLiteral("backingTexture")).toObject();
        const auto bytes = readFile(QDir(directory).filePath(material.value(QStringLiteral("file")).toString()));
        const auto materialSha = material.value(QStringLiteral("sha256")).toString().toLatin1();
        hairBacking = QImage::fromData(bytes).convertToFormat(QImage::Format_RGBA8888_Premultiplied);
        if (materialSha.size() != 64 || QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() != materialSha
            || hairBacking.size() != QSize(1254, 1254) || material.value(QStringLiteral("sourceRects")).toArray().size() != 2) {
            error_ = QStringLiteral("Pure hair backing source does not match its declared material");
            return;
        }
        for (const auto& value : material.value(QStringLiteral("sourceRects")).toArray()) {
            const auto coordinates = value.toArray();
            if (coordinates.size() != 4) { error_ = QStringLiteral("Hair backing registration is invalid"); return; }
            const QRect rect(coordinates[0].toInt(), coordinates[1].toInt(),
                             coordinates[2].toInt() - coordinates[0].toInt(), coordinates[3].toInt() - coordinates[1].toInt());
            if (!rect.isValid() || !hairBacking.rect().contains(rect)) {
                error_ = QStringLiteral("Hair backing source rectangle leaves the generated material"); return;
            }
            hairBackingSources.push_back(rect);
        }
        const auto supportScale = cleanup.value(QStringLiteral("supportScale")).toArray();
        if (supportScale.size() != 2 || supportScale[0].toDouble() < 1.0 || supportScale[0].toDouble() > 2.0
            || supportScale[1].toDouble() < 1.0 || supportScale[1].toDouble() > 2.0) {
            error_ = QStringLiteral("Pure hair support extent is invalid"); return;
        }
        impl_->hairSupportDrawable = backing;
        impl_->hairSupportScaleX = static_cast<float>(supportScale[0].toDouble());
        impl_->hairSupportScaleY = static_cast<float>(supportScale[1].toDouble());
    }
    if (metadata.object().contains(QStringLiteral("runtimeDeskWorkMode"))) {
        const auto desk = metadata.object().value(QStringLiteral("runtimeDeskWorkMode")).toObject();
        const auto stage = desk.value(QStringLiteral("stage")).toString();
        const auto expectedMoc = desk.value(QStringLiteral("mocSha256")).toString().toLatin1();
        const auto ids = desk.value(QStringLiteral("drawableIds")).toArray();
        const QStringList expectedIds{QStringLiteral("ArtMeshWorkstationDesk"), QStringLiteral("ArtMeshWorkstationDeskTop")};
        bool meshesValid = ids.size() == expectedIds.size();
        for (int i = 0; i < ids.size() && meshesValid; ++i) {
            meshesValid = ids[i].toString() == expectedIds[i];
            const auto name = ids[i].toString().toUtf8();
            const int index = model->GetDrawableIndex(Csm::CubismFramework::GetIdManager()->GetId(name.constData()));
            meshesValid = meshesValid && index >= 0;
            if (meshesValid) impl_->deskDrawables.push_back(index);
        }
        if (desk.value(QStringLiteral("version")).toInt() != 1
            || desk.value(QStringLiteral("parameter")).toString() != QStringLiteral("ParamDeskVisible")
            || desk.value(QStringLiteral("floorOwner")).toString() != QStringLiteral("runtime")
            || (stage != QStringLiteral("complete")
                && !(stage == QStringLiteral("preview") && !reviewModel.isEmpty()))
            || expectedMoc.size() != 64
            || QCryptographicHash::hash(moc, QCryptographicHash::Sha256).toHex() != expectedMoc
            || !impl_->parameterIndices.contains(QStringLiteral("ParamDeskVisible"))
            || !meshesValid
            || metadata.object().contains(QStringLiteral("runtimePostureTransition"))) {
            error_ = QStringLiteral("Desk work mode metadata or MOC identity is invalid");
            return;
        }
        if (desk.contains(QStringLiteral("contactTrim"))) {
            error_ = QStringLiteral("Desk hand trimming was rejected; use the complete sleeve materials");
            return;
        }
        if (desk.contains(QStringLiteral("replacementLaptop"))) {
            const auto id = desk.value(QStringLiteral("replacementLaptop")).toString().toUtf8();
            const int replacement = model->GetDrawableIndex(Csm::CubismFramework::GetIdManager()->GetId(id.constData()));
            for (const auto* name : {"ArtMeshObjects", "ArtMeshObjects4"}) {
                const int legacy = model->GetDrawableIndex(Csm::CubismFramework::GetIdManager()->GetId(name));
                if (legacy < 0) { error_ = QStringLiteral("Legacy desk laptop material is missing"); return; }
                impl_->deskLegacyMaterials.push_back(legacy);
            }
            if (id != QByteArray("ArtMeshWorkstationLaptop") || replacement < 0) {
                error_ = QStringLiteral("Whole rigid desk laptop material is missing");
                return;
            }
        }
        if (desk.contains(QStringLiteral("replacementSleeves"))) {
            const auto replacements = desk.value(QStringLiteral("replacementSleeves")).toObject();
            const QJsonObject expected{
                {QStringLiteral("ArtMeshObjects2"), QStringLiteral("ArtMeshWorkstationSleeveR")}
            };
            if (replacements != expected || !desk.contains(QStringLiteral("replacementLaptop"))
                || !desk.value(QStringLiteral("hiddenLeftHand")).toBool()) {
                error_ = QStringLiteral("Complete workstation sleeves require their matching rigid laptop");
                return;
            }
            // The left arm stays behind the opaque laptop in this scene.
            // Its incomplete legacy cutout is not a visible scene component.
            const int left = model->GetDrawableIndex(Csm::CubismFramework::GetIdManager()->GetId("ArtMeshObjects3"));
            if (left < 0) { error_ = QStringLiteral("Legacy left sleeve material is missing"); return; }
            impl_->deskLegacyMaterials.push_back(left);
            for (auto it = expected.begin(); it != expected.end(); ++it) {
                const auto legacyId = it.key().toUtf8();
                const auto replacementId = it.value().toString().toUtf8();
                const int legacy = model->GetDrawableIndex(Csm::CubismFramework::GetIdManager()->GetId(legacyId.constData()));
                const int replacement = model->GetDrawableIndex(Csm::CubismFramework::GetIdManager()->GetId(replacementId.constData()));
                if (legacy < 0 || replacement < 0) {
                    error_ = QStringLiteral("Complete workstation sleeve material is missing");
                    return;
                }
                impl_->deskLegacyMaterials.push_back(legacy);
            }
        }
    }
    if (metadata.object().contains(QStringLiteral("runtimePostureTransition"))) {
        const auto posture = metadata.object().value(QStringLiteral("runtimePostureTransition")).toObject();
        const auto stage = posture.value(QStringLiteral("stage")).toString();
        const bool structuralReview = stage == QStringLiteral("structural-review") && !reviewModel.isEmpty();
        const auto expectedMoc = posture.value(QStringLiteral("mocSha256")).toString().toLatin1();
        if (posture.value(QStringLiteral("version")).toDouble() != 2
            || posture.value(QStringLiteral("logicalToNative")).toString() != QStringLiteral("identity")
            || posture.value(QStringLiteral("geometryOwner")).toString() != QStringLiteral("author-model")
            || (stage != QStringLiteral("complete") && !structuralReview)
            || expectedMoc.size() != 64
            || QCryptographicHash::hash(moc, QCryptographicHash::Sha256).toHex() != expectedMoc
            || metadata.object().value(QStringLiteral("runtimeNeckConnection")).toObject()
                .value(QStringLiteral("independentSurface")).toObject().isEmpty()) {
            error_ = QStringLiteral("Authored posture metadata or MOC identity is invalid");
            return;
        }
        const auto coupled = posture.value(QStringLiteral("coupledParameters")).toObject();
        const QString skirt = coupled.value(QStringLiteral("skirtSpread")).toString();
        const QString hand = coupled.value(QStringLiteral("handGround")).toString();
        if ((!skirt.isEmpty() && skirt != QStringLiteral("ParamSkirtSpread"))
            || (!hand.isEmpty() && hand != QStringLiteral("ParamHandGround"))
            || (!structuralReview && (skirt.isEmpty() || hand.isEmpty()))) {
            error_ = QStringLiteral("Authored posture requires functional skirt and support parameters");
            return;
        }
        for (const QString& id : {skirt, hand}) {
            if (!id.isEmpty() && !impl_->parameterIndices.contains(id)) {
                error_ = QStringLiteral("Authored posture parameter is missing: ") + id;
                return;
            }
        }
        impl_->skirtSpreadParameter = skirt.isEmpty() ? -1 : impl_->parameterIndices.value(skirt);
        impl_->handGroundParameter = hand.isEmpty() ? -1 : impl_->parameterIndices.value(hand);
        impl_->authoredSitPose = true;
    }
    const QJsonObject separation = metadata.object().value(QStringLiteral("runtimeMaterialSeparation")).toObject();
    const QJsonArray textures = refs.value(QStringLiteral("Textures")).toArray();
    if (textures.isEmpty()) {
        error_ = QStringLiteral("Cubism texture list is empty");
        return;
    }
    int bodyTextureIndex = -1;
    QImage bodyMask, duplicateMask, underpaint, neckMask, neckEraseMask;
    const bool connectNeck = metadata.object().contains(QStringLiteral("runtimeNeckConnection"));
    const auto independentNeck = metadata.object().value(QStringLiteral("runtimeNeckConnection")).toObject()
        .value(QStringLiteral("independentSurface")).toObject();
    QJsonObject ownership;
    const auto verifiedBytes = [&](const QByteArray& bytes, const char* hashKey) {
        const auto expected = ownership.value(QString::fromLatin1(hashKey)).toString().toLatin1();
        return !bytes.isEmpty() && expected.size() == 64
            && QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() == expected;
    };
    if (metadata.object().contains(QStringLiteral("runtimeMaterialSeparation"))) {
        bodyTextureIndex = separation.value(QStringLiteral("textureIndex")).toInt(-1);
        if (bodyTextureIndex < 0 || bodyTextureIndex >= textures.size()
            || separation.value(QStringLiteral("textureIndex")).toDouble(-1) != bodyTextureIndex) {
            error_ = QStringLiteral("Material separation texture index is invalid");
            return;
        }
        ownership = QJsonDocument::fromJson(readFile(QDir(directory).filePath(
            separation.value(QStringLiteral("ownership")).toString()))).object();
        const QByteArray bodyMaskBytes = readFile(QDir(directory).filePath(
            separation.value(QStringLiteral("bodyMask")).toString()));
        const QByteArray duplicateMaskBytes = readFile(QDir(directory).filePath(
            separation.value(QStringLiteral("duplicateMask")).toString()));
        const QByteArray underpaintBytes = readFile(QDir(directory).filePath(
            separation.value(QStringLiteral("underpaint")).toString()));
        if (!verifiedBytes(moc, "moc_sha256")
            || !verifiedBytes(bodyMaskBytes, "body_mask_sha256")
            || !verifiedBytes(duplicateMaskBytes, "duplicate_mask_sha256")
            || !verifiedBytes(underpaintBytes, "underpaint_sha256")) {
            error_ = QStringLiteral("Material ownership masks do not match the current model");
            return;
        }
        bodyMask = QImage::fromData(bodyMaskBytes);
        duplicateMask = QImage::fromData(duplicateMaskBytes);
        underpaint = QImage::fromData(underpaintBytes).convertToFormat(QImage::Format_RGBA8888_Premultiplied);
        impl_->bodyTextureIndex = bodyTextureIndex;
        impl_->bodyModel = std::make_unique<PetCubismModel>();
        impl_->bodyModel->LoadModel(reinterpret_cast<const Csm::csmByte*>(moc.constData()),
                                   static_cast<Csm::csmSizeInt>(moc.size()));
        if (!impl_->bodyModel->GetModel()
            || !impl_->materialBinding.configure(model->GetModel(),
                impl_->bodyModel->GetModel()->GetModel(), metadata.object(), &error_)) {
            if (error_.isEmpty()) error_ = QStringLiteral("Could not load body material reference");
            return;
        }
        if (Live2D::Cubism::Core::csmGetDrawableTextureIndices(model->GetModel())[
                impl_->materialBinding.bodyDrawableIndex()] != bodyTextureIndex) {
            error_ = QStringLiteral("Body material drawable uses a different atlas page");
            return;
        }
        if (connectNeck) {
            const QByteArray neckBytes = readFile(QDir(directory).filePath(
                separation.value(QStringLiteral("neckMask")).toString()));
            const QByteArray neckEraseBytes = readFile(QDir(directory).filePath(
                separation.value(QStringLiteral("neckEraseMask")).toString()));
            if (!verifiedBytes(neckBytes, "neck_mask_sha256")
                || !verifiedBytes(neckEraseBytes, "neck_erase_mask_sha256")) {
                error_ = QStringLiteral("Neck ownership masks do not match the current model");
                return;
            }
            neckMask = QImage::fromData(neckBytes);
            neckEraseMask = QImage::fromData(neckEraseBytes);
            if (!impl_->neckBinding.configure(model->GetModel(), metadata.object(), ownership, &error_)) return;
            if (impl_->neckBinding.faceDrawableIndex() != impl_->materialBinding.bodyDrawableIndex()) {
                error_ = QStringLiteral("Neck and body material references must use the same Face drawable");
                return;
            }
        }
    } else if (connectNeck) {
        error_ = QStringLiteral("Neck connection requires separated body materials");
        return;
    }
    const auto upload = [](const QImage& image) {
        GLuint texture;
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image.width(), image.height(), 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, image.constBits());
        glGenerateMipmap(GL_TEXTURE_2D);
        return texture;
    };
    for (int textureIndex = 0; textureIndex < textures.size(); ++textureIndex) {
        const QString path = textures[textureIndex].toString();
        const QByteArray textureBytes = readFile(QDir(directory).filePath(path));
        QImage image = QImage::fromData(textureBytes);
        if (image.isNull()) {
            error_ = QStringLiteral("Cubism texture is missing: %1").arg(path);
            return;
        }
        QImage clothing;
        if (textureIndex == bodyTextureIndex) {
            if (!verifiedBytes(textureBytes, "atlas_sha256")) {
                error_ = QStringLiteral("Material ownership masks do not match the current atlas");
                return;
            }
            if (underpaint.isNull() || underpaint.size() != image.size()) {
                error_ = QStringLiteral("Hidden clothing underpaint must match the current atlas dimensions");
                return;
            }
            const QImage originalAtlas = image;
            if (!splitCubismMaterialTexture(image, bodyMask, duplicateMask, &image, &clothing, &error_)) return;
            if (connectNeck) {
                QImage neck;
                if (!splitCubismNeckTexture(originalAtlas, neckMask, neckEraseMask, &image, &neck, &error_)) return;
                if (independentNeck.isEmpty()) impl_->neckTexture = upload(neck);
            }
            impl_->bodyUnderpaintTexture = upload(underpaint);
        } else {
            image = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
            if (impl_->bodyModel) {
                clothing = QImage(image.size(), QImage::Format_RGBA8888_Premultiplied);
                clothing.fill(Qt::transparent);
            }
        }
        if (textureIndex == 0 && !backdropChart.isEmpty()) {
            if (QCryptographicHash::hash(textureBytes, QCryptographicHash::Sha256).toHex() != backdropAtlasSha) {
                error_ = QStringLiteral("Backdrop cleanup atlas identity is invalid");
                return;
            }
            QImage cleaned;
            // Two rear locks and the complete ahoge are genuine components.
            // The smaller disconnected ear tips already belong to the head.
            if (!removeCubismBackdropIslands(image, backdropChart, &cleaned, &error_, 3)) return;
            image = std::move(cleaned);
            const QImage beforeBacking = image.copy();
            {
                QPainter painter(&image);
                painter.setRenderHint(QPainter::SmoothPixmapTransform);
                painter.setCompositionMode(QPainter::CompositionMode_Source);
                for (int i = 0; i < hairBackingTargets.size(); ++i)
                    painter.drawImage(hairBackingTargets[i], hairBacking, hairBackingSources[i]);
            }
            // Keep ownership within the original backing silhouette. The
            // generated material supplies hair; its chart never supplies skin
            // or skirt pixels and never expands over another atlas tile.
            for (const auto& rect : hairBackingTargets) {
                for (int y = rect.top(); y <= rect.bottom(); ++y) {
                    auto* row = image.scanLine(y);
                    const auto* original = beforeBacking.constScanLine(y);
                    for (int x = rect.left(); x <= rect.right(); ++x) {
                        const int coverage = original[4*x+3];
                        for (int channel = 0; channel < 4; ++channel)
                            row[4*x+channel] = static_cast<uchar>((int(row[4*x+channel])*coverage+127)/255);
                    }
                }
            }
        }
        impl_->textures.push_back(upload(image));
        if (impl_->bodyModel) impl_->bodyTextures.push_back(upload(clothing));
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    if (!independentNeck.isEmpty()) {
        if (!connectNeck || !impl_->bodyModel) {
            error_ = QStringLiteral("Independent neck requires the existing exclusive skin ownership masks");
            return;
        }
        const QString neckSettingsPath = QDir(directory).filePath(independentNeck.value(QStringLiteral("model")).toString());
        const QString neckDirectory = QFileInfo(neckSettingsPath).absolutePath();
        const auto neckSettings = QJsonDocument::fromJson(readFile(neckSettingsPath)).object();
        const auto neckRefs = neckSettings.value(QStringLiteral("FileReferences")).toObject();
        const auto neckMoc = readFile(QDir(neckDirectory).filePath(neckRefs.value(QStringLiteral("Moc")).toString()));
        const auto rigBytes = readFile(QDir(directory).filePath(independentNeck.value(QStringLiteral("rig")).toString()));
        const auto matches = [&](const QByteArray& bytes, const char* key) {
            const auto expected = independentNeck.value(QString::fromLatin1(key)).toString().toLatin1();
            return !bytes.isEmpty() && expected.size() == 64
                && QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() == expected;
        };
        if (!matches(neckMoc, "mocSha256") || !matches(rigBytes, "rigSha256")) {
            error_ = QStringLiteral("Independent neck MOC or rig does not match the declared component");
            return;
        }
        impl_->neckModel = std::make_unique<PetCubismModel>();
        impl_->neckModel->LoadModel(reinterpret_cast<const Csm::csmByte*>(neckMoc.constData()),
                                    static_cast<Csm::csmSizeInt>(neckMoc.size()), true);
        if (!impl_->neckModel->GetModel()) {
            error_ = QStringLiteral("Cubism Core rejected the independent neck MOC");
            return;
        }
        if (!impl_->neckBinding.configureSurface(impl_->neckModel->GetModel()->GetModel(),
                QJsonDocument::fromJson(rigBytes).object(), &error_)) return;
        const auto neckTextureRefs = neckRefs.value(QStringLiteral("Textures")).toArray();
        if (neckTextureRefs.size() != 1) {
            error_ = QStringLiteral("Independent neck must use exactly one atlas page");
            return;
        }
        const auto bytes = readFile(QDir(neckDirectory).filePath(neckTextureRefs[0].toString()));
        const auto texture = QImage::fromData(bytes).convertToFormat(QImage::Format_RGBA8888_Premultiplied);
        if (!matches(bytes, "atlasSha256") || texture.isNull()) {
            error_ = QStringLiteral("Independent neck atlas does not match its component");
            return;
        }
        impl_->neckTextures.push_back(upload(texture));
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    impl_->model->GetModelMatrix()->SetHeight(1.90f);
    const int bodyPart = model->GetPartIndex(Csm::CubismFramework::GetIdManager()->GetId("PartBody"));
    const auto* parentParts = Live2D::Cubism::Core::csmGetDrawableParentPartIndices(model->GetModel());
    for (int index = 0; index < model->GetDrawableCount(); ++index) {
        if (bodyPart >= 0 && parentParts[index] == bodyPart) impl_->standingMeshes.push_back(index);
    }
    for (const auto& layer : metadata.object().value(QStringLiteral("layers")).toArray()) {
        const auto entry = layer.toObject();
        const auto source = entry.value(QStringLiteral("source")).toString();
        const bool seatedMesh = source.startsWith(QStringLiteral("busy "));
        const bool standingFoot = source.startsWith(QStringLiteral("footwear-"));
        const bool headMesh = source == QStringLiteral("face")
            || source == QStringLiteral("front hair") || source == QStringLiteral("headwear");
        const bool grassMesh = source == QStringLiteral("handwear right");
        if (!seatedMesh && !standingFoot && !headMesh && !grassMesh) continue;
        const auto id = entry.value(QStringLiteral("drawable")).toString().toUtf8();
        const int index = model->GetDrawableIndex(Csm::CubismFramework::GetIdManager()->GetId(id.constData()));
        if (index < 0) continue;
        if (seatedMesh) impl_->seatedMeshes.push_back(index);
        if (standingFoot) impl_->standingFeet.push_back(index);
        if (headMesh) impl_->headMeshes.push_back(index);
        if (source == QStringLiteral("face")) impl_->faceMesh = index;
        if (source.startsWith(QStringLiteral("busy leg "))) impl_->seatedFeet.push_back(index);
        if (grassMesh) {
            impl_->grassMesh = index;
            const auto* uv = model->GetDrawableVertexUvs(index);
            float minU = 1, maxU = 0, minV = 1, maxV = 0;
            for (int i = 0; i < model->GetDrawableVertexCount(index); ++i) {
                minU = std::min(minU, uv[i].X); maxU = std::max(maxU, uv[i].X);
                minV = std::min(minV, uv[i].Y); maxV = std::max(maxV, uv[i].Y);
            }
            // UVs remain attached to the deformed vertices. This source's
            // upper 24%, left 38% covers the fuzzy seed head (PSD y494..588),
            // excluding its leaf and the shaft through the palm. Normalizing
            // to this mesh avoids depending on atlas packing coordinates.
            impl_->grassTipMinV = maxV - 0.24f * (maxV - minV);
            impl_->grassTipMaxU = minU + 0.38f * (maxU - minU);
        }
    }
    model->Update();
    impl_->standingFloor = impl_->footFloor(impl_->standingFeet);
    impl_->model->CreateRenderer(static_cast<Csm::csmUint32>(width() * devicePixelRatioF()),
                                 static_cast<Csm::csmUint32>(height() * devicePixelRatioF()));
    if (impl_->bodyModel)
        impl_->bodyModel->CreateRenderer(static_cast<Csm::csmUint32>(width() * devicePixelRatioF()),
                                        static_cast<Csm::csmUint32>(height() * devicePixelRatioF()));
    if (impl_->neckModel)
        impl_->neckModel->CreateRenderer(static_cast<Csm::csmUint32>(width() * devicePixelRatioF()),
                                        static_cast<Csm::csmUint32>(height() * devicePixelRatioF()));
    impl_->bindTextures();
    ready_ = true;
    emit readyChanged(true);
}

void CubismCanvas::resizeGL(int width, int height) {
    if (!ready_ || !impl_->model || width <= 0 || height <= 0) return;
    impl_->model->CreateRenderer(static_cast<Csm::csmUint32>(width),
                                 static_cast<Csm::csmUint32>(height));
    if (impl_->bodyModel)
        impl_->bodyModel->CreateRenderer(static_cast<Csm::csmUint32>(width),
                                        static_cast<Csm::csmUint32>(height));
    if (impl_->neckModel)
        impl_->neckModel->CreateRenderer(static_cast<Csm::csmUint32>(width),
                                        static_cast<Csm::csmUint32>(height));
    impl_->bindTextures();
}

void CubismCanvas::paintGL() {
    if (sampleCount_ < 0) {
        GLint samples = 0;
        glGetIntegerv(GL_SAMPLES, &samples);
        sampleCount_ = samples;
    }
    glViewport(0, 0, static_cast<GLsizei>(width() * devicePixelRatioF()),
               static_cast<GLsizei>(height() * devicePixelRatioF()));
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!ready_ || !impl_->model) return;

    auto* model = impl_->model->GetModel();
    model->LoadParameters();
    if (impl_->rawExportReview) {
        for (auto it = motion_->values().cbegin(); it != motion_->values().cend(); ++it) {
            const auto index = impl_->parameterIndices.constFind(it.key());
            if (index != impl_->parameterIndices.cend())
                model->SetParameterValue(*index, static_cast<float>(it.value()));
        }
        frameSeconds_ = 0.0;
        model->Update();
        headHitPath_ = QPainterPath();
        grassTipHitPath_ = QPainterPath();
        Csm::CubismMatrix44 rawMatrix;
        rawMatrix.MultiplyByMatrix(impl_->model->GetModelMatrix());
        auto* renderer = impl_->model->GetRenderer<Csm::Rendering::CubismRenderer_OpenGLES2>();
        renderer->SetMvpMatrix(&rawMatrix);
        auto* offscreen = Csm::Rendering::CubismOffscreenManager_OpenGLES2::GetInstance();
        offscreen->BeginFrameProcess();
        renderer->DrawModel();
        offscreen->EndFrameProcess();
        return;
    }
    // The pose remains continuous, but the two painted bodies are exclusive.
    // After grounding, their collars are aligned to the same moving target;
    // selecting the material there avoids both ghosting and a collar jump.
    const double busyValue = motion_->values().value(QStringLiteral("ParamBusyLaptop"));
    const double sitValue = motion_->values().value(QStringLiteral("ParamSitPose"));
    const auto posture = impl_->authoredSitPose
        ? CubismPostureTransition::sampleAuthored(busyValue, sitValue)
        : CubismPostureTransition::sample(busyValue, sitValue);
    if (!posture.valid) return;
    // The visible sit-down (card-approved: she sits before the desk arrives)
    // descends the collar affine with logical SitPose; the busy texture mix
    // keeps its own late gate. Whichever is further along owns the collar.
    const double seatedMix = std::max(posture.seatedMix,
        CubismPostureTransition::sitDrive(sitValue));
    const double seatedMaterial = posture.nativeMaterial();
    // MouthOpenY only ever shapes the bite art while its switch is on (or the
    // open-smile art, its legacy consumer). Two failure modes live on this
    // parameter, both fixed at the single write site:
    //  - With the switch closed, a lagging MouthOpenY -- the per-state
    //    exponential blend keeps values_ trailing the keyframes for a few
    //    frames after the bite snaps shut -- deforms the resting omega into a
    //    wide phantom grin, which reads as two mouths (review f24-f26).
    //  - With the switch open, any authored value below 1 deforms the omega
    //    out of the bite aperture's cover: the omega curl pokes out above the
    //    gape and both mouths show at once (review f19). Fully open is the
    //    only clean bite state, and the clip authors the open/shut as instant
    //    jumps anyway, so the mouth is quantised to strictly binary here.
    const double rawGape = motion_->values().value(QStringLiteral("ParamMouthGape"));
    const double rawSmile = motion_->values().value(QStringLiteral("ParamSmileOpen"));
    const bool gapeOn = rawGape >= 0.5;
    const bool smileArtOn = rawSmile >= 0.5;
    for (auto it = motion_->values().cbegin(); it != motion_->values().cend(); ++it) {
        const auto index = impl_->parameterIndices.constFind(it.key());
        if (index == impl_->parameterIndices.cend()) continue;
        float value = static_cast<float>(it.value());
        if (it.key() == QStringLiteral("ParamBusyLaptop")) value = static_cast<float>(seatedMaterial);
        else if (it.key() == QStringLiteral("ParamSitPose")) value = static_cast<float>(posture.standingFold);
        else if (it.key() == QStringLiteral("ParamMouthOpenY")) {
            if (gapeOn) value = 1.0f;
            else if (!smileArtOn) value = 0.0f;
        }
        else if (it.key() == QStringLiteral("ParamMouthGape"))
            // The gape is a pure art selector: the layer is a switch on this
            // parameter, and the resting omega keeps showing underneath while
            // the switch crossfades -- a sampled 0.4 renders TWO mouths at
            // once. Every consumer samples on an arbitrary clock (live frame
            // timer, review captures), so the discretisation has to live here
            // at the single write site, not in per-track keyframes. Opening
            // size is MouthOpenY's job, which deforms the opaque gape art.
            value = value >= 0.5f ? 1.0f : 0.0f;
        model->SetParameterValue(*index, value);
    }
    if (impl_->authoredSitPose) {
        if (impl_->skirtSpreadParameter >= 0)
            model->SetParameterValue(impl_->skirtSpreadParameter,
                static_cast<float>(motion_->values().value(QStringLiteral("ParamSkirtSpread"),
                    CubismPostureTransition::authoredSkirtSpread(posture.standingFold))));
        if (impl_->handGroundParameter >= 0) {
            // Keep the existing grass grip while its prop is visible. The
            // ground support arms share those hand materials.
            const double handGround = motion_->values().value(QStringLiteral("ParamGrassVisible")) > 0.01
                ? 0.0 : motion_->values().value(QStringLiteral("ParamHandGround"),
                    CubismPostureTransition::authoredHandGround(posture.standingFold));
            model->SetParameterValue(impl_->handGroundParameter, static_cast<float>(handGround));
        }
    }
    const double physicsSeconds = std::exchange(frameSeconds_, 0.0);
    if (!motion_->frozenPhysics() && physicsSeconds > 0)
        impl_->model->evaluatePhysics(static_cast<float>(physicsSeconds));
    // ParamSitPose bends the standing knees. Swap body material
    // near the folded pose without drawing two translucent bodies;
    // the laptop has its own native visibility parameter. Existing grip/grass
    // tracks stay intact; head, hair and tail are shared by both poses.
    model->SetPartOpacity(Csm::CubismFramework::GetIdManager()->GetId("PartBody"),
        static_cast<float>(1.0 - seatedMaterial));
    model->Update();
    impl_->materialBinding.apply();
    if (impl_->hairSupportDrawable >= 0) {
        // The old backing was cropped around a neutral sleeve/skirt silhouette.
        // Give only the pure hair support enough overlap under moving clothing;
        // the real arms, body and outer hair keep their authored geometry.
        const int index = impl_->hairSupportDrawable;
        auto* vertices = const_cast<Core::csmVector2*>(Core::csmGetDrawableVertexPositions(model->GetModel())[index]);
        const auto* uv = model->GetDrawableVertexUvs(index);
        const int count = model->GetDrawableVertexCount(index);
        for (int side = 0; side < 2; ++side) {
            float left = std::numeric_limits<float>::infinity(), right = -left, top = -left;
            for (int i = 0; i < count; ++i) {
                if ((uv[i].X * 4096.0 < 3298.0) != (side == 0)) continue;
                left = std::min(left, vertices[i].X); right = std::max(right, vertices[i].X);
                top = std::max(top, vertices[i].Y);
            }
            const float centre = (left + right) * 0.5f;
            for (int i = 0; i < count; ++i) {
                if ((uv[i].X * 4096.0 < 3298.0) != (side == 0)) continue;
                vertices[i].X = centre + (vertices[i].X - centre) * impl_->hairSupportScaleX;
                vertices[i].Y = top + (vertices[i].Y - top) * impl_->hairSupportScaleY;
            }
        }
    }
    if (!impl_->deskLegacyMaterials.empty() && motion_->values().value(QStringLiteral("ParamDeskVisible")) > 0.0) {
        auto* opacity = const_cast<float*>(Core::csmGetDrawableOpacities(model->GetModel()));
        for (int legacy : impl_->deskLegacyMaterials) opacity[legacy] = 0.0f;
    }
    // The bite art's switch-opacity keyforms never made it into the exported
    // moc3 (the layer stays fully opaque no matter what ParamMouthGape says),
    // so gate its visibility here instead: Core keeps the live per-drawable
    // opacity array and the renderer reads it after this point, between now
    // and the next Update. Driven by the same discrete gape switch as the
    // parameter write above, so every consumer (live timer, review captures)
    // sees exactly one mouth state: resting omega, or fully open bite.
    if (impl_->gapeDrawable >= 0) {
        const float* opacities = Live2D::Cubism::Core::csmGetDrawableOpacities(model->GetModel());
        const_cast<float*>(opacities)[impl_->gapeDrawable] = gapeOn ? 1.0f : 0.0f;
    }

    // Keep the original global floor reference while BodyY/breath changes.
    // Authored legs already keep their complete shoes in the same Body context
    // as Sit0, so this single model translation cancels only that existing
    // context offset. Legacy material registration needs its additional seated
    // mesh translation; authored Sit geometry must never receive that shift.
    const float standingFoot = impl_->footFloor(impl_->standingFeet);
    const float seatedFoot = impl_->footFloor(impl_->seatedFeet);
    CubismPostureTransition::FloorPinnedTransform bodyClothTransform;
    Csm::CubismMatrix44 matrix;
    matrix.MultiplyByMatrix(impl_->model->GetModelMatrix());
    if (std::isfinite(standingFoot) && std::isfinite(impl_->standingFloor)
        && (impl_->authoredSitPose || std::isfinite(seatedFoot))) {
        auto* modelMatrix = impl_->model->GetModelMatrix();
        const float grounded = modelMatrix->TransformY(impl_->standingFloor)
            - modelMatrix->TransformY(standingFoot);
        matrix.Translate(matrix.GetTranslateX(), matrix.GetTranslateY() + grounded);
        for (int deskDrawable : impl_->deskDrawables) {
            // Furniture keeps its own ground. Cancel the character's global
            // foot correction on this prop only; Core rebuilds it next frame.
            using Position = std::remove_const_t<std::remove_pointer_t<
                decltype(model->GetDrawableVertexPositions(0))>>;
            auto* vertices = const_cast<Position*>(model->GetDrawableVertexPositions(deskDrawable));
            const int count = model->GetDrawableVertexCount(deskDrawable);
            for (int i = 0; i < count; ++i)
                vertices[i].Y += standingFoot - impl_->standingFloor;
        }
        if (!impl_->authoredSitPose) {
            // Update() rebuilds this buffer every frame. The legacy correction
            // changes only this draw call and never the author model.
            using Position = std::remove_const_t<std::remove_pointer_t<
                decltype(model->GetDrawableVertexPositions(0))>>;
            const float seatedShift = standingFoot - seatedFoot;
            for (int index : impl_->seatedMeshes) {
                auto* vertices = const_cast<Position*>(model->GetDrawableVertexPositions(index));
                const int count = model->GetDrawableVertexCount(index);
                for (int i = 0; i < count; ++i) vertices[i].Y += seatedShift;
            }
        }
    }
    if (impl_->hasNeck() && !impl_->authoredSitPose) {
        std::array<double, 2> standing{}, seated{};
        CubismPostureTransition::Point target;
        const bool anchorsReady = impl_->neckBinding.collarPositions(&standing, &seated, &error_)
            && CubismPostureTransition::collarTarget({standing[0], standing[1]},
                {seated[0], seated[1]}, seatedMix, &target);
        const CubismPostureTransition::FloorPinnedTransform standingTransform{
            {standing[0], standing[1]}, target, standingFoot};
        const CubismPostureTransition::FloorPinnedTransform seatedTransform{
            {seated[0], seated[1]}, target, standingFoot};
        bodyClothTransform = standingTransform;
        bool aligned = anchorsReady && standingTransform.valid() && seatedTransform.valid();
        if (aligned) {
            // Each group's affine map pins its sole and reaches the common
            // collar exactly. Shared head/hair/tail geometry stays untouched.
            for (int index : impl_->standingMeshes)
                aligned = alignDrawable(model->GetModel(), index, standingTransform) && aligned;
            for (int index : impl_->seatedMeshes)
                aligned = alignDrawable(model->GetModel(), index, seatedTransform) && aligned;
        }
        if (!aligned) {
            if (error_.isEmpty()) error_ = QStringLiteral("Posture collar alignment is invalid");
            qWarning() << "Posture connection:" << error_;
            ready_ = false;
            emit readyChanged(false);
            return;
        }
    }
    // Resolve the neck after floor and collar alignment. Its top remains on
    // the main head; its tip follows the exclusively visible collar. Both
    // materials already meet the continuous target before selection.
    if (impl_->hasNeck() && !impl_->neckBinding.update(seatedMaterial, &error_)) {
        qWarning() << "Neck connection:" << error_;
        ready_ = false;
        emit readyChanged(false);
        return;
    }
    // Input geometry is captured after Update and after grounding, at the
    // exact pose that is about to be rendered. Front hair extends below the
    // chin; clip it there so stroking a long lock is still a body drag.
    headHitPath_ = QPainterPath();
    headHitPath_.setFillRule(Qt::WindingFill);
    QRectF faceBounds;
    const auto point = [&](const auto& vertex) {
        return QPointF((matrix.TransformX(vertex.X) + 1.0) * width() / 2.0,
                       (1.0 - matrix.TransformY(vertex.Y)) * height() / 2.0);
    };
    for (int index : impl_->headMeshes) {
        const auto* vertices = model->GetDrawableVertexPositions(index);
        const auto* indices = model->GetDrawableVertexIndices(index);
        const int count = model->GetDrawableVertexIndexCount(index);
        QPainterPath mesh;
        mesh.setFillRule(Qt::WindingFill);
        for (int i = 0; i + 2 < count; i += 3) {
            QPolygonF triangle;
            triangle << point(vertices[indices[i]]) << point(vertices[indices[i + 1]])
                     << point(vertices[indices[i + 2]]);
            mesh.addPolygon(triangle);
            mesh.closeSubpath();
        }
        if (index == impl_->faceMesh) faceBounds = mesh.boundingRect();
        headHitPath_.addPath(mesh);
    }
    if (!faceBounds.isEmpty()) {
        QPainterPath aboveChin;
        aboveChin.addRect(QRectF(0, 0, width(), faceBounds.bottom()));
        headHitPath_ = headHitPath_.intersected(aboveChin);
    }
    grassTipHitPath_ = QPainterPath();
    if (impl_->grassMesh >= 0 && motion_->values().value(QStringLiteral("ParamGrassVisible")) >= 0.35
        && model->GetDrawableOpacity(impl_->grassMesh) >= 0.2f) {
        const int index = impl_->grassMesh;
        const auto* vertices = model->GetDrawableVertexPositions(index);
        const auto* uv = model->GetDrawableVertexUvs(index);
        const auto* indices = model->GetDrawableVertexIndices(index);
        struct HitVertex { QPointF uv, pixel; };
        const auto clip = [](const std::vector<HitVertex>& input, bool vertical, double boundary) {
            std::vector<HitVertex> output;
            if (input.empty()) return output;
            const auto coordinate = [vertical](const HitVertex& v) { return vertical ? v.uv.y() : v.uv.x(); };
            const auto inside = [&](const HitVertex& v) {
                return vertical ? coordinate(v) >= boundary : coordinate(v) <= boundary;
            };
            auto previous = input.back();
            for (const auto& current : input) {
                if (inside(previous) != inside(current)) {
                    const double t = (boundary - coordinate(previous)) / (coordinate(current) - coordinate(previous));
                    output.push_back({previous.uv + t * (current.uv - previous.uv),
                                      previous.pixel + t * (current.pixel - previous.pixel)});
                }
                if (inside(current)) output.push_back(current);
                previous = current;
            }
            return output;
        };
        std::vector<QPointF> tipPoints;
        for (int i = 0; i + 2 < model->GetDrawableVertexIndexCount(index); i += 3) {
            std::vector<HitVertex> polygon;
            for (int j = 0; j < 3; ++j) {
                const int vertex = indices[i + j];
                polygon.push_back({QPointF(uv[vertex].X, uv[vertex].Y), point(vertices[vertex])});
            }
            polygon = clip(clip(polygon, true, impl_->grassTipMinV), false, impl_->grassTipMaxU);
            if (polygon.size() < 3) continue;
            for (const auto& vertex : polygon) tipPoints.push_back(vertex.pixel);
        }
        // A fuzzy seed head needs a small, forgiving region. Its convex hull
        // also avoids boolean unions of hundreds of touching mesh triangles,
        // which can stall QPainterPath on degenerate exported keyforms.
        std::sort(tipPoints.begin(), tipPoints.end(), [](const QPointF& a, const QPointF& b) {
            return a.x() < b.x() || (a.x() == b.x() && a.y() < b.y());
        });
        tipPoints.erase(std::unique(tipPoints.begin(), tipPoints.end()), tipPoints.end());
        const auto cross = [](const QPointF& a, const QPointF& b, const QPointF& c) {
            return (b.x() - a.x()) * (c.y() - a.y()) - (b.y() - a.y()) * (c.x() - a.x());
        };
        std::vector<QPointF> hull;
        for (const auto& p : tipPoints) {
            while (hull.size() >= 2 && cross(hull[hull.size() - 2], hull.back(), p) <= 0) hull.pop_back();
            hull.push_back(p);
        }
        const auto lowerSize = hull.size();
        for (auto it = tipPoints.rbegin(); it != tipPoints.rend(); ++it) {
            while (hull.size() > lowerSize && cross(hull[hull.size() - 2], hull.back(), *it) <= 0) hull.pop_back();
            hull.push_back(*it);
        }
        if (!hull.empty()) hull.pop_back();
        QPolygonF pixels;
        for (const auto& p : hull) pixels << p;
        if (pixels.size() >= 3) {
            grassTipHitPath_.addPolygon(pixels);
            grassTipHitPath_.closeSubpath();
        }
        QPainterPathStroker padding;
        padding.setWidth(width() * 0.03); // about 4px on each side at desktop size
        padding.setCapStyle(Qt::RoundCap);
        padding.setJoinStyle(Qt::RoundJoin);
        grassTipHitPath_ = grassTipHitPath_.united(padding.createStroke(grassTipHitPath_));
    }
    auto* renderer = impl_->model->GetRenderer<Csm::Rendering::CubismRenderer_OpenGLES2>();
    renderer->SetMvpMatrix(&matrix);
    auto* offscreen = Csm::Rendering::CubismOffscreenManager_OpenGLES2::GetInstance();
    offscreen->BeginFrameProcess();
    if (impl_->bodyModel) {
        // Cloth hidden by the original jaw must also exist when the head
        // turns. This pass contains navy fabric only, never a second neck,
        // and stays behind the head in both standing and seated postures.
        auto* body = impl_->bodyModel->GetModel();
        auto* opacities = const_cast<float*>(Live2D::Cubism::Core::csmGetDrawableOpacities(body->GetModel()));
        const int cloth = impl_->materialBinding.bodyDrawableIndex();
        for (int index = 0; index < body->GetDrawableCount(); ++index)
            opacities[index] = index == cloth ? 1.0f : 0.0f;
        auto* bodyRenderer = impl_->bodyModel->GetRenderer<Csm::Rendering::CubismRenderer_OpenGLES2>();
        bodyRenderer->SetMvpMatrix(&matrix);
        bodyRenderer->BindTexture(impl_->bodyTextureIndex, impl_->bodyUnderpaintTexture);
        bodyRenderer->DrawModel();
        if (impl_->neckModel) {
            // Skin sits above the hidden navy backing and below both visible
            // collars. The independent MOC's own keyforms provide the bend;
            // attach their deltas to the main Native jaw for this draw only.
            auto* neck = impl_->neckModel->GetModel();
            const int surface = impl_->neckBinding.surfaceDrawableIndex();
            auto* vertices = const_cast<Core::csmVector2*>(neck->GetDrawableVertexPositions(surface));
            const auto& positions = impl_->neckBinding.positions();
            const std::vector<Core::csmVector2> saved(vertices, vertices + positions.size());
            std::copy(positions.begin(), positions.end(), vertices);
            auto& colors = neck->GetOverrideMultiplyAndScreenColor();
            const auto& headColors = model->GetOverrideMultiplyAndScreenColor();
            const int face = impl_->neckBinding.faceDrawableIndex();
            const auto multiply = colors.GetDrawableMultiplyColor(surface);
            const auto screen = colors.GetDrawableScreenColor(surface);
            const bool multiplyEnabled = colors.GetDrawableMultiplyColorEnabled(surface);
            const bool screenEnabled = colors.GetDrawableScreenColorEnabled(surface);
            auto* neckOpacities = const_cast<float*>(Core::csmGetDrawableOpacities(neck->GetModel()));
            const float opacity = neckOpacities[surface];
            neckOpacities[surface] = model->GetDrawableOpacity(face);
            colors.SetDrawableMultiplyColor(surface, headColors.GetDrawableMultiplyColor(face));
            colors.SetDrawableScreenColor(surface, headColors.GetDrawableScreenColor(face));
            colors.SetDrawableMultiplyColorEnabled(surface, true);
            colors.SetDrawableScreenColorEnabled(surface, true);
            auto* neckRenderer = impl_->neckModel->GetRenderer<Csm::Rendering::CubismRenderer_OpenGLES2>();
            neckRenderer->SetMvpMatrix(&matrix);
            neckRenderer->DrawModel();
            std::copy(saved.begin(), saved.end(), vertices);
            neckOpacities[surface] = opacity;
            colors.SetDrawableMultiplyColor(surface, multiply);
            colors.SetDrawableScreenColor(surface, screen);
            colors.SetDrawableMultiplyColorEnabled(surface, multiplyEnabled);
            colors.SetDrawableScreenColorEnabled(surface, screenEnabled);
        }
        // The transferred standing collar switches with the standing posture;
        // the seated torso supplies its own visible collar above the padding.
        opacities[cloth] = static_cast<float>(1.0 - seatedMaterial);
        bodyRenderer->BindTexture(impl_->bodyTextureIndex, impl_->bodyTextures[impl_->bodyTextureIndex]);
        // The hidden navy support must still cover the shared jaw boundary.
        // Align only the visible standing collar, then restore the reference
        // vertices before drawing the independently attached neck surface.
        using CorePosition = Live2D::Cubism::Core::csmVector2;
        auto* clothVertices = const_cast<CorePosition*>(
            Live2D::Cubism::Core::csmGetDrawableVertexPositions(body->GetModel())[cloth]);
        std::vector<CorePosition> savedClothVertices;
        if (impl_->hasNeck() && !posture.seatedMaterial && !impl_->authoredSitPose) {
            savedClothVertices.assign(clothVertices, clothVertices + body->GetDrawableVertexCount(cloth));
            if (!alignDrawable(body->GetModel(), cloth, bodyClothTransform)) {
                std::copy(savedClothVertices.begin(), savedClothVertices.end(), clothVertices);
                qWarning() << "Standing collar alignment failed";
                offscreen->EndFrameProcess();
                return;
            }
        }
        bodyRenderer->DrawModel();
        if (!savedClothVertices.empty())
            std::copy(savedClothVertices.begin(), savedClothVertices.end(), clothVertices);
        if (impl_->neckTexture && !impl_->neckModel) {
            // Reuse the Face topology for a skin-only pass. The sparse mesh's
            // affine UV deformation affects only the isolated neck texture;
            // the visible face, eyes and hair keep their original geometry.
            auto* vertices = const_cast<CorePosition*>(
                Live2D::Cubism::Core::csmGetDrawableVertexPositions(body->GetModel())[cloth]);
            const auto& neckPositions = impl_->neckBinding.positions();
            const std::vector<CorePosition> savedVertices(vertices, vertices + neckPositions.size());
            std::copy(neckPositions.begin(), neckPositions.end(), vertices);
            auto& colors = body->GetOverrideMultiplyAndScreenColor();
            const auto savedMultiply = colors.GetDrawableMultiplyColor(cloth);
            const auto savedScreen = colors.GetDrawableScreenColor(cloth);
            const auto multiplyEnabled = colors.GetDrawableMultiplyColorEnabled(cloth);
            const auto screenEnabled = colors.GetDrawableScreenColorEnabled(cloth);
            const auto& headColors = model->GetOverrideMultiplyAndScreenColor();
            colors.SetDrawableMultiplyColor(cloth, headColors.GetDrawableMultiplyColor(cloth));
            colors.SetDrawableScreenColor(cloth, headColors.GetDrawableScreenColor(cloth));
            colors.SetDrawableMultiplyColorEnabled(cloth, true);
            colors.SetDrawableScreenColorEnabled(cloth, true);
            opacities[cloth] = model->GetDrawableOpacity(cloth);
            bodyRenderer->BindTexture(impl_->bodyTextureIndex, impl_->neckTexture);
            bodyRenderer->DrawModel();
            std::copy(savedVertices.begin(), savedVertices.end(), vertices);
            colors.SetDrawableMultiplyColor(cloth, savedMultiply);
            colors.SetDrawableScreenColor(cloth, savedScreen);
            colors.SetDrawableMultiplyColorEnabled(cloth, multiplyEnabled);
            colors.SetDrawableScreenColorEnabled(cloth, screenEnabled);
            // The standing cloth tracks the busy texture mix only: during the
            // visible SitPose descent the material gate is still standing.
            opacities[cloth] = static_cast<float>(1.0 - posture.seatedMix);
            bodyRenderer->BindTexture(impl_->bodyTextureIndex, impl_->bodyTextures[impl_->bodyTextureIndex]);
        }
    }
    renderer->DrawModel();
    offscreen->EndFrameProcess();
}
