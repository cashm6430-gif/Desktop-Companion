#include <GL/glew.h>

#include "CubismCanvas.h"

#include <CubismFramework.hpp>
#include <Id/CubismId.hpp>
#include <Id/CubismIdManager.hpp>
#include <Math/CubismMatrix44.hpp>
#include <Math/CubismModelMatrix.hpp>
#include <Model/CubismModel.hpp>
#include <Model/CubismUserModel.hpp>
#include <Physics/CubismPhysics.hpp>
#include <Rendering/OpenGL/CubismOffscreenManager_OpenGLES2.hpp>
#include <Rendering/OpenGL/CubismRenderer_OpenGLES2.hpp>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>
#include <QSurfaceFormat>
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
}

struct CubismCanvas::Impl {
    CubismAllocator allocator;
    Csm::CubismFramework::Option frameworkOption{};
    std::unique_ptr<PetCubismModel> model;
    std::vector<GLuint> textures;
    QHash<QString, int> parameterIndices;
    std::vector<int> standingFeet;
    std::vector<int> seatedFeet;
    // Every mesh painted for the seated variant. It is registered chin-to-head
    // instead of sole-to-sole, so the soles sit ~64 px above the standing
    // shoes; they are shifted onto the standing floor each frame.
    std::vector<int> seatedMeshes;
    float standingFloor = 0;
    bool frameworkStarted = false;

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
    if (!impl_->textures.empty())
        glDeleteTextures(static_cast<GLsizei>(impl_->textures.size()), impl_->textures.data());
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

    const QString directory = QDir(QCoreApplication::applicationDirPath()).filePath(
        QStringLiteral("assets/live2d/whale-girl"));
    const QString settingsPath = QDir(directory).filePath(QStringLiteral("whale-girl-layered-draft.model3.json"));
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

    const QJsonArray textures = refs.value(QStringLiteral("Textures")).toArray();
    if (textures.isEmpty()) {
        error_ = QStringLiteral("Cubism texture list is empty");
        return;
    }
    for (const QJsonValue& entry : textures) {
        const QImage image = QImage(QDir(directory).filePath(entry.toString()))
                                 .convertToFormat(QImage::Format_RGBA8888_Premultiplied);
        if (image.isNull()) {
            error_ = QStringLiteral("Cubism texture is missing: %1").arg(entry.toString());
            return;
        }
        GLuint texture = 0;
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, image.width(), image.height(), 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, image.constBits());
        glGenerateMipmap(GL_TEXTURE_2D);
        impl_->textures.push_back(texture);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    impl_->model->GetModelMatrix()->SetHeight(1.90f);
    const auto metadata = QJsonDocument::fromJson(readFile(QDir(directory).filePath(
        QStringLiteral("whale-girl-layered-draft.psd2live.json"))));
    for (const auto& layer : metadata.object().value(QStringLiteral("layers")).toArray()) {
        const auto entry = layer.toObject();
        const auto source = entry.value(QStringLiteral("source")).toString();
        const bool seatedMesh = source.startsWith(QStringLiteral("busy "));
        const bool standingFoot = source.startsWith(QStringLiteral("footwear-"));
        if (!seatedMesh && !standingFoot) continue;
        const auto id = entry.value(QStringLiteral("drawable")).toString().toUtf8();
        const int index = model->GetDrawableIndex(Csm::CubismFramework::GetIdManager()->GetId(id.constData()));
        if (index < 0) continue;
        if (seatedMesh) impl_->seatedMeshes.push_back(index);
        if (standingFoot) impl_->standingFeet.push_back(index);
        if (source.startsWith(QStringLiteral("busy leg "))) impl_->seatedFeet.push_back(index);
    }
    model->Update();
    impl_->standingFloor = impl_->footFloor(impl_->standingFeet);
    impl_->model->CreateRenderer(static_cast<Csm::csmUint32>(width() * devicePixelRatioF()),
                                 static_cast<Csm::csmUint32>(height() * devicePixelRatioF()));
    impl_->bindTextures();
    ready_ = true;
    emit readyChanged(true);
}

void CubismCanvas::resizeGL(int width, int height) {
    if (!ready_ || !impl_->model || width <= 0 || height <= 0) return;
    impl_->model->CreateRenderer(static_cast<Csm::csmUint32>(width),
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
    // Cross-fade the two painted bodies instead of swapping them on one frame:
    // the seated art is ~21 px shorter than the crouched standing body, so a
    // hard swap always read as a single snapping frame. ParamBusyLaptop drives
    // the seated drawables' opacity, so fading it in fades the art in too.
    // Smoothstep keeps the half-and-half overlap brief: two stacked silhouettes
    // read as ghosting, so get through the middle quickly.
    const double fade = std::clamp(
        (motion_->values().value(QStringLiteral("ParamBusyLaptop")) - 0.74) / 0.19, 0.0, 1.0);
    const double seatedMix = fade * fade * (3.0 - 2.0 * fade);
    const double sitProgress = std::clamp(motion_->values().value(QStringLiteral("ParamSitPose")) / 0.9, 0.0, 1.0);
    // Finish most of the knee bend before the material changes, including on
    // the first frame of unfolding. This keeps the shared head's height close
    // across the two painted silhouettes while the soles remain grounded.
    const double standingFold = (0.93 * sitProgress * sitProgress * (3.0 - 2.0 * sitProgress))
        * (1.0 - seatedMix) + seatedMix;
    for (auto it = motion_->values().cbegin(); it != motion_->values().cend(); ++it) {
        const auto index = impl_->parameterIndices.constFind(it.key());
        if (index != impl_->parameterIndices.cend())
            model->SetParameterValue(*index, static_cast<float>(it.key() == QStringLiteral("ParamBusyLaptop")
                ? seatedMix
                : (it.key() == QStringLiteral("ParamSitPose") ? standingFold : it.value())));
    }
    const double physicsSeconds = std::exchange(frameSeconds_, 0.0);
    if (!motion_->frozenPhysics() && physicsSeconds > 0)
        impl_->model->evaluatePhysics(static_cast<float>(physicsSeconds));
    // ParamSitPose bends the standing knees. Swap body material
    // near the folded pose without drawing two translucent bodies;
    // the laptop has its own native visibility parameter. Existing grip/grass
    // tracks stay intact; head, hair and tail are shared by both poses.
    model->SetPartOpacity(Csm::CubismFramework::GetIdManager()->GetId("PartBody"),
        static_cast<float>(1.0 - seatedMix));
    model->Update();

    // Folded knees lift the standing shoes off the floor, and the seated art is
    // registered chin-to-head so its soles float higher still. Cancel the knee
    // lift with the model transform, then bring only the seated meshes down onto
    // the same floor. Both silhouettes then keep ground contact all the way
    // through the cross-fade; shifting the whole model instead pulled one of
    // them off the desktop, and dropping the transform let the shoes float.
    const float standingFoot = impl_->footFloor(impl_->standingFeet);
    const float seatedFoot = impl_->footFloor(impl_->seatedFeet);
    Csm::CubismMatrix44 matrix;
    matrix.MultiplyByMatrix(impl_->model->GetModelMatrix());
    if (std::isfinite(standingFoot) && std::isfinite(seatedFoot)
        && std::isfinite(impl_->standingFloor)) {
        auto* modelMatrix = impl_->model->GetModelMatrix();
        const float grounded = modelMatrix->TransformY(impl_->standingFloor)
            - modelMatrix->TransformY(standingFoot);
        matrix.Translate(matrix.GetTranslateX(), matrix.GetTranslateY() + grounded);
        // The SDK exposes the vertex buffer as const, but Update() rebuilds it
        // every frame, so a write here only affects this draw call.
        using Position = std::remove_const_t<std::remove_pointer_t<
            decltype(model->GetDrawableVertexPositions(0))>>;
        const float seatedShift = standingFoot - seatedFoot;
        for (int index : impl_->seatedMeshes) {
            auto* vertices = const_cast<Position*>(model->GetDrawableVertexPositions(index));
            const int count = model->GetDrawableVertexCount(index);
            for (int i = 0; i < count; ++i) vertices[i].Y += seatedShift;
        }
    }
    auto* renderer = impl_->model->GetRenderer<Csm::Rendering::CubismRenderer_OpenGLES2>();
    renderer->SetMvpMatrix(&matrix);
    auto* offscreen = Csm::Rendering::CubismOffscreenManager_OpenGLES2::GetInstance();
    offscreen->BeginFrameProcess();
    renderer->DrawModel();
    offscreen->EndFrameProcess();
}
