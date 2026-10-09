#pragma once

#include <QStringList>

class QApplication;

// Explicit optional entry point. It never constructs a PetWindow and never
// alters the existing hook configuration or removes a live hook server.
int runToonHost(QApplication& app, const QStringList& arguments);
