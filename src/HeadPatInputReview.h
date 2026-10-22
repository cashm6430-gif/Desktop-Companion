#pragma once

class QApplication;
class PetController;
class PetWindow;
class QString;

// Dedicated, offscreen review process: real Qt pointer handlers and the Native
// player, synthetic input only. The caller must dispatch before creating hook
// listeners or desktop event sources.
int runHeadPatInputReview(QApplication& app, PetController& controller,
                         PetWindow& window, const QString& output,
                         const QString& scenario);
