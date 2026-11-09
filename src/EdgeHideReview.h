#pragma once
#include <QString>
class QApplication;
class PetController;
class PetWindow;
int runEdgeHideReview(QApplication& app, PetController& controller,
                      PetWindow& window, const QString& output, const QString& scenario);
int runEdgeDockWindowReview(QApplication& app, PetController& controller,
                            PetWindow& window, const QString& output, const QString& scenario);
