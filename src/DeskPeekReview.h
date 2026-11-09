#pragma once

#include <QString>

class QApplication;
class PetWindow;

int runDeskPeekReview(QApplication& app, PetWindow& window, const QString& output,
                      const QString& scenario, const QString& clipPath);
