#include "document.h"
#include "logfile.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <cstdio>
#include <stdexcept>
static void check(bool ok, const char *label) {
  if (!ok)
    throw std::runtime_error(label);
}
static void write(const QString &p, const QByteArray &d) {
  QFile f(p);
  check(f.open(QIODevice::WriteOnly) && f.write(d) == d.size(), "write");
}
static QByteArray read(const QString &p) {
  QFile f(p);
  check(f.open(QIODevice::ReadOnly), "read");
  return f.readAll();
}
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  std::atomic_bool stop{false};
  try {
    if (argc == 4 && app.arguments()[1] == "--search") {
      QElapsedTimer timer;
      timer.start();
      auto result =
          scanLog(app.arguments()[2], app.arguments()[3], "UTF-8", stop);
      if (!result.error.isEmpty())
        throw std::runtime_error(result.error.toStdString());
      printf("%lld %lld\n", (long long)result.count,
             (long long)timer.nsecsElapsed());
      return 0;
    }
    QTemporaryDir dir;
    auto path = dir.filePath("service.ini");
    QString sample = "城市=杭州\r\nx=10\ny=保留\r";
    for (auto codec : {QByteArray("UTF-8"), QByteArray("GBK"),
                       QByteArray("UTF-16LE"), QByteArray("UTF-16BE")}) {
      auto bom = QByteArray::fromHex(codec == "UTF-16LE"   ? "fffe"
                                     : codec == "UTF-16BE" ? "feff"
                                                           : "");
      auto bytes = bom + encode(sample, codec);
      write(path, bytes);
      auto doc = Document::open(path);
      check(doc.codec == codec && doc.text == sample, "detect");
      doc.save(path, doc.text);
      check(read(path) == bytes, "roundtrip");
      auto edited = sample;
      edited.replace("10", "11");
      doc.save(path, edited);
      check(read(path) == bom + encode(edited, codec), "edit");
    }
    for (auto codec : {QByteArray("UTF-16LE"), QByteArray("UTF-16BE"),
                       QByteArray("Windows-1252")}) {
      QString value = "prix=45 €\r\n";
      auto bytes = encode(value, codec);
      write(path, bytes);
      auto imported = Document::open(path, codec);
      check(imported.text == value && imported.bom.isEmpty(),
            "manual encoding");
      imported.save(path, imported.text);
      check(read(path) == bytes, "no BOM conversion");
    }
    auto bom = QByteArray::fromHex("efbbbf");
    write(path, bom + sample.toUtf8());
    auto doc = Document::open(path);
    doc.save(path, doc.text);
    check(read(path) == bom + sample.toUtf8(), "utf8 bom");
    write(path, "external");
    bool conflict = false;
    try {
      doc.save(path, "other");
    } catch (...) {
      conflict = true;
    }
    check(conflict && read(path) == "external", "conflict");
    try {
      doc.save(path, "mine");
      check(false, "expected revision conflict");
    } catch (const DiskConflict &conflict) {
      write(path, "other revision");
      bool stale = false;
      try {
        doc.save(path, "mine", conflict.revision);
      } catch (const DiskConflict &second) {
        stale = true;
        doc.save(path, "mine", second.revision);
      }
      check(stale && read(path) == bom + "mine", "recheck confirmed overwrite");
    }
    QByteArray raw(256 * 1024 - 1, 'x');
    auto term = QString("请求");
    raw += term.toUtf8();
    raw += term.toUtf8();
    write(path, raw);
    auto scan = scanLog(path, term, "UTF-8", stop);
    check(scan.count == 2 && scan.offsets[0] == 256 * 1024 - 1, "cross block");
    write(path, QByteArray::fromHex("814040"));
    check(scanLog(path, "@", "GBK", stop).offsets == QVector<qint64>{2},
          "gbk trail");
    stop = true;
    check(scanLog(path, "@", "GBK", stop).cancelled, "cancel");
    puts("round trips, edited bytes, external conflict, UTF-8 boundary, GBK "
         "trail and cancellation passed");
  } catch (const std::exception &e) {
    fprintf(stderr, "%s\n", e.what());
    return 1;
  }
}
