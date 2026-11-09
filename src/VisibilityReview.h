#pragma once

#include <QString>

class QApplication;
class PetController;
class PetWindow;

int runVisibilityReview(QApplication& app, PetController& controller, PetWindow& window,
                        const QString& output, qint64& nowMs);
