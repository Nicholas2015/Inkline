#pragma once
#include <QSslSocket>
// Runs the deployed executable against disposable settings and real editor
// widgets.
static int verifyInstall(const QString &output) {
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
      put(snapshot,QJsonDocument(QJsonObject{{"version",1},{"active",0},{"tabs",QJsonArray{tab}}}).toJson());
      Window restored;
      restored.startSession(legacyDir);
      auto *tabs=restored.findChild<QTabWidget*>();
      auto *editor=tabs?dynamic_cast<Edit*>(tabs->currentWidget()):nullptr;
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
