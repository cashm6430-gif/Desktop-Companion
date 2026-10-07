#pragma once

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QString>

// Sleep lifecycle trace for desktop diagnosis (GUI exe has no stdout). One
// line per phase change, next to the exe. Cheap enough to leave enabled.
// Kept as a free function so both the motion coordinator and any future
// caller log into the same file with the same format.
inline void sleepTrace(const QString& what)
{
    static QFile log(QCoreApplication::applicationDirPath()
                     + QStringLiteral("/sleep_trace.log"));
    if (!log.isOpen()) log.open(QIODevice::Append | QIODevice::Text);
    if (log.isOpen())
        log.write(qPrintable(QStringLiteral("[%1] %2\n")
            .arg(QDateTime::currentMSecsSinceEpoch()).arg(what)));
}
