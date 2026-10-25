#pragma once

#include <QString>

class QApplication;
class PetController;
class PetWindow;

int runWorkPostureReview(QApplication& app, PetController& controller,
                        PetWindow& window, const QString& output,
                        const QString& scenario);
