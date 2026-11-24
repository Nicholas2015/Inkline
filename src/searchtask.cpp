#include "searchtask.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <utility>

qint64 searchPageCount(qint64 matches) {
  return matches > 0 ? 1 + (matches - 1) / SearchPageSize : 0;
}

QList<QJsonArray> readSearchPage(const SearchCache &cache, qint64 page) {
  QList<QJsonArray> rows;
  if (!cache || !cache->isValid() || page < 0)
    return rows;
  QFile file(cache->filePath(QString::number(page) + ".jsonl"));
  if (!file.open(QIODevice::ReadOnly))
    return rows;
  while (!file.atEnd()) {
    const auto line = file.readLine();
    if (line.isEmpty())
      break;
    const auto row = QJsonDocument::fromJson(line).array();
    if (row.size() >= 6)
      rows.append(row);
  }
  return rows;
}

bool writeSearchFile(const QString &path, const QByteArray &data) {
  QFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(data) == data.size() &&
         file.flush();
}

namespace {
QJsonObject readReply(const QTemporaryDir &cache, int code,
                      QProcess::ExitStatus status) {
  QFile file(cache.filePath("reply.json"));
  if (code || status != QProcess::NormalExit || !file.open(QIODevice::ReadOnly))
    return {{"error", "搜索进程退出，请重试。"}};
  const auto reply = QJsonDocument::fromJson(file.readAll()).object();
  if (file.error() != QFileDevice::NoError || reply.isEmpty())
    return {{"error", "搜索结果不完整"}};
  return reply;
}
} // namespace

SearchTask::SearchTask(QObject *owner, QString workerProgram)
    : QObject(owner),
      program(workerProgram.isEmpty() ? QCoreApplication::applicationFilePath()
                                      : workerProgram) {}

SearchTask::~SearchTask() {
  cancel();
  if (process)
    process->waitForFinished(1000);
}

void SearchTask::cancel() {
  next = {};
  completion = {};
  if (process)
    process->kill();
}

void SearchTask::finish(QProcess *child, QJsonObject reply) {
  if (child != process)
    return;
  process = nullptr;
  child->deleteLater();
  auto done = std::exchange(completion, {});
  auto restart = std::exchange(next, {});
  if (restart)
    restart();
  else if (done)
    done(std::move(reply));
}

void SearchTask::run(SearchCache cache, const QJsonObject &request,
                     std::function<void(QJsonObject)> done) {
  if (process) {
    // Wait for the old child to exit before reusing its request/reply files.
    // A second restart replaces this queued request.
    completion = {};
    next = [this, cache, request, done] { run(cache, request, done); };
    process->kill();
    return;
  }
  if (!cache || !cache->isValid()) {
    done({{"error", "无法写入临时文件"}});
    return;
  }
  QFile staleReply(cache->filePath("reply.json"));
  const QString path = cache->filePath("request.json");
  if ((staleReply.exists() && !staleReply.remove()) ||
      !writeSearchFile(path, QJsonDocument(request).toJson(QJsonDocument::Compact))) {
    done({{"error", "无法写入临时文件"}});
    return;
  }

  auto *child = new QProcess(this);
  process = child;
  completion = std::move(done);
  connect(child, &QProcess::started, this, [this, child] {
    if (child == process && !completion)
      child->kill();
  });
  connect(child, &QProcess::errorOccurred, this,
          [this, child](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart)
              finish(child, {{"error", child->errorString()}});
          });
  connect(child, &QProcess::finished, this,
          [this, child, cache](int code, QProcess::ExitStatus status) {
            finish(child, completion ? readReply(*cache, code, status) : QJsonObject());
          });
  child->start(program, {"--search-worker", path});
}
