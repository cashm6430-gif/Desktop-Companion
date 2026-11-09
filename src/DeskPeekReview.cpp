#include "DeskPeekReview.h"

#include "MotionClip.h"
#include "PetWindow.h"
#include "motion/MotionMath.h"
#include "motion/MotionParameterIds.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <cmath>

namespace {
const QString eyeX = QStringLiteral("ParamEyeBallX");
const QString eyeY = QStringLiteral("ParamEyeBallY");
const QString eyeSmile = QStringLiteral("ParamEyeSmile");
const QString mouthForm = QStringLiteral("ParamMouthForm");

QJsonObject parametersJson(const ParameterMotion::Parameters& values) {
    QJsonObject result;
    for (auto it = values.cbegin(); it != values.cend(); ++it) result.insert(it.key(), it.value());
    return result;
}

// The authored curve represents looking to screen right. Only the face and
// upper-body lean may be layered onto the existing scene. A seated pet keeps
// her entire torso and hand/keyboard pose from the ordinary work player.
void applyPeek(ParameterMotion::Parameters& pose, const MotionClip::Parameters& deltas,
               double side, double weight, bool seated) {
    using namespace motion;
    const auto add = [&](const QString& axis, double scale = 1.0) {
        pose[axis] += deltas.value(axis) * scale * weight;
    };
    add(eyeX, side);
    add(eyeY, seated ? 1.0 : 0.32);
    add(angleX, side * (seated ? 0.65 : 1.0));
    add(angleY, seated ? 1.0 : 0.35);
    add(angleZ, side * (seated ? 0.8 : 1.0));
    if (!seated) {
        add(bodyX, side);
        add(bodyZ, side);
    }
    add(eyeSmile);
    add(mouthForm);
    add(cheek);
    for (const auto& axis : {leftEye, rightEye}) pose[axis] *= 1.0 + deltas.value(axis) * weight;
}
}

int runDeskPeekReview(QApplication& app, PetWindow& window, const QString& output,
                      const QString& scenario, const QString& clipPath) {
    const bool drag = scenario == QStringLiteral("desk-peek-standing-drag");
    const bool remove = scenario == QStringLiteral("desk-peek-laptop-delete");
    const bool seated = remove || scenario == QStringLiteral("desk-peek-laptop-left")
        || scenario == QStringLiteral("desk-peek-laptop-right");
    const bool left = scenario.endsWith(QStringLiteral("-left"));
    if (!drag && !seated && scenario != QStringLiteral("desk-peek-standing-left")
        && scenario != QStringLiteral("desk-peek-standing-right")) return 2;
    if (!app.property("desktopCompanionDeskWorkMode").toBool() || clipPath.isEmpty()) return 2;

    const QDir destination(output);
    MotionClip clip;
    ParameterMotion base;
    base.setDeskWorkMode(true);
    QString error;
    if (!clip.loadJson(clipPath, &error) || clip.approval() != QStringLiteral("pending")
        || clip.isLoop() || !clip.isAdditive() || clip.duration() > 6.0
        || !base.loadMotionLibrary(QDir(app.applicationDirPath()).filePath(QStringLiteral("assets/motions")), &error)) {
        qWarning() << "Desk peek review:" << error;
        return 2;
    }
    base.setProceduralBlink(false); // one authored blink, reproducible for review
    if (seated) base.forceLaptopBusy();
    for (int i = 0; i < 120; ++i) base.advance(1.0 / 30.0);
    if (seated && base.deskWorkPhase() != QStringLiteral("work")) return 2;
    window.setPreviewPose(base.values(), 420);

    constexpr double step = 1.0 / 15.0;
    constexpr int start = 6;
    constexpr int interrupt = 25;
    constexpr double handoff = 0.13;
    const int frames = remove
        ? interrupt + static_cast<int>(std::ceil((base.actionDuration(PetController::State::Delete) + 1.2) / step))
        : start + static_cast<int>(std::ceil((clip.duration() + 0.65) / step));
    const bool interrupted = drag || remove;
    const double side = left ? -1.0 : 1.0;
    int frame = 0;
    QJsonArray trace;
    QTimer timer;
    timer.setInterval(50);
    QObject::connect(&timer, &QTimer::timeout, &app, [&] {
        QString event;
        if (frame == start) event = QStringLiteral("peek");
        if (interrupted && frame == interrupt) {
            if (drag) {
                base.beginDragMotion();
                base.updateDragMotion(110.0, 0.0);
                event = QStringLiteral("drag_begin");
            } else {
                base.setState(PetController::State::Delete);
                event = QStringLiteral("delete_begin");
            }
        }
        if (drag && frame == interrupt + 8) {
            base.endDragMotion();
            event = QStringLiteral("drag_end");
        }
        if (remove && base.consumeActionFinished()) {
            base.setState(PetController::State::Busy);
            event = QStringLiteral("resume_busy");
        }
        const double local = (frame - start) * step;
        const bool yielding = interrupted && frame >= interrupt;
        // Hold the last rendered offset as it fades away; no new peek keys
        // can run after the handoff, even when the foreground action finishes.
        const double sampled = yielding ? (interrupt - start - 1) * step : local;
        const double weight = local < 0.0 || local >= clip.duration() ? 0.0
            : yielding ? 1.0 - motion::smooth((frame - interrupt + 1) * step / handoff) : 1.0;
        base.advance(step);
        auto pose = base.values();
        if (weight > 0.0) applyPeek(pose, clip.sample(sampled), side, weight, seated);
        const QString file = QStringLiteral("frame-%1.png").arg(frame, 3, 10, QLatin1Char('0'));
        if (window.renderBackend() != QStringLiteral("cubism_native")
            || !window.renderSequenceFrame(pose, step, destination.filePath(file), base.bubblePulse())) {
            app.exit(1);
            return;
        }
        trace.append(QJsonObject{{QStringLiteral("time"), frame * step}, {QStringLiteral("file"), file},
            {QStringLiteral("event"), event}, {QStringLiteral("weight"), weight},
            {QStringLiteral("desk_phase"), base.deskWorkPhase()},
            {QStringLiteral("base"), parametersJson(base.values())},
            {QStringLiteral("applied"), parametersJson(pose)}});
        if (++frame < frames) return;
        QFile receipt(destination.filePath(QStringLiteral("scene.json")));
        const QJsonDocument report(QJsonObject{{QStringLiteral("scenario"), scenario},
            {QStringLiteral("approval"), clip.approval()}, {QStringLiteral("revision"), clip.revision()},
            {QStringLiteral("render_backend"), window.renderBackend()}, {QStringLiteral("render_error"), window.renderError()},
            {QStringLiteral("isolated_settings"), app.property("desktopCompanionReviewSettingsIsolated").toBool()},
            {QStringLiteral("scope"), QStringLiteral("candidate_layer_on_real_motion_player_native_render")},
            {QStringLiteral("start_frame"), start}, {QStringLiteral("step"), step},
            {QStringLiteral("frames"), trace}});
        app.exit(receipt.open(QIODevice::WriteOnly) && receipt.write(report.toJson()) > 0 ? 0 : 1);
    });
    QTimer::singleShot(1000, &timer, [&] { timer.start(); });
    return app.exec();
}
