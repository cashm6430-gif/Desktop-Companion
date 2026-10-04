#include "CubismMaterialBinding.h"

#include <QHash>
#include <QJsonArray>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {
bool identicalIds(const char* const* left, const char* const* right, int count) {
    if (count && (!left || !right)) return false;
    for (int i = 0; i < count; ++i) {
        if (!left[i] || !right[i] || std::strcmp(left[i], right[i]) != 0) return false;
    }
    return true;
}
} // namespace

bool CubismMaterialBinding::configure(Core::csmModel* visible, Core::csmModel* bodyModel,
                                      const QJsonObject& metadata, QString* error) {
    // Failed reconfiguration must not keep a previous pair of pointers active.
    visible_ = bodyModel_ = nullptr;
    parameterCount_ = partCount_ = 0;
    bodyDrawableIndex_ = -1;
    headParameterIndices_.clear();
    if (error) error->clear();
    const auto fail = [error](const QString& reason) {
        if (error) *error = reason;
        return false;
    };
    if (!visible || !bodyModel || visible == bodyModel) {
        return fail(QStringLiteral("Material separation requires two independent Core models."));
    }

    const int parameters = Core::csmGetParameterCount(visible);
    const int parts = Core::csmGetPartCount(visible);
    const int drawables = Core::csmGetDrawableCount(visible);
    if (parameters != Core::csmGetParameterCount(bodyModel)
        || parts != Core::csmGetPartCount(bodyModel)
        || drawables != Core::csmGetDrawableCount(bodyModel)
        || !identicalIds(Core::csmGetParameterIds(visible), Core::csmGetParameterIds(bodyModel), parameters)
        || !identicalIds(Core::csmGetPartIds(visible), Core::csmGetPartIds(bodyModel), parts)
        || !identicalIds(Core::csmGetDrawableIds(visible), Core::csmGetDrawableIds(bodyModel), drawables)) {
        return fail(QStringLiteral("Material separation model counts or IDs do not match."));
    }
    const auto* counts = Core::csmGetDrawableVertexCounts(visible);
    const auto* bodyCounts = Core::csmGetDrawableVertexCounts(bodyModel);
    const auto* uvs = Core::csmGetDrawableVertexUvs(visible);
    const auto* bodyUvs = Core::csmGetDrawableVertexUvs(bodyModel);
    const auto* indexCounts = Core::csmGetDrawableIndexCounts(visible);
    const auto* bodyIndexCounts = Core::csmGetDrawableIndexCounts(bodyModel);
    const auto* indices = Core::csmGetDrawableIndices(visible);
    const auto* bodyIndices = Core::csmGetDrawableIndices(bodyModel);
    for (int mesh = 0; mesh < drawables; ++mesh) {
        if (counts[mesh] != bodyCounts[mesh] || indexCounts[mesh] != bodyIndexCounts[mesh]) {
            return fail(QStringLiteral("Material separation drawable topology does not match."));
        }
        for (int vertex = 0; vertex < counts[mesh]; ++vertex) {
            const auto& uv = uvs[mesh][vertex];
            const auto& bodyUv = bodyUvs[mesh][vertex];
            if (!std::isfinite(uv.X) || !std::isfinite(uv.Y)
                || uv.X != bodyUv.X || uv.Y != bodyUv.Y) {
                return fail(QStringLiteral("Material separation drawable UVs do not match."));
            }
        }
        for (int index = 0; index < indexCounts[mesh]; ++index) {
            if (indices[mesh][index] != bodyIndices[mesh][index]) {
                return fail(QStringLiteral("Material separation drawable indices do not match."));
            }
        }
    }

    const auto declarationValue = metadata.value(QStringLiteral("runtimeMaterialSeparation"));
    if (!declarationValue.isObject()) {
        return fail(QStringLiteral("runtimeMaterialSeparation must be an object."));
    }
    const auto declaration = declarationValue.toObject();
    const QString source = declaration.value(QStringLiteral("bodySource")).toString();
    int selectedMesh = -1;
    int matchingLayers = 0;
    const auto* drawableIds = Core::csmGetDrawableIds(visible);
    for (const auto& value : metadata.value(QStringLiteral("layers")).toArray()) {
        const auto layer = value.toObject();
        if (layer.value(QStringLiteral("source")).toString() != source) continue;
        ++matchingLayers;
        const auto drawable = layer.value(QStringLiteral("drawable")).toString();
        for (int mesh = 0; mesh < drawables; ++mesh) {
            if (QString::fromUtf8(drawableIds[mesh]) == drawable) selectedMesh = mesh;
        }
    }
    if (source.isEmpty() || matchingLayers != 1 || selectedMesh < 0 || counts[selectedMesh] <= 0) {
        return fail(QStringLiteral("Material separation source %1 cannot resolve a unique drawable.").arg(source));
    }

    const auto headIdsValue = declaration.value(QStringLiteral("headOnlyParameters"));
    if (!headIdsValue.isArray() || headIdsValue.toArray().isEmpty()) {
        return fail(QStringLiteral("headOnlyParameters must be a nonempty array."));
    }
    QHash<QString, int> parameterIndices;
    const auto* parameterIds = Core::csmGetParameterIds(visible);
    for (int parameter = 0; parameter < parameters; ++parameter) {
        parameterIndices.insert(QString::fromUtf8(parameterIds[parameter]), parameter);
    }
    QSet<QString> seenHeadIds;
    QVector<int> headIndices;
    for (const auto& value : headIdsValue.toArray()) {
        const QString id = value.toString();
        if (!value.isString() || id.isEmpty() || seenHeadIds.contains(id)
            || !parameterIndices.contains(id)) {
            return fail(QStringLiteral("Head-only parameter %1 is missing, invalid or duplicated.").arg(id));
        }
        seenHeadIds.insert(id);
        headIndices.append(parameterIndices.value(id));
    }

    visible_ = visible;
    bodyModel_ = bodyModel;
    parameterCount_ = parameters;
    partCount_ = parts;
    bodyDrawableIndex_ = selectedMesh;
    headParameterIndices_ = headIndices;
    return true;
}

void CubismMaterialBinding::apply() {
    if (!visible_ || !bodyModel_) return;
    auto* bodyParameters = Core::csmGetParameterValues(bodyModel_);
    std::copy_n(Core::csmGetParameterValues(visible_), parameterCount_, bodyParameters);
    if (partCount_) {
        std::copy_n(Core::csmGetPartOpacities(visible_), partCount_, Core::csmGetPartOpacities(bodyModel_));
    }
    for (const int index : headParameterIndices_) bodyParameters[index] = 0.0f;
    Core::csmUpdateModel(bodyModel_);
}
