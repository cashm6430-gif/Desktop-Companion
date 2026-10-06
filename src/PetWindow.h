#pragma once

#include "PetController.h"
#include "ParameterMotion.h"
#include "PetPointerGesture.h"

#include <QElapsedTimer>
#include <QIcon>
#include <QImage>
#include <QJsonObject>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPoint>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QWidget>

#ifdef HAVE_CUBISM
class CubismCanvas;
#endif

// A 96 px frameless, click-through overlay that flies the deleted file's icon
// from its desktop position towards the pet's fist, where the in-window eat
// choreography (drawFedProp) takes over at its grip moment. Defined in
// PetWindow.cpp; only the pointer lives here.
class DeleteOverlay;
class QAction;

class PetWindow final : public QWidget {
    Q_OBJECT
public:
    explicit PetWindow(PetController* controller, QWidget* parent = nullptr);
    ~PetWindow() override;
    const ParameterMotion::Parameters& motionParameters() const { return motion_.values(); }
    QString renderBackend() const;
    QString renderError() const;
    int renderSampleCount() const;
    // Parameter id -> {min, max, default} from the loaded MOC3; empty when the
    // Native backend is not up, so callers can tell "no data" from "no range".
    QJsonObject modelParameterRanges() const;
    bool saveRenderFrame(const QString& path);
    void shutdown();
    void setPreviewPose(const ParameterMotion::Parameters& parameters, int pixels = 840);
    // `bubblePulse` is the thinking-bubble envelope for this instant. A comic
    // bubble is not a MOC3 parameter, so it rides beside the pose rather than
    // inside it -- but it does have to be captured, or the approval frames
    // would show a pet that never thinks.
    bool renderSequenceFrame(const ParameterMotion::Parameters& parameters, double seconds,
                             const QString& path, double bubblePulse = 0.0);
    void setInteractionPreviewEnabled(bool enabled);
    bool interactionPreviewEnabled() const { return interactionPreviewEnabled_; }
    bool isHeadAt(const QPointF& position) const;
    bool startGrassInteraction();
    QString grassInteractionPhase() const { return motion_.grassInteractionPhase(); }
    double grassInteractionTime() const { return motion_.grassInteractionTime(); }
    QPainterPath grassTipHitPath() const;
    bool isGrassTipAt(const QPointF& position) const;
    // Native scene review uses the actual window player and Qt input handlers,
    // with a manual frame clock instead of the desktop's wall clock.
    void prepareLiveInteractionReview(int pixels = 840);
    bool renderLiveInteractionFrame(double seconds, const QString& path);

protected:
    bool event(QEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    // Dropping a file on the character is the feed gesture: the eat action
    // plays for whatever was dropped. Windows click-through already scopes
    // this to the character, because the drop only reaches the window while
    // the cursor sits on opaque pixels (see updateInputTransparency()).
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

signals:
    void filesDropped(const QStringList& paths);

private:
    void setState(PetController::State state);
    // The comic "?" bubble is window-layer art: no drawable in the MOC3 paints
    // it, so every frame the pet presents -- the live window and the review
    // captures -- has to have it composed in.
    QImage frameWithBubble(const QImage& frame, double pulse) const;
    // The frame as presented: Cubism framebuffer, mirrored horizontally when
    // the pet lunges to the left (so the authored right-handed choreography
    // faces the deleted file), with the bubble composed on top. Used by both
    // the live paint and the interaction mask so they can never disagree.
    QImage composedFrame() const;
    // One shared entry for every eat trigger (drop, desktop deletion, tray
    // menu). Drives the deleted-file icon: overlay flight from the desktop
    // position, then the in-window wrap choreography; mirror the frame when
    // the file sat on the pet's other side.
    void onEatTriggered(const QString& file, const QPointF& sourcePos, const QIcon& icon);
    void setInteractionMask(const QPixmap& artwork);
    void updateInputTransparency();
    void showMenu(const QPoint& globalPos);
    // The fed-file prop: the icon of the dropped file rides the window layer
    // (same reasoning as the bubble -- it is UI, not a MOC3 drawable), drifting
    // from the hand towards the mouth while the eat action plays.
    void drawFedProp(QPainter& painter);
    // Moves the dropped files to the Recycle Bin (FOF_ALLOWUNDO, recoverable),
    // never a hard delete. The pet "eating" a file must stay undoable.
    void recyclePaths(const QStringList& paths);
    void updatePointerGesture();
    void releasePointerGesture();
    void constrainPositionToScreen();
    void advanceLiveFrame(double seconds);
    void playGrass();
    bool grassInteractionApproved() const;
    QPixmap idleImage_;
    QPixmap busyImage_;
    QPixmap deleteImage_;
#ifdef HAVE_CUBISM
    QPixmap cubismHitMask_;
    QImage cubismFrame_;
    CubismCanvas* cubismCanvas_ = nullptr;
#endif
    QSystemTrayIcon tray_;
    QMenu trayMenu_;
    QTimer frameTimer_;
    QTimer laptopPreviewTimer_;
    QTimer patPreviewTimer_;
    QAction* interactionPreviewAction_ = nullptr;
    QImage hitCoverage_;
    QElapsedTimer frameClock_;
    ParameterMotion motion_;
    PetController* controller_;
    QPoint dragOffset_;
    QPoint gestureWindowOrigin_;
    QPointF dragLastGlobal_;   // scene-move drag reaction input (motion card 4)
    QPointF dragVelocity_;
    QElapsedTimer dragClock_;
    PetPointerGesture pointerGesture_;
    bool patAttempted_ = false;
    bool interactionPreviewEnabled_ = false;
    bool grassTouchPressed_ = false;
    bool dragging_ = false;
    int frame_ = 0;
    QElapsedTimer fedClock_;
    QPixmap fedIcon_;
    QPixmap wrapProp_;
    // True while a deletion triggered a lunge towards a file on the pet's
    // left: the Cubism frame is mirrored so the authored right-hand reach
    // plays as a left-hand one. Reset when the delete state ends.
    bool lungeMirrored_ = false;
    DeleteOverlay* deleteOverlay_ = nullptr;
};
