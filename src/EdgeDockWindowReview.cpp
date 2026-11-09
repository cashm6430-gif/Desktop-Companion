#include "EdgeHideReview.h"

#include "PetController.h"
#include "PetWindow.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLineF>
#include <QMouseEvent>
#include <QSaveFile>
#include <QTimer>

int runEdgeDockWindowReview(QApplication& app, PetController& controller,
                            PetWindow& window, const QString& output, const QString& scenario) {
    const QString side = scenario.mid(QStringLiteral("edge-dock-window-").size());
    const QPoint outward = side == QStringLiteral("left") ? QPoint(-1, 0)
        : side == QStringLiteral("right") ? QPoint(1, 0)
        : side == QStringLiteral("top") ? QPoint(0, -1)
        : side == QStringLiteral("bottom") ? QPoint(0, 1) : QPoint();
    constexpr int size = 280;
    const QRect screen(-10000, -10000, 900, 700);
    const QDir directory(output);
    if (outward.isNull() || !directory.exists()) return 2;
    window.prepareLiveInteractionReview(size);

    const QPoint edgeOrigin(outward.x() < 0 ? screen.left()
            : outward.x() > 0 ? screen.right() + 1 - size : screen.center().x() - size / 2,
        outward.y() < 0 ? screen.top()
            : outward.y() > 0 ? screen.bottom() + 1 - size : screen.center().y() - size / 2);
    const QPoint origin = edgeOrigin - outward * 90;
    window.move(origin);
    QTimer timer;
    QElapsedTimer elapsed;
    elapsed.start();
    int frame = 0, eaten = 0;
    bool finished = false;
    QString memoHash;
    QPointF anchor;
    QJsonObject checks;
    QJsonArray frames;
    QObject::connect(&controller, &PetController::eatTriggered, &window,
        [&](const QString&, const QPointF&, const QIcon&) { ++eaten; });
    const auto finish = [&](int code, const QString& failure = {}) {
        if (finished) return;
        finished = true;
        timer.stop();
        QSaveFile file(directory.filePath(QStringLiteral("scene.json")));
        const auto bytes = QJsonDocument(QJsonObject{
            {QStringLiteral("scenario"), scenario},
            {QStringLiteral("scope"), QStringLiteral("native_pet_window_virtual_screen_synthetic_input")},
            {QStringLiteral("passed"), code == 0}, {QStringLiteral("failure"), failure},
            {QStringLiteral("checks"), checks}, {QStringLiteral("frames"), frames},
            {QStringLiteral("screen"), QJsonArray{screen.x(), screen.y(), screen.width(), screen.height()}},
            {QStringLiteral("render_backend"), window.renderBackend()},
            {QStringLiteral("elapsed_ms"), elapsed.elapsed()}
        }).toJson();
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) code = 2;
        app.exit(code);
    };
    const auto check = [&](const QString& name, bool passed) {
        checks[name] = passed;
        if (!passed) finish(1, name);
        return passed;
    };
    const auto phase = [&] {
        return EdgeDockSession::Phase(window.edgeDockReviewState().value(QStringLiteral("phase")).toInt());
    };
    const auto face = [&] {
        const auto state = window.edgeDockReviewState();
        return QPointF(state.value(QStringLiteral("face_x")).toDouble(), state.value(QStringLiteral("face_y")).toDouble());
    };
    const auto mouse = [&](QEvent::Type type, QPointF global, Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent event(type, global - QPointF(window.pos()), global, button, buttons, Qt::NoModifier);
        QApplication::sendEvent(&window, &event);
    };
    const auto press = [&](QPointF global) { mouse(QEvent::MouseButtonPress, global, Qt::LeftButton, Qt::LeftButton); };
    const auto move = [&](QPointF global) { mouse(QEvent::MouseMove, global, Qt::NoButton, Qt::LeftButton); };
    const auto release = [&](QPointF global) { mouse(QEvent::MouseButtonRelease, global, Qt::LeftButton, Qt::NoButton); };
    const auto dragToEdge = [&] {
        window.move(origin);
        const QPointF body = QPointF(origin) + QPointF(size * 0.3, size * 0.65);
        press(body);
        move(body + outward * 45);
        move(body + outward * 96);
        release(body + outward * 96);
    };
    const auto quiet = [&] {
        return !window.edgeReviewState().value(QStringLiteral("interaction")).toBool();
    };
    QObject::connect(&timer, &QTimer::timeout, &app, [&] {
        if (elapsed.elapsed() > 45000) { finish(2, QStringLiteral("deadline")); return; }
        if (window.renderBackend() != QStringLiteral("cubism_native")) return;
        using Phase = EdgeDockSession::Phase;
        if (frame == 1) {
            dragToEdge();
            if (!check(QStringLiteral("default_off"), phase() == Phase::Normal && window.pos() != origin)) return;
            if (!check(QStringLiteral("enable_isolated_fixture"), window.setEdgeDockScreenForReview(screen))) return;
            window.move(origin);
            if (!check(QStringLiteral("create_memo"), window.setUserMemo(QStringLiteral("edge fixture")))) return;
            memoHash = window.memoReviewState().value(QStringLiteral("text_sha256")).toString();
        }
        if (frame == 2) {
            dragToEdge();
            if (!check(QStringLiteral("normal_release_enters"), phase() == Phase::Entering && !window.acceptDrops())) return;
        }
        if (frame == 8) controller.turnStarted(QStringLiteral("fixture"), QStringLiteral("ends-hidden"));
        if (frame == 10) controller.applyPermissionSnapshot(QStringLiteral("fixture"), QStringLiteral("ends-hidden"), {QStringLiteral("request")});
        if (frame == 11) {
            controller.desktopItemDeleted(QStringLiteral("synthetic-desktop-file"));
            if (!check(QStringLiteral("hidden_bookkeeping_and_delete_gate"), controller.state() == PetController::State::Busy
                    && controller.activeTurnCount() == 1 && controller.pendingPermissionCount() == 1 && eaten == 0
                    && window.edgeDockReviewState().value(QStringLiteral("motion_state")).toInt() == int(PetController::State::Idle)
                    && quiet())) return;
        }
        if (frame == 14) controller.turnStopped(QStringLiteral("fixture"), QStringLiteral("ends-hidden"));
        if (frame == 15 && !check(QStringLiteral("hidden_stop_consumed"), controller.activeTurnCount() == 0
                && controller.pendingPermissionCount() == 0 && quiet())) return;
        if (frame == 23) {
            if (!check(QStringLiteral("head_only_and_memo_preserved"), phase() == Phase::Hidden
                    && (outward.x() ? window.width() : window.height()) < size * 0.5
                    && !memoHash.isEmpty() && memoHash == window.memoReviewState().value(QStringLiteral("text_sha256")).toString())) return;
            press(QPointF(window.pos()) + QPointF(1, 1));
            if (!check(QStringLiteral("transparent_pixel_cannot_pull"), phase() == Phase::Hidden)) return;
            release(QPointF(window.pos()) + QPointF(1, 1));
            anchor = face();
            press(anchor);
            move(anchor - outward * 18);
            release(anchor - outward * 18);
            if (!check(QStringLiteral("short_pull_returns_to_head"), phase() == Phase::Hidden)) return;
        }
        if (frame == 24) {
            anchor = face();
            press(anchor);
            if (!check(QStringLiteral("head_is_draggable"), phase() == Phase::Dragging)) return;
            move(anchor - outward * 40);
            QEvent ungrab(QEvent::UngrabMouse);
            QApplication::sendEvent(&window, &ungrab);
            if (!check(QStringLiteral("lost_capture_returns_to_head"), phase() == Phase::Hidden)) return;
        }
        if (frame == 25) {
            controller.turnStarted(QStringLiteral("fixture"), QStringLiteral("still-active"));
            controller.applyPermissionSnapshot(QStringLiteral("fixture"), QStringLiteral("still-active"), {QStringLiteral("current-request")});
            anchor = face();
            press(anchor);
        }
        if (frame >= 26 && frame <= 31) {
            const QPointF target = anchor - outward * (20 * (frame - 25));
            move(target);
            if (!check(QStringLiteral("grabbed_point_tracks_mouse"), phase() == Phase::Dragging
                    && QLineF(face(), target).length() < 0.01)) return;
            if (frame == 31) {
                release(target);
                if (!check(QStringLiteral("restores_current_busy_without_replay"), phase() == Phase::Normal
                        && window.size() == QSize(size, size) && screen.contains(window.geometry()) && window.acceptDrops()
                        && controller.activeTurnCount() == 1 && controller.pendingPermissionCount() == 1 && eaten == 0
                        && window.edgeDockReviewState().value(QStringLiteral("motion_state")).toInt() == int(PetController::State::Busy)
                        && quiet())) return;
            }
        }
        if (frame == 34) {
            // Tearing down the isolated fixture while docked must recover.
            dragToEdge();
            if (!check(QStringLiteral("busy_can_dock"), phase() == Phase::Entering)) return;
            if (!check(QStringLiteral("fixture_disable_recovers"), window.setEdgeDockScreenForReview({})
                    && phase() == Phase::Normal
                    && window.size() == QSize(size, size) && screen.contains(window.geometry()) && window.acceptDrops())) return;
        }
        if (frame == 35) {
            controller.desktopItemDeleted(QStringLiteral("synthetic-desktop-file"));
            if (!check(QStringLiteral("fresh_delete_still_works"), eaten == 1
                    && controller.state() == PetController::State::Delete)) return;
            finish(0);
            return;
        }
        const auto png = QStringLiteral("frame_%1.png").arg(frame, 3, 10, QLatin1Char('0'));
        if (!check(QStringLiteral("capture_native_frame"), window.renderLiveInteractionFrame(0.05, directory.filePath(png)))) return;
        if (phase() != Phase::Normal
            && !check(QStringLiteral("dock_window_contained"), screen.contains(window.geometry()))) return;
        auto state = window.edgeDockReviewState();
        if (phase() != Phase::Normal) {
            const QImage image(directory.filePath(png));
            bool visible = false;
            for (int y = 0; y < image.height() && !visible; ++y)
                for (int x = 0; x < image.width() && !visible; ++x) visible = qAlpha(image.pixel(x, y)) >= 16;
            if (!check(QStringLiteral("visible_cropped_frame"), visible && image.size() == window.size())) return;
        }
        state.insert(QStringLiteral("image"), png);
        frames.append(state);
        ++frame;
    });
    timer.start(10);
    return app.exec();
}
