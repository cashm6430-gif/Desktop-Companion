#include "CubismNeckBinding.h"

#include <QJsonArray>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace {
bool point(const QJsonValue& value, std::array<double, 2>* result) {
    if (!value.isArray() || value.toArray().size() != 2) return false;
    const auto values = value.toArray();
    for (int axis = 0; axis < 2; ++axis) {
        if (!values[axis].isDouble() || !std::isfinite(values[axis].toDouble())) return false;
        (*result)[axis] = values[axis].toDouble();
    }
    return true;
}

bool finite(const Core::csmVector2& value) {
    return std::isfinite(value.X) && std::isfinite(value.Y);
}
} // namespace

void CubismNeckBinding::reset() {
    model_ = nullptr;
    faceDrawableIndex_ = -1;
    head_ = standing_ = seated_ = Anchor{};
    stretchWeights_.clear();
    positions_.clear();
    atlasSize_ = headOffset_ = {};
    surface_ = nullptr;
    surfaceDrawableIndex_ = -1;
    surfaceParameters_ = {{-1, -1, -1}};
    sourcePixelsPerUnit_ = 0;
    surfaceHeadSamples_.clear();
    surfaceNeutral_.clear();
}

bool CubismNeckBinding::configure(Core::csmModel* model, const QJsonObject& metadata,
                                const QJsonObject& ownership, QString* error) {
    reset();
    if (error) error->clear();
    const auto fail = [error](const QString& reason) {
        if (error) *error = reason;
        return false;
    };
    if (!model) return fail(QStringLiteral("Neck connection requires a Native model."));
    const auto declarationValue = metadata.value(QStringLiteral("runtimeNeckConnection"));
    if (!declarationValue.isObject()) {
        return fail(QStringLiteral("runtimeNeckConnection must be an object."));
    }
    const auto declaration = declarationValue.toObject();
    std::array<double, 2> atlasSize{};
    if (!point(ownership.value(QStringLiteral("atlas_size")), &atlasSize)
        || atlasSize[0] <= 0 || atlasSize[1] <= 0
        || std::floor(atlasSize[0]) != atlasSize[0] || std::floor(atlasSize[1]) != atlasSize[1]) {
        return fail(QStringLiteral("Neck atlas dimensions are invalid."));
    }
    const auto chartsValue = ownership.value(QStringLiteral("source_charts"));
    if (!chartsValue.isObject()) return fail(QStringLiteral("Neck source charts are missing."));
    const auto charts = chartsValue.toObject();
    const auto layers = metadata.value(QStringLiteral("layers")).toArray();
    const int count = Core::csmGetDrawableCount(model);
    const auto* ids = Core::csmGetDrawableIds(model);
    const auto* vertexCounts = Core::csmGetDrawableVertexCounts(model);
    const auto* uvs = Core::csmGetDrawableVertexUvs(model);
    const auto* indexCounts = Core::csmGetDrawableIndexCounts(model);
    const auto* indices = Core::csmGetDrawableIndices(model);

    const auto resolve = [&](const QString& source, const QJsonValue& anchorValue,
                             Anchor* anchor, std::array<double, 2>* offset,
                             std::array<double, 2>* sourcePoint) {
        if (source.isEmpty() || !point(anchorValue, sourcePoint)) return false;
        QString drawable;
        int sourceMatches = 0;
        for (const auto& value : layers) {
            const auto layer = value.toObject();
            if (layer.value(QStringLiteral("source")).toString() == source) {
                ++sourceMatches;
                drawable = layer.value(QStringLiteral("drawable")).toString();
            }
        }
        if (sourceMatches != 1 || drawable.isEmpty() || !charts.value(source).isObject()) return false;
        const auto chart = charts.value(source).toObject();
        if (chart.value(QStringLiteral("drawable")).toString() != drawable
            || !point(chart.value(QStringLiteral("offset")), offset)) return false;
        const auto bounds = chart.value(QStringLiteral("atlas")).toArray();
        if (bounds.size() != 4) return false;
        std::array<double, 4> box{};
        for (int i = 0; i < 4; ++i) {
            if (!bounds[i].isDouble() || !std::isfinite(bounds[i].toDouble())) return false;
            box[i] = bounds[i].toDouble();
        }
        if (box[0] < 0 || box[1] < 0 || box[2] > atlasSize[0] || box[3] > atlasSize[1]
            || box[2] <= box[0] || box[3] <= box[1]) return false;
        const double atlasX = (*sourcePoint)[0] - (*offset)[0];
        const double atlasY = (*sourcePoint)[1] - (*offset)[1];
        if (atlasX < box[0] || atlasX > box[2] || atlasY < box[1] || atlasY > box[3]) return false;
        const std::array<double, 2> target{atlasX / atlasSize[0], 1.0 - atlasY / atlasSize[1]};
        int meshMatches = 0;
        for (int mesh = 0; mesh < count; ++mesh) {
            if (QString::fromUtf8(ids[mesh]) == drawable) {
                ++meshMatches;
                anchor->drawable = mesh;
            }
        }
        if (meshMatches != 1) return false;
        const int mesh = anchor->drawable;
        if (vertexCounts[mesh] <= 0 || indexCounts[mesh] <= 0 || indexCounts[mesh] % 3 != 0) return false;
        for (int vertex = 0; vertex < vertexCounts[mesh]; ++vertex) {
            if (!finite(uvs[mesh][vertex])) return false;
        }
        bool found = false;
        for (int i = 0; i < indexCounts[mesh]; i += 3) {
            const std::array<int, 3> vertices{indices[mesh][i], indices[mesh][i + 1], indices[mesh][i + 2]};
            if (std::any_of(vertices.begin(), vertices.end(), [&](int v) { return v >= vertexCounts[mesh]; })) {
                return false;
            }
            const auto& a = uvs[mesh][vertices[0]];
            const auto& b = uvs[mesh][vertices[1]];
            const auto& c = uvs[mesh][vertices[2]];
            const double determinant = (double(b.Y) - c.Y) * (double(a.X) - c.X)
                + (double(c.X) - b.X) * (double(a.Y) - c.Y);
            if (std::abs(determinant) < 1e-14) continue;
            const double w0 = ((double(b.Y) - c.Y) * (target[0] - c.X)
                + (double(c.X) - b.X) * (target[1] - c.Y)) / determinant;
            const double w1 = ((double(c.Y) - a.Y) * (target[0] - c.X)
                + (double(a.X) - c.X) * (target[1] - c.Y)) / determinant;
            const double w2 = 1.0 - w0 - w1;
            if (!found && std::min({w0, w1, w2}) >= -1e-8 && std::max({w0, w1, w2}) <= 1.0 + 1e-8) {
                anchor->vertices = vertices;
                anchor->weights = {w0, w1, w2};
                found = true;
            }
        }
        return found;
    };

    Anchor head, standing, seated;
    std::array<double, 2> headOffset{}, standingOffset{}, seatedOffset{};
    std::array<double, 2> headPoint{}, standingPoint{}, seatedPoint{};
    if (!resolve(declaration.value(QStringLiteral("headSource")).toString(),
            declaration.value(QStringLiteral("headAnchor")), &head, &headOffset, &headPoint)
        || !resolve(declaration.value(QStringLiteral("standingSource")).toString(),
            declaration.value(QStringLiteral("standingAnchor")), &standing, &standingOffset, &standingPoint)
        || !resolve(declaration.value(QStringLiteral("seatedSource")).toString(),
            declaration.value(QStringLiteral("seatedAnchor")), &seated, &seatedOffset, &seatedPoint)) {
        return fail(QStringLiteral("Neck anchors must resolve unique source charts and Native UV triangles."));
    }
    if (head.drawable == standing.drawable || head.drawable == seated.drawable || standing.drawable == seated.drawable) {
        return fail(QStringLiteral("Neck head and collar surfaces must be distinct drawables."));
    }
    const auto stretchStartValue = declaration.value(QStringLiteral("stretchStartY"));
    const double stretchStart = stretchStartValue.toDouble(std::numeric_limits<double>::quiet_NaN());
    if (!stretchStartValue.isDouble() || !std::isfinite(stretchStart) || headPoint[1] <= stretchStart) {
        return fail(QStringLiteral("Neck stretch start must precede its skin tip."));
    }
    std::vector<double> weights(vertexCounts[head.drawable]);
    for (int vertex = 0; vertex < vertexCounts[head.drawable]; ++vertex) {
        const double sourceY = atlasSize[1] * (1.0 - uvs[head.drawable][vertex].Y) + headOffset[1];
        // Keep this affine across each original UV triangle. Clamping the
        // vertices of this sparse Face mesh would also move the jaw seam.
        weights[vertex] = (sourceY - stretchStart) / (headPoint[1] - stretchStart);
        if (!std::isfinite(weights[vertex])) return fail(QStringLiteral("Neck stretch weights are invalid."));
    }
    model_ = model;
    faceDrawableIndex_ = head.drawable;
    head_ = head;
    standing_ = standing;
    seated_ = seated;
    stretchWeights_ = std::move(weights);
    atlasSize_ = atlasSize;
    headOffset_ = headOffset;
    return true;
}

bool CubismNeckBinding::configureSurface(Core::csmModel* surface, const QJsonObject& rig,
                                       QString* error) {
    if (error) error->clear();
    const auto fail = [error](const QString& reason) {
        if (error) *error = reason;
        return false;
    };
    if (!model_ || !surface || surface == model_ || surface_)
        return fail(QStringLiteral("An independent neck requires two configured Native models."));
    if (rig.value(QStringLiteral("version")).toInt() != 1)
        return fail(QStringLiteral("Independent neck rig version is unsupported."));
    const QString drawable = rig.value(QStringLiteral("drawable")).toString();
    int surfaceMesh = -1;
    const auto* surfaceIds = Core::csmGetDrawableIds(surface);
    const int surfaceCount = Core::csmGetDrawableCount(surface);
    if (surfaceCount != 1 || drawable.isEmpty())
        return fail(QStringLiteral("The neck component must contain exactly one declared drawable."));
    if (QString::fromUtf8(surfaceIds[0]) == drawable) surfaceMesh = 0;
    if (surfaceMesh < 0) return fail(QStringLiteral("Independent neck drawable does not match its rig."));
    const auto points = rig.value(QStringLiteral("vertexSourcePoints")).toArray();
    const int surfaceVertices = Core::csmGetDrawableVertexCounts(surface)[surfaceMesh];
    if (surfaceVertices < 3 || points.size() != surfaceVertices)
        return fail(QStringLiteral("Independent neck source points must match the complete Native topology."));
    std::array<int, 3> parameterIndices{{-1, -1, -1}};
    const auto parameterNames = rig.value(QStringLiteral("parameters")).toObject();
    const std::array<QString, 3> roles{QStringLiteral("bottomX"), QStringLiteral("bottomY"), QStringLiteral("curve")};
    const auto* parameterIds = Core::csmGetParameterIds(surface);
    const auto* defaults = Core::csmGetParameterDefaultValues(surface);
    for (int role = 0; role < 3; ++role) {
        const auto name = parameterNames.value(roles[role]).toString();
        if (name.isEmpty()) return fail(QStringLiteral("Independent neck parameter roles are incomplete."));
        for (int parameter = 0; parameter < Core::csmGetParameterCount(surface); ++parameter) {
            if (QString::fromUtf8(parameterIds[parameter]) == name) parameterIndices[role] = parameter;
        }
        if (parameterIndices[role] < 0 || defaults[parameterIndices[role]] != 0.0f)
            return fail(QStringLiteral("Independent neck axes require Native parameters with neutral zero."));
    }
    if (parameterIndices[0] == parameterIndices[1] || parameterIndices[0] == parameterIndices[2]
        || parameterIndices[1] == parameterIndices[2])
        return fail(QStringLiteral("Independent neck axes must be distinct."));
    Core::csmVector2 canvas{}, origin{};
    float pixelsPerUnit = 0;
    Core::csmReadCanvasInfo(model_, &canvas, &origin, &pixelsPerUnit);
    if (!std::isfinite(pixelsPerUnit) || pixelsPerUnit <= 0)
        return fail(QStringLiteral("Main model source scale is invalid."));
    Core::csmVector2 surfaceCanvas{}, surfaceOrigin{};
    float surfacePixelsPerUnit = 0;
    Core::csmReadCanvasInfo(surface, &surfaceCanvas, &surfaceOrigin, &surfacePixelsPerUnit);
    std::array<double, 2> declaredCanvas{};
    if (surfacePixelsPerUnit != pixelsPerUnit
        || rig.value(QStringLiteral("pixelsPerUnit")).toDouble() != pixelsPerUnit
        || !point(rig.value(QStringLiteral("sourceCanvasSize")), &declaredCanvas)
        || declaredCanvas[0] != canvas.X || declaredCanvas[1] != canvas.Y
        || surfaceCanvas.X != canvas.X || surfaceCanvas.Y != canvas.Y
        || surfaceOrigin.X != origin.X || surfaceOrigin.Y != origin.Y)
        return fail(QStringLiteral("Independent neck and main model must use the same source canvas and unit scale."));

    const auto* uv = Core::csmGetDrawableVertexUvs(model_)[faceDrawableIndex_];
    const auto* indices = Core::csmGetDrawableIndices(model_)[faceDrawableIndex_];
    const int indexCount = Core::csmGetDrawableIndexCounts(model_)[faceDrawableIndex_];
    std::vector<Anchor> samples;
    for (const auto& value : points) {
        std::array<double, 2> source{};
        if (!point(value, &source)) return fail(QStringLiteral("Neck source points must be finite pairs."));
        const double u = (source[0] - headOffset_[0]) / atlasSize_[0];
        const double v = 1.0 - (source[1] - headOffset_[1]) / atlasSize_[1];
        Anchor anchor;
        anchor.drawable = faceDrawableIndex_;
        bool found = false;
        for (int i = 0; i + 2 < indexCount && !found; i += 3) {
            const auto& a = uv[indices[i]];
            const auto& b = uv[indices[i + 1]];
            const auto& c = uv[indices[i + 2]];
            const double determinant = (double(b.Y) - c.Y) * (double(a.X) - c.X)
                + (double(c.X) - b.X) * (double(a.Y) - c.Y);
            if (std::abs(determinant) < 1e-14) continue;
            const double w0 = ((double(b.Y) - c.Y) * (u - c.X) + (double(c.X) - b.X) * (v - c.Y)) / determinant;
            const double w1 = ((double(c.Y) - a.Y) * (u - c.X) + (double(a.X) - c.X) * (v - c.Y)) / determinant;
            const double w2 = 1.0 - w0 - w1;
            if (std::min({w0, w1, w2}) < -1e-8 || std::max({w0, w1, w2}) > 1.0 + 1e-8) continue;
            anchor.vertices = {indices[i], indices[i + 1], indices[i + 2]};
            anchor.weights = {w0, w1, w2};
            samples.push_back(anchor);
            found = true;
        }
        if (!found) return fail(QStringLiteral("Independent neck vertex %1 at (%2, %3) is outside the original Face UV mesh.")
            .arg(samples.size()).arg(source[0]).arg(source[1]));
    }
    auto* values = Core::csmGetParameterValues(surface);
    for (const auto parameter : parameterIndices) values[parameter] = 0;
    Core::csmUpdateModel(surface);
    const auto* neutral = Core::csmGetDrawableVertexPositions(surface)[surfaceMesh];
    std::vector<Core::csmVector2> neutralPositions(neutral, neutral + surfaceVertices);
    if (std::any_of(neutralPositions.begin(), neutralPositions.end(), [](const auto& p) { return !finite(p); }))
        return fail(QStringLiteral("Independent neck neutral geometry is invalid."));
    surface_ = surface;
    surfaceDrawableIndex_ = surfaceMesh;
    surfaceParameters_ = parameterIndices;
    sourcePixelsPerUnit_ = pixelsPerUnit;
    surfaceHeadSamples_ = std::move(samples);
    surfaceNeutral_ = std::move(neutralPositions);
    return true;
}

bool CubismNeckBinding::sample(const Anchor& anchor, std::array<double, 2>* result) const {
    *result = {};
    const auto* positions = Core::csmGetDrawableVertexPositions(model_)[anchor.drawable];
    for (int i = 0; i < 3; ++i) {
        const auto& position = positions[anchor.vertices[i]];
        if (!finite(position)) return false;
        (*result)[0] += anchor.weights[i] * position.X;
        (*result)[1] += anchor.weights[i] * position.Y;
    }
    return true;
}

bool CubismNeckBinding::collarPositions(std::array<double, 2>* standing,
                                      std::array<double, 2>* seated, QString* error) const {
    if (error) error->clear();
    if (!model_ || !standing || !seated || standing == seated) {
        if (error) *error = QStringLiteral("Collar positions require a configured neck and two outputs.");
        return false;
    }
    std::array<double, 2> standingResult{}, seatedResult{};
    if (!sample(standing_, &standingResult) || !sample(seated_, &seatedResult)) {
        if (error) *error = QStringLiteral("Collar anchor geometry is not finite.");
        return false;
    }
    *standing = standingResult;
    *seated = seatedResult;
    return true;
}

bool CubismNeckBinding::update(double seatedMix, QString* error) {
    positions_.clear();
    if (error) error->clear();
    const auto fail = [error](const QString& reason) {
        if (error) *error = reason;
        return false;
    };
    if (!model_) return fail(QStringLiteral("Neck connection is not configured."));
    if (!std::isfinite(seatedMix) || seatedMix < 0 || seatedMix > 1) {
        return fail(QStringLiteral("Neck seated blend must be between zero and one."));
    }
    const auto* allPositions = Core::csmGetDrawableVertexPositions(model_);
    std::array<double, 2> head{}, standing{}, seated{};
    if (!sample(head_, &head) || !sample(standing_, &standing) || !sample(seated_, &seated)) {
        return fail(QStringLiteral("Neck anchor geometry is not finite."));
    }
    const std::array<double, 2> delta{
        standing[0] + seatedMix * (seated[0] - standing[0]) - head[0],
        standing[1] + seatedMix * (seated[1] - standing[1]) - head[1]};
    if (surface_) {
        const std::array<float, 3> requested{
            static_cast<float>(delta[0] * sourcePixelsPerUnit_),
            static_cast<float>(delta[1] * sourcePixelsPerUnit_),
            static_cast<float>(std::clamp(delta[0] * sourcePixelsPerUnit_ / 24.0, -1.0, 1.0))};
        auto* values = Core::csmGetParameterValues(surface_);
        const auto* minimum = Core::csmGetParameterMinimumValues(surface_);
        const auto* maximum = Core::csmGetParameterMaximumValues(surface_);
        for (int role = 0; role < 3; ++role) {
            const int parameter = surfaceParameters_[role];
            if (!std::isfinite(requested[role]) || requested[role] < minimum[parameter] || requested[role] > maximum[parameter])
                return fail(QStringLiteral("Neck attachment exceeds the independent Native parameter range."));
            values[parameter] = requested[role];
        }
        Core::csmUpdateModel(surface_);
        const auto* deformed = Core::csmGetDrawableVertexPositions(surface_)[surfaceDrawableIndex_];
        std::vector<Core::csmVector2> result(surfaceHeadSamples_.size());
        for (std::size_t vertex = 0; vertex < result.size(); ++vertex) {
            std::array<double, 2> attached{};
            if (!sample(surfaceHeadSamples_[vertex], &attached) || !finite(deformed[vertex]))
                return fail(QStringLiteral("Independent neck deformation is not finite."));
            result[vertex] = {
                static_cast<float>(attached[0] + deformed[vertex].X - surfaceNeutral_[vertex].X),
                static_cast<float>(attached[1] + deformed[vertex].Y - surfaceNeutral_[vertex].Y)};
            if (!finite(result[vertex])) return fail(QStringLiteral("Independent neck attachment is not finite."));
        }
        positions_ = std::move(result);
        return true;
    }
    std::vector<Core::csmVector2> result(stretchWeights_.size());
    for (std::size_t i = 0; i < result.size(); ++i) {
        const auto& position = allPositions[faceDrawableIndex_][i];
        if (!finite(position)) return fail(QStringLiteral("Neck source geometry is not finite."));
        result[i].X = static_cast<float>(position.X + stretchWeights_[i] * delta[0]);
        result[i].Y = static_cast<float>(position.Y + stretchWeights_[i] * delta[1]);
        if (!finite(result[i])) return fail(QStringLiteral("Neck generated geometry is not finite."));
    }
    positions_ = std::move(result);
    return true;
}
