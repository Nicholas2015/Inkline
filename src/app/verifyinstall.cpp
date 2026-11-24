#include "verifyinstall.h"
#include "mainwindow.h"
#include "editcommands.h"
#include "editor.h"
#include "logview.h"
#include "workspacefiles.h"
#include <QSettings>
#include <QToolBar>
#include <QTemporaryDir>
#include <QSslSocket>
#include <QElapsedTimer>
// Runs the deployed executable against disposable settings and real editor
// widgets.
int verifyInstall(const QString &output) {
  QTemporaryDir temp;
  QJsonObject result;
  auto require = [&](bool condition, const QString &name) {
    result.insert(name, condition);
    if (!condition)
      throw std::runtime_error(name.toStdString());
  };
  auto action = [](Window &w, const QString &name) -> QAction * {
    for (auto *a : w.findChildren<QAction *>())
      if (a->text() == name)
        return a;
    return nullptr;
  };
  try {
    require(temp.isValid(), "temporaryDirectory");
    require(QSslSocket::supportsSsl(), "tlsBackend");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, temp.path());
    const auto session = temp.filePath("session");
    const auto config = temp.filePath("sample.ini");
    const auto code = temp.filePath("sample.py");
    auto put = [&](const QString &p, const QByteArray &b) {
      QFile f(p);
      require(f.open(QIODevice::WriteOnly) && f.write(b) == b.size(),
              "writeFixture");
    };
    {
      QSettings legacy(QSettings::IniFormat, QSettings::UserScope, "InkEdit", "InkEditNative");
      legacy.setValue("find/history", QStringList{"legacy-search"});
      legacy.sync();
      require(QSettings().value("find/history").toStringList().contains("legacy-search"), "legacySettings");
      const auto legacyDir=temp.filePath("legacy-session");
      const auto snapshot=legacyDir+"/saved.json";
      const auto draftId=QUuid::createUuid().toString(QUuid::WithoutBraces);
      require(QDir().mkpath(snapshot+".drafts"),"legacyFixtureDirectory");
      put(snapshot+".drafts/"+draftId,QString("旧版未保存的草稿").toUtf8());
      QJsonObject tab{{"path",""},{"dirty",true},{"codec","UTF-8"},{"draftId",draftId}};
      put(snapshot,QJsonDocument(QJsonObject{{"project",temp.path()},{"version",1},{"active",0},{"tabs",QJsonArray{tab}}}).toJson());
      Window restored;
      auto *toolbar = restored.findChild<QToolBar *>();
      require(toolbar && toolbar->actions().contains(action(restored, "查找…")) &&
              toolbar->actions().contains(action(restored, "AI 代码建议…")), "sharedToolbarActions");
      restored.startSession(legacyDir);
      auto *tabs=restored.findChild<QTabWidget*>();
      auto *editor=tabs?dynamic_cast<Edit*>(tabs->currentWidget()):nullptr;
      auto *tree = restored.findChild<QTreeView *>("workspaceTree");
      auto *model = tree ? qobject_cast<QFileSystemModel *>(tree->model()) : nullptr;
      require(model && QFileInfo(model->filePath(tree->rootIndex())) == QFileInfo(temp.path()), "workspaceRoot");
      const QString nested = legacyDir + "/nested.ini";
      put(nested, "enabled=true");
      auto fileIndex = model->index(nested);
      require(fileIndex.isValid(), "workspaceNestedFile");
      tree->activated(fileIndex);
      tree->activated(fileIndex);
      require(tabs->count() == 2 && tabs->currentWidget()->property("filePath").toString() == nested,
              "workspaceOpenAndReuseTab");
      tabs->setCurrentIndex(0);
      require(!tree->currentIndex().isValid(), "workspaceDraftClearsSelection");
      tree->collapseAll();
      tabs->setCurrentIndex(1);
      require(tree->currentIndex() == fileIndex && tree->isExpanded(fileIndex.parent()),
              "workspaceTabRevealsFile");
      require(editor && editor->dirty() && editor->text()=="旧版未保存的草稿","legacyDraft");
      require(restored.windowTitle().contains("墨行 Inkline"),"renamedWindow");
    }
    put(config, encode("城市=杭州\r\nkey=1\r\n", "GBK"));
    put(code, "print('ok')\n");
    {
      Window w;
      w.startSession(session);
      w.open(config);
      w.open(code);
      auto *tabs = w.findChild<QTabWidget *>();
      require(tabs && tabs->count() == 2, "openTabs");
      tabs->setCurrentIndex(0);
      auto *e = dynamic_cast<Edit *>(tabs->currentWidget());
      require(e && e->doc.codec == "GBK", "detectGBK");
      auto *lf = action(w, "LF");
      require(lf, "eolAction");
      lf->trigger();
      require(e->text() == "城市=杭州\nkey=1\n", "convertCRLFtoLF");
      auto *utf =
          action(w, "UTF-16LE"); // Use the save-conversion action, not reload.
      for (auto *a : w.findChildren<QAction *>())
        if (a->text() == "UTF-16LE" && qobject_cast<QMenu *>(a->parent()) &&
            qobject_cast<QMenu *>(a->parent())->title() == "保存时使用编码")
          utf = a;
      require(utf, "encodingAction");
      utf->trigger();
      auto *save = action(w, "保存");
      require(save, "saveAction");
      save->trigger();
      auto check = Document::open(config);
      require(check.codec == "UTF-16LE" && check.text == e->text(),
              "savedEncodingAndContent");
      tabs->setCurrentIndex(1);
      e = dynamic_cast<Edit *>(tabs->currentWidget());
      require(e && e->lexer(), "codeHighlight");
      e->append("# 草稿\n");
      e->setCursorPosition(1, 2);
      require(w.close(), "saveDraftSession");
    }
    {
      Window w;
      w.startSession(session);
      auto *tabs = w.findChild<QTabWidget *>();
      auto *e = dynamic_cast<Edit *>(tabs->currentWidget());
      require(tabs->count() == 2 && e && e->text().contains("草稿") &&
                  e->dirty(),
              "restoreDraft");
      int line, col;
      e->getCursorPosition(&line, &col);
      require(line == 1 && col == 2, "restoreCursor");
      require(Document::open(code).text == "print('ok')\n", "preserveDisk");
      w.open(config, true); // Already-open editor keeps focus.
      auto log = temp.filePath("test.log");
      put(log, "INFO ok\nERROR failed\n");
      w.open(log);
      auto *logEditor=dynamic_cast<Edit*>(tabs->currentWidget());
      require(logEditor && dynamic_cast<LogLexer*>(logEditor->lexer()),"editableLogLexer");
      logEditor->SendScintilla(Edit::SCI_COLOURISE,0,-1);
      require(logEditor->SendScintilla(Edit::SCI_GETSTYLEAT,0UL)==LogStyle::Info &&
              logEditor->SendScintilla(Edit::SCI_GETSTYLEAT,8UL)==LogStyle::Error,"severityStyles");
      w.open(log, true);
      require(dynamic_cast<Log *>(tabs->currentWidget()) != nullptr, "openLog");
      require(!qApp->windowIcon().pixmap(32).isNull(), "applicationIcon");
    }
    {
      Window w;
      auto *tabs = w.findChild<QTabWidget *>();
      auto verifyTokens = [&](QString suffix, QByteArray content, int key, int value) {
        auto path = temp.filePath("colors." + suffix);
        put(path, content);
        w.open(path);
        auto *editor = dynamic_cast<Edit *>(tabs->currentWidget());
        require(editor && editor->lexer(), suffix + "Lexer");
        editor->SendScintilla(Edit::SCI_COLOURISE, 0UL, -1);
        const int keyStyle = editor->SendScintilla(Edit::SCI_GETSTYLEAT, key);
        const int valueStyle = editor->SendScintilla(Edit::SCI_GETSTYLEAT, value);
        require(keyStyle != valueStyle && editor->lexer()->color(keyStyle) != editor->lexer()->color(valueStyle),
                suffix + "KeyAndValueHighlight");
      };
      for (const auto &ext : {"c", "cpp", "java"}) verifyTokens(ext, "int x = 123;", 1, 9);
      verifyTokens("py", "def run(): return 1", 1, 5);
      verifyTokens("GO", "package main", 1, 9);
      verifyTokens("rs", "fn main() {}", 1, 5);
      verifyTokens("zig", "pub fn run() void {}", 1, 8);
      verifyTokens("toml", "port = 8080", 1, 8);
      verifyTokens("conf", "listen 8080;", 1, 8);
      verifyTokens("properties", "server.port=8080", 1, 13);
      verifyTokens("YML", "port: 8080", 1, 7);
      verifyTokens("json", R"({"port":8080})", 2, 9);
    }
    {
      Window w;
      auto path = temp.filePath("formatter.json");
      const auto raw = QByteArray(R"({"count":9007199254740993,"name":"demo"})");
      put(path, raw);
      w.open(path);
      auto *tabs = w.findChild<QTabWidget *>();
      auto *e = dynamic_cast<Edit *>(tabs->currentWidget());
      auto *validate = w.findChild<QAction *>("validateConfig");
      auto *format = w.findChild<QAction *>("formatConfig");
      require(e && validate && format, "configCommands");
      auto waitConfig = [&] {
        QElapsedTimer timer;
        timer.start();
        while (e->property("configBusy").toBool() && timer.elapsed() < 8000)
          QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
      };
      format->trigger();
      waitConfig();
      require(e->text().contains("9007199254740993") && e->text().contains('\n') &&
              Document::open(path).text == QString::fromUtf8(raw), "formatBufferOnly");
      e->undo();
      require(e->text() == QString::fromUtf8(raw), "undoConfigurationFormat");
      e->setText("{\n  \"enabled\": nope\n}");
      validate->trigger();
      waitConfig();
      int row, column;
      e->getCursorPosition(&row, &column);
      require(row == 1 && column > 0 && !w.statusBar()->currentMessage().isEmpty(),
              "configurationErrorLocation");
    }
    {
      Window w;
      w.open(code);
      auto *tabs = w.findChild<QTabWidget *>();
      auto *e = dynamic_cast<Edit *>(tabs->currentWidget());
      std::unique_ptr<Edit> left(linkedPane(e)), right(linkedPane(e));
      left->setCursorPosition(0, 0);
      left->SendScintilla(Edit::SCI_REPLACESEL, "# linked\\n");
      require(e->dirty() && e->text().startsWith("# linked") && right->text() == e->text(),
              "splitViewsShareEdits");
      right->undo();
      require(e->text() == Document::open(code).text && left->text() == right->text(), "splitSharedUndo");
      auto *toggle = action(w, "切换本行书签");
      auto *nextMark = action(w, "下个书签");
      auto *previousMark = action(w, "上个书签");
      auto *sortLines = action(w, "行排序");
      require(toggle && toggle->shortcut() == QKeySequence("Ctrl+F2") &&
              nextMark && nextMark->shortcut() == QKeySequence("F2") &&
              previousMark && previousMark->shortcut() == QKeySequence("Shift+F2") && sortLines,
              "editorMenuBindings");
      e->setText("b\na");
      e->setCursorPosition(1, 0);
      toggle->trigger();
      e->setCursorPosition(0, 0);
      previousMark->trigger();
      int row, column;
      e->getCursorPosition(&row, &column);
      require(row == 1, "previousBookmarkWraps");
      e->setCursorPosition(0, 0);
      nextMark->trigger();
      e->getCursorPosition(&row, &column);
      require(row == 1, "nextBookmarkActivated");
      e->selectAll();
      sortLines->trigger();
      require(e->text() == "a\nb", "sortLinesMenuAction");
      require(WorkspaceFiles::relocated("/work/api/a.ini", "/work/api", "/work/v2") == "/work/v2/a.ini" &&
              WorkspaceFiles::relocated("/work/api-old/b.ini", "/work/api", "/work/v2") == "/work/api-old/b.ini",
              "workspaceRenameByComponent");
    }
    result.insert("passed", true);
  } catch (const std::exception &e) {
    result.insert("error", QString::fromUtf8(e.what()));
    result.insert("passed", false);
  }
  QFile out(output);
  if (!out.open(QIODevice::WriteOnly))
    return 2;
  out.write(QJsonDocument(result).toJson());
  return result["passed"].toBool() ? 0 : 1;
}
