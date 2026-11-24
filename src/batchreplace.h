#pragma once
#include "document.h"
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>

// Positions are UTF-16 offsets into the exact revision used by the preview.
inline Document applyBatchFile(const QJsonObject &file, const QJsonArray &matches) {
  const QString path = file["path"].toString();
  QFileInfo info(path);
  if (!info.isFile() || info.isSymLink() ||
      info.canonicalFilePath() != file["canonical"].toString())
    throw std::runtime_error("文件路径已变化");
  auto doc = Document::open(path, file["codec"].toString().toLatin1());
  if (doc.digest.toHex() != file["digest"].toString().toLatin1())
    throw DiskConflict(doc.digest);
  qsizetype cursor = 0;
  QString updated;
  for (auto value : matches) {
    const auto row = value.toObject();
    const auto at = row["start"].toInteger(-1);
    const auto before = row["before"].toString();
    if (at < cursor || at > doc.text.size() || !QStringView(doc.text).mid(at).startsWith(before))
      throw std::runtime_error("预览位置已失效");
    updated += QStringView(doc.text).mid(cursor, at - cursor);
    updated += row["after"].toString();
    cursor = at + before.size();
  }
  updated += QStringView(doc.text).mid(cursor);
  if (updated != doc.text)
    doc.save(path, updated);
  return doc;
}
