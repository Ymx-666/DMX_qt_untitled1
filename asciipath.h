#ifndef ASCIIPATH_H
#define ASCIIPATH_H

#include <QByteArray>
#include <QString>
#include <QDateTime>

inline QString safeAsciiComponent(const QString &in, const QString &fallbackPrefix)
{
    QByteArray out;
    out.reserve(in.size());
    for (int i = 0; i < in.size(); ++i) {
        const ushort u = in.at(i).unicode();
        const bool ok =
            (u >= '0' && u <= '9') ||
            (u >= 'A' && u <= 'Z') ||
            (u >= 'a' && u <= 'z') ||
            (u == '_') || (u == '-');
        out.append(ok ? char(u) : '_');
    }
    while (!out.isEmpty() && (out.at(0) == '_' || out.at(0) == '.' || out.at(0) == ' ')) out.remove(0, 1);
    while (!out.isEmpty() && (out.at(out.size() - 1) == '_' || out.at(out.size() - 1) == '.' || out.at(out.size() - 1) == ' ')) out.chop(1);
    if (out.isEmpty()) {
        out = fallbackPrefix.toLatin1() + QByteArray::number(QDateTime::currentMSecsSinceEpoch());
    }
    return QString::fromLatin1(out);
}

inline QString makeAsciiTimestamp(const QDateTime &dt, const QString &prefix)
{
    const QString raw = prefix + QString::fromLatin1(QByteArray::number(dt.toMSecsSinceEpoch()));
    return safeAsciiComponent(raw, prefix);
}

#endif
