#pragma once

#include <Live2DCubismCore.hpp>

#include <QJsonObject>
#include <QString>
#include <array>
#include <vector>

namespace Core = Live2D::Cubism::Core;

// Builds a neck-only render surface. The main Native model remains untouched;
// its resolved head and grounded collar geometry provide the two endpoints.
class CubismNeckBinding final {
public:
    bool configure(Core::csmModel* model, const QJsonObject& metadata,
                   const QJsonObject& ownership, QString* error = nullptr);

    // An independently authored MOC supplies dense topology and parameter
    // deformation. Source points bind its neutral vertices to the live jaw.
    bool configureSurface(Core::csmModel* surface, const QJsonObject& rig,
                          QString* error = nullptr);

    // Call after Core Update and the seated meshes' floor correction. The
    // renderer may temporarily use these positions with its neck-only texture.
    bool update(double seatedMix, QString* error = nullptr);

    // Resolved after grounding, before a body alignment is applied. Reading
    // these shared collar anchors lets the material transition meet the same
    // attachment points used by the neck surface.
    bool collarPositions(std::array<double, 2>* standing,
                         std::array<double, 2>* seated, QString* error = nullptr) const;

    int faceDrawableIndex() const { return faceDrawableIndex_; }
    int surfaceDrawableIndex() const { return surfaceDrawableIndex_; }
    const std::vector<Core::csmVector2>& positions() const { return positions_; }

private:
    struct Anchor {
        int drawable = -1;
        std::array<int, 3> vertices{};
        std::array<double, 3> weights{};
    };

    void reset();
    bool sample(const Anchor& anchor, std::array<double, 2>* result) const;

    Core::csmModel* model_ = nullptr;
    int faceDrawableIndex_ = -1;
    Anchor head_, standing_, seated_;
    std::vector<double> stretchWeights_;
    std::vector<Core::csmVector2> positions_;
    std::array<double, 2> atlasSize_{}, headOffset_{};
    Core::csmModel* surface_ = nullptr;
    int surfaceDrawableIndex_ = -1;
    std::array<int, 3> surfaceParameters_{{-1, -1, -1}};
    float sourcePixelsPerUnit_ = 0;
    std::vector<Anchor> surfaceHeadSamples_;
    std::vector<Core::csmVector2> surfaceNeutral_;
};
