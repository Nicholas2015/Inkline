#pragma once
#include <QString>
#include <QVector>
#include <atomic>
struct Scan {
  qint64 count = 0;
  QVector<qint64> offsets;
  bool cancelled = false;
  QString error;
};
Scan scanLog(const QString &path, const QString &query, const QByteArray &codec,
             const std::atomic_bool &stop);
QByteArray readPage(const QString &path, qint64 start, const QByteArray &codec);
