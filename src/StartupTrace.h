#pragma once

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QString>

// Startup phase trace for desktop diagnosis (the GUI exe has no stdout). One
// line per checkpoint, milliseconds since the first checkpoint, next to the
// exe. Cheap enough to leave enabled; delete the file to reset it.
namespace startup {

inline qint64 sinceFirst() {
    static QElapsedTimer timer;
    if (!timer.isValid()) timer.start();
    return timer.elapsed();
}

inline void trace(const QString& what) {
    QFile log(QCoreApplication::applicationDirPath() + QStringLiteral("/startup_trace.log"));
    if (!log.open(QIODevice::Append | QIODevice::Text)) return;
    log.write(qPrintable(QStringLiteral("[%1 ms] %2\n").arg(sinceFirst()).arg(what)));
}

} // namespace startup
