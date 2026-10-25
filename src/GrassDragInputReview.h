#pragma once

class QApplication;
class PetController;
class PetWindow;
class QString;

// Synthetic Qt input in a Native window. main creates a fresh output folder
// and isolates QSettings before constructing that window.
int runGrassDragInputReview(QApplication& app, PetController& controller,
                           PetWindow& window, const QString& output,
                           const QString& scenario);
