#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QTemporaryDir>
#include <functional>
#include <memory>

class QProcess;
using SearchCache = std::shared_ptr<QTemporaryDir>;

inline constexpr qint64 SearchPageSize = 512;
qint64 searchPageCount(qint64 matches);
QList<QJsonArray> readSearchPage(const SearchCache &cache, qint64 page);
bool writeSearchFile(const QString &path, const QByteArray &data);

class SearchTask final : public QObject {
public:
  explicit SearchTask(QObject *owner = nullptr, QString workerProgram = {});
  ~SearchTask() override;

  // A new run supersedes the previous request. Only the latest run calls done.
  void run(SearchCache cache, const QJsonObject &request,
           std::function<void(QJsonObject)> done);
  void cancel();

private:
  void finish(QProcess *child, QJsonObject reply);
  QString program;
  QProcess *process = nullptr;
  std::function<void(QJsonObject)> completion;
  std::function<void()> next;
};
