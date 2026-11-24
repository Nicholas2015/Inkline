#pragma once
#include <QByteArray>
#include <QString>
#include <functional>

using RecordVisitor =
    std::function<bool(qint64, qint64, qint64, qsizetype, const QString &)>;
void visitLogRecords(const QString &path, qint64 size, qint64 start,
                     const QByteArray &codec, const QString &query, bool regex,
                     bool caseSensitive, const RecordVisitor &visit);
int runLogJob();
qint64 logAlignedStart(const QString &path, qint64 offset,
                       const QByteArray &codec);
