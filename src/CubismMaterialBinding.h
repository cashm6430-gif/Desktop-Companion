#pragma once

#include <Live2DCubismCore.hpp>

#include <QJsonObject>
#include <QString>
#include <QVector>

namespace Core = Live2D::Cubism::Core;

// Evaluates clothing from a second Native model while the visible model keeps
// its complete head and neck deformation. Both instances must outlive this
// object. The renderer separates their pixels with complementary textures.
class CubismMaterialBinding final {
public:
    bool configure(Core::csmModel* visible, Core::csmModel* bodyModel,
                   const QJsonObject& metadata, QString* error = nullptr);

    // Call after updating the visible model. Copies its resolved pose and part
    // opacities to the body model, then removes only declared head influences.
    // Never changes the visible model's state or geometry.
    void apply();

    int bodyDrawableIndex() const { return bodyDrawableIndex_; }

private:
    Core::csmModel* visible_ = nullptr;
    Core::csmModel* bodyModel_ = nullptr;
    int parameterCount_ = 0;
    int partCount_ = 0;
    int bodyDrawableIndex_ = -1;
    QVector<int> headParameterIndices_;
};
