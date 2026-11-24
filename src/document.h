#pragma once
#include <QByteArray>
#include <QString>
#include <stdexcept>
struct EncodingError : std::runtime_error {
  using std::runtime_error::runtime_error;
};
struct DiskConflict : std::runtime_error {
  QByteArray revision;
  explicit DiskConflict(QByteArray value)
      : std::runtime_error("磁盘文件已变化"), revision(std::move(value)) {}
};
struct Document {
  QString path, text;
  QByteArray codec = "UTF-8", bom, digest;
  static Document open(const QString &path, const QByteArray &encoding = {});
  void save(const QString &path, const QString &content,
            const QByteArray &approvedRevision = {});
};
QString decode(const QByteArray &bytes, const QByteArray &codec,
               bool strict = true);
QByteArray encode(const QString &text, const QByteArray &codec);
