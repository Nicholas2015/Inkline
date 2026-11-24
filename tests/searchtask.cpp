#include "searchtask.h"
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTimer>
#include <cstdio>

namespace {
int worker(const QString &path) {
  QFile request(path);
  if (!request.open(QIODevice::ReadOnly))
    return 1;
  const auto mode = QJsonDocument::fromJson(request.readAll()).object()["mode"].toString();
  const auto dir = QFileInfo(path).absoluteDir();
  if (mode == "wait") {
    if (!writeSearchFile(dir.filePath("ready"), "ready"))
      return 1;
    QTimer::singleShot(30000, QCoreApplication::instance(), &QCoreApplication::quit);
    return QCoreApplication::exec();
  }
  if (mode == "fail")
    return 2;
  if (mode == "no-reply")
    return 0;
  const auto reply = mode == "broken" ? QByteArray("{") :
      QJsonDocument(QJsonObject{{"mode", mode}}).toJson(QJsonDocument::Compact);
  return writeSearchFile(dir.filePath("reply.json"), reply) ? 0 : 1;
}

bool waitFor(const std::function<bool()> &ready) {
  if (ready())
    return true;
  QEventLoop loop;
  QTimer poll;
  QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
    if (ready())
      loop.quit();
  });
  poll.start(10);
  QTimer::singleShot(5000, &loop, &QEventLoop::quit);
  loop.exec();
  return ready();
}
} // namespace

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  const auto args = app.arguments();
  if (args.size() == 3 && args[1] == "--search-worker")
    return worker(args[2]);

  int failures = 0;
  auto check = [&](bool success, const char *name) {
    if (!success) {
      std::fprintf(stderr, "FAIL: %s\n", name);
      ++failures;
    }
  };

  int discarded = 0;
  auto startWaiting = [&](SearchTask &task) {
    auto cache = std::make_shared<QTemporaryDir>();
    task.run(cache, {{"mode", "wait"}}, [&](QJsonObject) { ++discarded; });
    check(waitFor([&] { return QFileInfo::exists(cache->filePath("ready")); }),
          "worker signals ready before cancellation");
  };

  SearchTask task;
  startWaiting(task);
  task.cancel();
  task.cancel();
  startWaiting(task);

  // The first run below replaces a live worker. Each subsequent run also
  // checks that an earlier failure leaves the task usable.
  auto run = [&](SearchTask &target, const QString &mode,
                 SearchCache cache = std::make_shared<QTemporaryDir>()) {
    QList<QJsonObject> replies;
    target.run(cache, {{"mode", mode}},
               [&](QJsonObject reply) { replies.append(reply); });
    check(waitFor([&] { return !replies.isEmpty(); }), "worker completes");
    target.cancel();
    check(replies.size() == 1, "one reply per request");
    return replies.value(0);
  };
  check(run(task, "new")["mode"].toString() == "new", "only the replacement run completes");
  for (const auto *mode : {"fail", "no-reply", "broken"})
    check(!run(task, mode)["error"].toString().isEmpty(), mode);
  check(run(task, "ok")["mode"].toString() == "ok", "success after worker error");

  startWaiting(task);
  task.run(std::make_shared<QTemporaryDir>(), {{"mode", "superseded"}},
           [&](QJsonObject) { ++discarded; });
  check(run(task, "latest")["mode"].toString() == "latest", "only the latest queued run completes");

  startWaiting(task);
  task.run(std::make_shared<QTemporaryDir>(), {{"mode", "cancelled"}},
           [&](QJsonObject) { ++discarded; });
  task.cancel();


  auto cache = std::make_shared<QTemporaryDir>();
  check(run(task, "old", cache)["mode"].toString() == "old", "populate reused cache");
  check(!run(task, "no-reply", cache)["error"].toString().isEmpty(),
        "a reused cache cannot supply an earlier reply");
  SearchTask missingProgram(nullptr, cache->filePath("missing-program"));
  check(!run(missingProgram, "ok")["error"].toString().isEmpty(), "worker fails to start");
  cache = std::make_shared<QTemporaryDir>();
  check(QDir(cache->path()).mkdir("request.json"), "block request path");
  QJsonObject writeError;
  task.run(cache, {}, [&](QJsonObject reply) { writeError = reply; });
  check(!writeError["error"].toString().isEmpty(), "request write failure");
  {
    SearchTask temporary;
    startWaiting(temporary);
  }
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  check(discarded == 0, "cancel, restart and destruction suppress old completions");

  check(searchPageCount(0) == 0 && searchPageCount(1) == 1 &&
        searchPageCount(512) == 1 && searchPageCount(513) == 2,
        "page count at batch boundaries");
  const QJsonArray first{"first", 1, 2, 3, "text", 0};
  const QJsonArray second{"second", 0, 0, 0, "", "path", -1};
  const auto jsonLine = [](const QJsonArray &row) {
    return QJsonDocument(row).toJson(QJsonDocument::Compact) + '\n';
  };
  check(writeSearchFile(cache->filePath("0.jsonl"), jsonLine(first) + "broken\n[]\n[1]\n" + jsonLine(second)) &&
        writeSearchFile(cache->filePath("1.jsonl"), jsonLine(second)), "write result pages");
  check(readSearchPage(cache, 0) == QList<QJsonArray>{first, second}, "read valid rows in order");
  check(readSearchPage(cache, 1) == QList<QJsonArray>{second}, "read requested page only");
  check(readSearchPage(cache, 2).isEmpty(), "missing page");
  return failures != 0;
}
