#include "EdgeHideReview.h"
#include "EdgeHideLayout.h"
#include "PetController.h"
#include "PetWindow.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QSaveFile>
#include <QTimer>
#include <QWidget>
#include <algorithm>
#include <cmath>
#include <functional>

namespace {
class PreviewSurface final : public QWidget {
public:
    PreviewSurface(edge_dock::Edge edge, bool continuous) : edge_(edge), continuous_(continuous) {
        setWindowFlags(Qt::Tool | Qt::FramelessWindowHint);
        setFixedSize(560, 560);
        setMouseTracking(true);
        start_ = {180, 180};
    }
    edge_hide::Layout pose() const {
        auto result = continuous_ ? edge_hide::continuousLayout(edge_, screen_, 200, start_, progress_)
                                  : edge_hide::layout(edge_, screen_, 200, start_, progress_);
        if (pressed_ && hiddenPress_) {
            // While captured, the exact point pressed on the head stays under
            // the pointer as the model turns back toward upright.
            result.angle = pressedAngle_ * edge_hide::ease(progress_);
            result.modelToScreen = QTransform();
            result.modelToScreen.translate(cursor_.x(), cursor_.y());
            result.modelToScreen.rotate(result.angle);
            result.modelToScreen.translate(-headAnchor_.x(), -headAnchor_.y());
        }
        return result;
    }
    void setFrame(const QImage& image) { frame_ = image; update(); }
    void setProgress(double value) { progress_ = std::clamp(value, 0.0, 1.0); update(); }
    double progress() const { return progress_; }
    bool pressed() const { return pressed_; }
    bool returned() const { return returned_; }
    bool entered() const { return entered_; }
    bool fullWindowInside() const { return screen_.contains(QRectF(start_, QSizeF(200, 200))); }
    bool cursorSeen() const { return cursorSeen_; }
    QPointF cursor() const { return cursor_; }
    QPointF facePoint() const { return pose().modelToScreen.map(QPointF(100, 80)); }
    QPointF grabbedPoint() const { return pose().modelToScreen.map(headAnchor_); }
    QPointF inward() const {
        switch (edge_) {
        case edge_dock::Edge::Left: return {1, 0};
        case edge_dock::Edge::Right: return {-1, 0};
        case edge_dock::Edge::Top: return {0, 1};
        case edge_dock::Edge::Bottom: return {0, -1};
        default: return {};
        }
    }
    QPointF outward() const { return -inward(); }
protected:
    bool event(QEvent* event) override {
        if (pressed_ && (event->type() == QEvent::UngrabMouse || event->type() == QEvent::WindowDeactivate)) {
            const bool wasHidden = hiddenPress_;
            pressed_ = false;
            if (wasHidden) setProgress(1.0);
            else { start_ = startAtPress_; setProgress(0.0); }
        }
        return QWidget::event(event);
    }
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), QColor("#384659"));
        painter.fillRect(screen_, QColor("#eaf0f5"));
        painter.save();
        painter.setClipRect(screen_);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.setTransform(pose().modelToScreen, true);
        if (!frame_.isNull()) painter.drawImage(QRectF(0, 0, 200, 200), frame_);
        painter.restore();
        painter.setPen(QPen(QColor("#aab9c9"), 2));
        painter.drawRect(screen_);
    }
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() != Qt::LeftButton || (progress_ > 0.001 && progress_ < 0.999)
            || !pose().headHit(event->position(), frame_)) return;
        const auto beforePress = pose();
        bool invertible = false;
        headAnchor_ = beforePress.modelToScreen.inverted(&invertible).map(event->position());
        if (!invertible) return;
        pressedAngle_ = beforePress.angle;
        cursor_ = event->position(); cursorSeen_ = true;
        pressed_ = true; hiddenPress_ = progress_ >= 0.999;
        startAtPress_ = start_; press_ = event->position(); event->accept();
    }
    void mouseMoveEvent(QMouseEvent* event) override {
        cursor_ = event->position(); cursorSeen_ = true;
        if (!pressed_ || !(event->buttons() & Qt::LeftButton)) { event->accept(); return; }
        const QPointF delta = event->position() - press_;
        if (!hiddenPress_) {
            start_ = startAtPress_ + delta;
            update(); event->accept(); return;
        }
        const QPointF direction = inward();
        const double distance = QPointF::dotProduct(delta, direction);
        setProgress(1.0 - std::clamp(distance / 80.0, 0.0, 1.0));
        event->accept();
    }
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (!pressed_ || event->button() != Qt::LeftButton) return;
        if (!hiddenPress_) {
            pressed_ = false;
            const QRect bounds(start_.toPoint(), QSize(200, 200));
            const QRect full = screen_.toRect();
            entered_ = edge_dock::outerEdgeAtRelease(bounds, event->position().toPoint(),
                full, full, {full}, 24) == edge_;
            event->accept(); return;
        }
        returned_ = progress_ <= 0.01;
        if (returned_) {
            start_ = pose().modelToScreen.map(QPointF(0, 0));
            start_.setX(std::clamp(start_.x(), screen_.left(), screen_.right() - 200));
            start_.setY(std::clamp(start_.y(), screen_.top(), screen_.bottom() - 200));
        }
        pressed_ = false;
        setProgress(returned_ ? 0.0 : 1.0);
        event->accept();
    }
private:
    edge_dock::Edge edge_;
    bool continuous_ = false;
    const QRectF screen_{20, 20, 520, 520};
    QPointF start_;
    QPointF startAtPress_;
    QPointF press_;
    QPointF cursor_;
    QPointF headAnchor_;
    double pressedAngle_ = 0;
    QImage frame_;
    double progress_ = 0;
    bool pressed_ = false;
    bool hiddenPress_ = false;
    bool entered_ = false;
    bool cursorSeen_ = false;
    bool returned_ = false;
};
}

int runEdgeHideReview(QApplication& app, PetController& controller, PetWindow& window,
                      const QString& output, const QString& scenario) {
    QString side = scenario.mid(QStringLiteral("edge-hide-").size());
    const bool continuous = side.startsWith(QStringLiteral("continuous-"));
    if (continuous) side.remove(0, QStringLiteral("continuous-").size());
    const auto edge = side == QStringLiteral("left") ? edge_dock::Edge::Left
        : side == QStringLiteral("right") ? edge_dock::Edge::Right
        : side == QStringLiteral("top") ? edge_dock::Edge::Top
        : side == QStringLiteral("bottom") ? edge_dock::Edge::Bottom : edge_dock::Edge::None;
    const QDir directory(QFileInfo(output).absoluteFilePath());
    if (edge == edge_dock::Edge::None || !directory.exists()) return 2;
    window.prepareLiveInteractionReview(560);
    window.move(-10000, -10000);
    PreviewSurface view(edge, continuous);
    view.move(-10000, -10000);
    view.show();
    int eaten = 0;
    QObject::connect(&controller, &PetController::eatTriggered, &view,
                     [&](const QString&, const QPointF&, const QIcon&) { ++eaten; });
    QTimer timer, deadline;
    timer.setInterval(10); timer.setTimerType(Qt::PreciseTimer);
    deadline.setSingleShot(true);
    QElapsedTimer elapsed; elapsed.start();
    int frame = 0;
    bool finished = false;
    bool deletionSuppressed = false, acceptedHead = false;
    QPointF press, initialPress;
    QJsonObject checks;
    QJsonArray frames;
    QString memoHash;
    const auto finish = [&](int code, const QString& failure = {}) {
        if (finished) return;
        finished = true; timer.stop(); deadline.stop();
        QJsonObject result{{QStringLiteral("scenario"), scenario},
            {QStringLiteral("scope"), QStringLiteral("isolated_qt_virtual_screen_live_cubism_synthetic_pointer")},
            {QStringLiteral("passed"), code == 0}, {QStringLiteral("failure"), failure},
            {QStringLiteral("checks"), checks}, {QStringLiteral("frames"), frames},
            {QStringLiteral("frame_count"), frames.size()}, {QStringLiteral("wall_ms"), elapsed.elapsed()},
            {QStringLiteral("render_backend"), window.renderBackend()},
            {QStringLiteral("limitations"), QJsonArray{QStringLiteral("virtual screen, not actual desktop window placement"),
                QStringLiteral("synthetic mouse and fixture events; no real hook or physical mouse"),
                QStringLiteral("delete gate is fixture-only; no normal-entry behavior enabled")}}};
        QSaveFile file(directory.filePath(QStringLiteral("scene.json")));
        const QByteArray bytes = QJsonDocument(result).toJson();
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) code = 2;
        app.exit(code);
    };
    const auto check = [&](const QString& name, bool ok) {
        checks[name] = ok;
        if (!ok) finish(1, name);
        return ok;
    };
    const auto mouse = [&](QEvent::Type type, QPointF local, Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent event(type, local, QPointF(view.mapToGlobal(local.toPoint())), button, buttons, Qt::NoModifier);
        QApplication::sendEvent(&view, &event);
    };
    QObject::connect(&deadline, &QTimer::timeout, &app, [&] { finish(2, QStringLiteral("deadline")); });
    QObject::connect(&timer, &QTimer::timeout, &app, [&] {
        if (finished) return;
        QString event;
        if (!check(QStringLiteral("native_and_isolated"), window.renderBackend() == QStringLiteral("cubism_native")
                && app.property("desktopCompanionReviewSettingsIsolated").toBool())) return;
        if (frame == 0) {
            if (!check(QStringLiteral("memo_created"), window.setUserMemo(QStringLiteral("edge fixture")))) return;
            memoHash = window.memoReviewState().value(QStringLiteral("text_sha256")).toString();
        }
        if (frame == 2) {
            initialPress = view.facePoint();
            mouse(QEvent::MouseButtonPress, initialPress, Qt::LeftButton, Qt::LeftButton);
            if (!check(QStringLiteral("normal_head_press_accepted"), view.pressed())) return;
            event = QStringLiteral("normal_press");
        }
        if (frame >= 3 && frame <= 10) {
            mouse(QEvent::MouseMove, initialPress + view.outward() * ((frame - 2) * 20), Qt::NoButton, Qt::LeftButton);
            event = QStringLiteral("drag_to_edge");
        }
        if (frame == 11) {
            mouse(QEvent::MouseButtonRelease, initialPress + view.outward() * 160, Qt::LeftButton, Qt::NoButton);
            if (!check(QStringLiteral("edge_drop_enters"), view.entered() && !view.pressed())) return;
            if (!check(QStringLiteral("preview_gate_entered"), window.setEdgeReviewSuppressed(true))) return;
            event = QStringLiteral("edge_drop");
        }
        if (frame >= 12 && frame <= 32) view.setProgress((frame - 12) / 20.0);
        if (frame == 33) {
            const QPointF target = view.pose().modelToScreen.map(QPointF(160, 70));
            mouse(QEvent::MouseMove, target, Qt::NoButton, Qt::NoButton);
            const QPointF gaze = view.pose().gaze(view.cursor());
            if (!check(QStringLiteral("cursor_event_transforms_to_model"), view.cursorSeen()
                    && std::abs(gaze.x() - 0.6) < 1e-8 && std::abs(gaze.y() + 0.3) < 1e-8)) return;
            event = QStringLiteral("cursor_target");
        }
        if (frame == 35) { controller.turnStarted(QStringLiteral("edge-session"), QStringLiteral("edge-turn")); event = QStringLiteral("work_started"); }
        if (frame == 37) { controller.applyPermissionSnapshot(QStringLiteral("edge-session"), QStringLiteral("edge-turn"), {QStringLiteral("request")}); event = QStringLiteral("permission_requested"); }
        if (frame == 40) {
            // The actual desktop source is deliberately not connected here.
            // This fixture checks gate semantics; it is not the production gate.
            if (!window.edgeReviewState().value(QStringLiteral("suppressed")).toBool()) controller.desktopItemDeleted();
            else deletionSuppressed = true;
            event = QStringLiteral("fixture_delete");
        }
        if (frame == 41) {
            const QPointF partialPress = view.facePoint();
            mouse(QEvent::MouseButtonPress, partialPress, Qt::LeftButton, Qt::LeftButton);
            mouse(QEvent::MouseMove, partialPress + view.inward() * 20, Qt::NoButton, Qt::LeftButton);
            mouse(QEvent::MouseButtonRelease, partialPress + view.inward() * 20, Qt::LeftButton, Qt::NoButton);
            if (!check(QStringLiteral("short_pull_returns_to_hidden"), !view.returned()
                    && !view.pressed() && view.progress() == 1.0)) return;
            event = QStringLiteral("short_pull");
        }
        if (frame == 42) {
            const QPointF cancelPress = view.facePoint();
            mouse(QEvent::MouseButtonPress, cancelPress, Qt::LeftButton, Qt::LeftButton);
            mouse(QEvent::MouseMove, cancelPress + view.inward() * 20, Qt::NoButton, Qt::LeftButton);
            QEvent ungrab(QEvent::UngrabMouse);
            QApplication::sendEvent(&view, &ungrab);
            if (!check(QStringLiteral("lost_capture_restores_hidden"), !view.pressed()
                    && !view.returned() && view.progress() == 1.0)) return;
            event = QStringLiteral("lost_capture");
        }
        if (frame == 44) {
            const auto state = window.edgeReviewState();
            mouse(QEvent::MouseButtonPress, QPointF(25, 25), Qt::LeftButton, Qt::LeftButton);
            if (!check(QStringLiteral("busy_and_permission_bookkeeping"), controller.activeTurnCount() == 1
                    && controller.pendingPermissionCount() == 1 && controller.state() == PetController::State::Busy)
                || !check(QStringLiteral("no_automatic_cue_or_delete"), deletionSuppressed && eaten == 0
                    && !state.value(QStringLiteral("permission_cue")).toBool() && !state.value(QStringLiteral("interaction")).toBool())
                || !check(QStringLiteral("memo_content_retained"), memoHash == window.memoReviewState().value(QStringLiteral("text_sha256")).toString())
                || !check(QStringLiteral("empty_area_not_grabbable"), !view.pressed())
                || !check(QStringLiteral("gaze_reaches_native_model"), window.motionParameters().value(QStringLiteral("ParamEyeBallX")) > 0.03)) return;
        }
        if (frame == 45) {
            press = view.facePoint();
            mouse(QEvent::MouseButtonPress, press, Qt::LeftButton, Qt::LeftButton);
            acceptedHead = view.pressed();
            if (!check(QStringLiteral("head_press_accepted"), acceptedHead)) return;
            event = QStringLiteral("head_press");
        }
        if (frame >= 46 && frame <= 53) {
            const QPointF cursor = press + view.inward() * ((frame - 45) * 10);
            mouse(QEvent::MouseMove, cursor, Qt::NoButton, Qt::LeftButton);
            if (!check(QStringLiteral("drag_keeps_grabbed_head_under_pointer"),
                    QLineF(view.grabbedPoint(), cursor).length() < 0.01)) return;
            event = QStringLiteral("drag_inward");
        }
        if (frame == 54) {
            mouse(QEvent::MouseButtonRelease, press + view.inward() * 80, Qt::LeftButton, Qt::NoButton);
            if (!check(QStringLiteral("drag_restored"), view.returned() && !view.pressed() && view.fullWindowInside())
                || !check(QStringLiteral("preview_gate_left"), window.setEdgeReviewSuppressed(false))) return;
            event = QStringLiteral("released_inward");
        }
        if (frame >= 55 && !check(QStringLiteral("no_old_event_replayed"), eaten == 0
                && controller.activeTurnCount() == 1 && controller.pendingPermissionCount() == 1
                && !window.edgeReviewState().value(QStringLiteral("permission_cue")).toBool())) return;
        const auto pose = view.pose();
        const QPointF targetLocal(160, 70);
        const QPointF referenceGaze = pose.gaze(pose.modelToScreen.map(targetLocal));
        if (!check(QStringLiteral("inverse_gaze_transform"), std::abs(referenceGaze.x() - 0.6) < 1e-8
                && std::abs(referenceGaze.y() + 0.3) < 1e-8)) return;
        const QPointF gaze = view.cursorSeen() ? pose.gaze(view.cursor()) : QPointF();
        window.setEdgeReviewGaze(gaze.x(), gaze.y());
        double rigX = 0, rigZ = 0;
        if (continuous && frame > 12 && frame < 32) {
            const double envelope = std::sin(3.141592653589793 * (frame - 12) / 20.0);
            rigX = 1.5 * envelope;
            rigZ = (edge == edge_dock::Edge::Right ? -1.0 : edge == edge_dock::Edge::Left ? 1.0 : 0.0) * envelope;
        }
        if (continuous) {
            if (!check(QStringLiteral("rig_entry_parameters_bounded"), std::abs(rigX) <= 1.5 && std::abs(rigZ) <= 1.0
                    && window.modelParameterRanges().contains(QStringLiteral("ParamBodyAngleX"))
                    && window.modelParameterRanges().contains(QStringLiteral("ParamBodyAngleZ")))) return;
            window.setEdgeReviewBodyOffsets(rigX, rigZ);
        }
        const QImage native = window.advanceEdgeReviewFrame(0.05);
        if (!check(QStringLiteral("native_frame"), !native.isNull())) return;
        if (continuous && frame == 22) {
            const auto state = window.edgeReviewState();
            if (!check(QStringLiteral("rig_offsets_reached_cubism"),
                    std::abs(state.value(QStringLiteral("applied_body_x")).toDouble() - rigX) < 0.001
                    && std::abs(state.value(QStringLiteral("applied_body_z")).toDouble() - rigZ) < 0.001)) return;
        }
        if (continuous && frame >= 12 && frame <= 32
            && !check(QStringLiteral("continuous_head_visible"), pose.headHit(
                pose.modelToScreen.map(QPointF(100, 80)), native))) return;
        view.setFrame(native);
        if (continuous && (frame == 16 || frame == 22 || frame == 28)
            && !native.save(directory.filePath(QStringLiteral("native-%1.png").arg(frame, 3, 10, QChar('0'))))) {
            finish(2, QStringLiteral("native_detail_save")); return;
        }
        const QString name = QStringLiteral("frame-%1.png").arg(frame, 3, 10, QChar('0'));
        if (!view.grab().save(directory.filePath(name))) { finish(2, QStringLiteral("frame_save")); return; }
        const auto params = window.motionParameters();
        frames.append(QJsonObject{{QStringLiteral("index"), frame}, {QStringLiteral("file"), name},
            {QStringLiteral("event"), event}, {QStringLiteral("progress"), view.progress()},
            {QStringLiteral("angle"), pose.angle}, {QStringLiteral("gaze_x"), gaze.x()},
            {QStringLiteral("gaze_y"), gaze.y()}, {QStringLiteral("eye_x"), params.value(QStringLiteral("ParamEyeBallX"))},
            {QStringLiteral("entry_body_x_offset"), rigX}, {QStringLiteral("entry_body_z_offset"), rigZ},
            {QStringLiteral("pressed"), view.pressed()}, {QStringLiteral("state"), window.edgeReviewState()}});
        if (++frame == 75) finish(0);
    });
    deadline.start(90000);
    QTimer::singleShot(1200, &app, [&] { if (!finished) timer.start(); });
    return app.exec();
}
