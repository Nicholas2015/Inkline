#pragma once
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QUuid>
#include <memory>

// Each running window owns one snapshot. A second process may only restore
// snapshots whose owner has exited. QSaveFile preserves the last good copy.
class SessionStore {
  QString path_;
  std::unique_ptr<QLockFile> lock_;

public:
  QString directory;
  QJsonObject acquire() {
    QDir dir(directory.isEmpty() ? QStandardPaths::writableLocation(
                                       QStandardPaths::AppDataLocation) +
                                       "/sessions"
                                 : directory);
    if (!dir.mkpath("."))
      return {};
    for (const auto &info :
         dir.entryInfoList({"*.json"}, QDir::Files, QDir::Time)) {
      auto candidate =
          std::make_unique<QLockFile>(info.absoluteFilePath() + ".lock");
      candidate->setStaleLockTime(0);
      if (!candidate->tryLock())
        continue;
      QFile f(info.absoluteFilePath());
      if (!f.open(QIODevice::ReadOnly))
        continue;
      auto state = QJsonDocument::fromJson(f.readAll()).object();
      if (state.value("version").toInt() != 1)
        continue;
      auto tabs = state["tabs"].toArray();
      bool complete = true;
      for (int i = 0; i < tabs.size(); ++i) {
        auto tab = tabs[i].toObject();
        auto key = tab["draftId"].toString();
        if (key.isEmpty())
          continue;
        if (QUuid(key).isNull() ||
            QUuid(key).toString(QUuid::WithoutBraces) != key) {
          complete = false;
          break;
        }
        QFile body(info.absoluteFilePath() + ".drafts/" + key);
        if (!body.open(QIODevice::ReadOnly)) {
          complete = false;
          break;
        }
        const auto bytes = body.readAll();
        if (body.error() != QFileDevice::NoError) {
          complete = false;
          break;
        }
        tab.insert("draft", QString::fromUtf8(bytes));
        tabs[i] = tab;
      }
      if (!complete)
        continue;
      state["tabs"] = tabs;
      path_ = info.absoluteFilePath();
      lock_ = std::move(candidate);
      return state;
    }
    path_ = dir.filePath(QUuid::createUuid().toString(QUuid::WithoutBraces) +
                         ".json");
    lock_ = std::make_unique<QLockFile>(path_ + ".lock");
    if (!lock_->tryLock())
      path_.clear();
    return {};
  }
  bool write(QJsonObject state) {
    if (path_.isEmpty())
      return false;
    QDir dir(path_ + ".drafts");
    auto tabs = state["tabs"].toArray();
    QSet<QString> used;
    for (int i = 0; i < tabs.size(); ++i) {
      auto tab = tabs[i].toObject();
      auto key = tab["draftId"].toString();
      if (key.isEmpty())
        continue;
      used.insert(key);
      if (!dir.exists(key)) {
        if (!dir.mkpath("."))
          return false;
        QSaveFile body(dir.filePath(key));
        auto bytes = tab["draft"].toString().toUtf8();
        if (!body.open(QIODevice::WriteOnly) ||
            body.write(bytes) != bytes.size() || !body.commit())
          return false;
      }
      tab.remove("draft");
      tabs[i] = tab;
    }
    state["tabs"] = tabs;
    state.insert("version", 1);
    const auto bytes = QJsonDocument(state).toJson(QJsonDocument::Compact);
    QSaveFile f(path_);
    if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size() ||
        !f.commit())
      return false;
    for (const auto &key : dir.entryList(QDir::Files))
      if (!used.contains(key) && !QUuid(key).isNull())
        dir.remove(key);
    return true;
  }
};
