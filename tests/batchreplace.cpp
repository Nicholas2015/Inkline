#include "batchreplaceeditor.h"
#include <QFile>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <cstdio>

static void check(bool condition, const char *reason) {
  if (!condition) throw std::runtime_error(reason);
}
static void put(const QString &path, const QByteArray &data) {
  QFile out(path);
  check(out.open(QIODevice::WriteOnly) && out.write(data) == data.size() && out.flush(),
        "fixture write failed");
}
static QByteArray get(const QString &path) {
  QFile in(path);
  check(in.open(QIODevice::ReadOnly), "fixture read failed");
  return in.readAll();
}
int main(int argc, char **argv) {
  QApplication app(argc, argv);
  try {
    check(argc == 2, "expected worker executable");
    QTemporaryDir src, cache;
    check(src.isValid() && cache.isValid(), "temporary directory");
    const auto path = src.filePath("one.txt");
    const auto original = QByteArray::fromHex("fffe") + encode("a1\r\na2\na3\r", "UTF-16LE");
    put(path, original);
    put(src.filePath("two.txt"), "a4");
    put(src.filePath("binary.txt"), QByteArray("a1\0b", 4));
    put(src.filePath("ignored.cpp"), "a5");
    const auto req = cache.filePath("request.json");
    QJsonObject request{{"op", "replace-preview"}, {"root", src.path()}, {"glob", "*.txt"},
                        {"query", "a(\\d)"}, {"regex", true}, {"replacement", "b\\1"}};
    put(req, QJsonDocument(request).toJson());
    check(QProcess::execute(QString::fromLocal8Bit(argv[1]), {"--search-worker", req}) == 0, "worker failed");
    auto result = QJsonDocument::fromJson(get(cache.filePath("reply.json"))).object();
    check(result["count"].toInt() == 4 && result["skipped"].toInt() == 1, "folder scan");
    auto files = result["preview"].toArray();
    check(files.size() == 2 && get(path) == original, "preview mutated a file");
    auto first = files.at(0).toObject(), second = files.at(1).toObject();
    if (first["relative"] != "one.txt") std::swap(first, second);
    auto matches = first["matches"].toArray();
    check(matches.size() == 3 && matches.at(1).toObject()["line"].toInt() == 2, "match lines");
    QTabWidget tabs;
    auto *editor = new Edit(Document::open(path));
    tabs.addTab(editor, "one.txt");
    editor->setText("draft");
    editor->setModified(true);
    bool protectedDraft = false;
    try {
      applyBatchToTabs(&tabs, first, {matches.at(0)});
    } catch (const std::exception &) { protectedDraft = true; }
    check(protectedDraft && editor->text() == "draft" && get(path) == original, "dirty tab overwritten");
    editor->setText(editor->doc.text);
    editor->setModified(false);
    applyBatchToTabs(&tabs, first, {matches.at(0), matches.at(2)});
    check(!editor->dirty() && editor->text() == "b1\r\na2\nb3\r" &&
          editor->doc.digest == Document::open(path).digest, "clean tab did not refresh");
    check(get(path) == QByteArray::fromHex("fffe") + encode("b1\r\na2\nb3\r", "UTF-16LE"), "selected match, BOM or mixed EOL");
    put(src.filePath("two.txt"), "external edit");
    bool conflict = false;
    try {
      applyBatchFile(second, second["matches"].toArray());
    } catch (const DiskConflict &) { conflict = true; }
    check(conflict && get(src.filePath("two.txt")) == "external edit", "stale guard");
    return 0;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  }
}
