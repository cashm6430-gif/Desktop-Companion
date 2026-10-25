#pragma once

#include <QString>

class QApplication;
class PetController;
class PetWindow;

int runDragInputReview(QApplication& app, PetController& controller,
                       PetWindow& window, const QString& output,
                       const QString& scenario);
