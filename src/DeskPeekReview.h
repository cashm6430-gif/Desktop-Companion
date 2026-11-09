#pragma once

#include <QString>

class QApplication;
class PetController;
class PetWindow;

int runDeskPeekReview(QApplication& app, PetWindow& window, const QString& output,
                      const QString& scenario, const QString& clipPath);
int runDeskPeekAdoptionReview(QApplication& app, PetController& controller, PetWindow& window,
                              const QString& output);
int runDeskPeekManualReview(QApplication& app, PetController& controller, PetWindow& window,
                            const QString& output);
