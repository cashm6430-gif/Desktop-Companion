#pragma once

#include <QtGlobal>

class QApplication;
class PetController;
class PetWindow;
class QString;

// Isolated Native review, dispatched before creating event sources. main must
// seed a collapsed fixture note and inject its local store/settings paths
// before constructing PetWindow. No hooks or desktop file operations are used.
int runMemoInputReview(QApplication& app, PetController& controller,
                       PetWindow& window, const QString& output,
                       qint64& reviewNowMs);
