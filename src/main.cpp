#include "appearance.h"
#include "codetools.h"
#include "editcommands.h"
#include "editor.h"
#include "logscan.h"
#include "logview.h"
#include "searchtools.h"
#include "sessionstore.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QFileOpenEvent>
#include <QFileSystemWatcher>
#include <QJsonArray>
#include <QKeyEvent>
#include <QScrollBar>
#include <QSettings>
#include <QStyle>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
class Window final : public QMainWindow {
  QTabWidget *tabs = new QTabWidget;
  QAction *saveAction, *asAction;
  SessionStore sessions;
  QTimer snapshot;
  QFileSystemWatcher disk;
  QSet<QString> diskChanges;
  bool checkingDisk = false;
  void watch(const QString &p) {
    if (!p.isEmpty() && QFileInfo::exists(p) && !disk.files().contains(p))
      disk.addPath(p);
  }
  void inspectDisk() {
    if (checkingDisk || !isActiveWindow())
      return;
    auto *e = edit();
    if (!e || !diskChanges.remove(e->doc.path))
      return;
    auto p = e->doc.path;
    watch(p);
    QFile f(p);
    if (f.size() >= 32 * 1024 * 1024) {
      statusBar()->showMessage("磁盘文件已增长为大日志，可另存当前修改", 5000);
      return;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!f.open(QIODevice::ReadOnly) || !hash.addData(&f)) {
      statusBar()->showMessage("磁盘文件不可读，可另存当前内容", 5000);
      return;
    }
    auto digest = hash.result();
    f.close();
    if (digest == e->doc.digest ||
        digest == e->property("ignoreDigest").toByteArray())
      return;
    checkingDisk = true;
    QMessageBox box(
        QMessageBox::Question, "磁盘文件已变化",
        QString("「%1」已在其他程序中更改。").arg(QFileInfo(p).fileName()),
        QMessageBox::NoButton, this);
    auto *reload = box.addButton(e->dirty() ? "放弃草稿并重新载入" : "重新载入",
                                 QMessageBox::DestructiveRole);
    auto *compare = box.addButton("查看磁盘内容", QMessageBox::ActionRole);
    auto *ignore = box.addButton("保留当前内容", QMessageBox::RejectRole);
    box.setDefaultButton(ignore);
    box.exec();
    try {
      if (box.clickedButton() == reload)
        replaceTab(e, Document::open(p));
      else if (box.clickedButton() == compare) {
        auto *d = new QDialog(this);
        d->setWindowTitle("当前内容与磁盘版本");
        d->setAttribute(Qt::WA_DeleteOnClose);
        auto *row = new QHBoxLayout(d);
        for (const QString &v :
             QStringList{e->text(), Document::open(p).text}) {
          auto *view = new QPlainTextEdit(v);
          view->setReadOnly(true);
          row->addWidget(view);
        }
        d->resize(900, 600);
        d->show();
      } else
        e->setProperty("ignoreDigest", digest);
    } catch (const std::exception &err) {
      report(this, err);
    }
    checkingDisk = false;
  }
  bool pending = false, restoring = true;
  QList<QPair<QString, bool>> closed;
  QMenu *recent = nullptr;
  QToolButton *codecButton = new QToolButton, *eolButton = new QToolButton,
              *languageButton = new QToolButton;
  static QString pathOf(QWidget *w) {
    return w ? w->property("filePath").toString() : QString();
  }
  void remember(const QString &path) {
    if (path.isEmpty())
      return;
    auto paths = QSettings().value("recentFiles").toStringList();
    paths.removeAll(path);
    paths.prepend(path);
    QSettings().setValue("recentFiles", paths.mid(0, 20));
  }
  void changed() {
    if (!restoring)
      pending = true;
  }
  QFuture<bool> writing;
  bool checkpoint(bool asynchronous = false) {
    if (asynchronous && writing.isRunning())
      return true;
    writing.waitForFinished();
    QJsonArray list;
    for (int i = 0; i < tabs->count(); ++i) {
      QWidget *w = tabs->widget(i);
      QJsonObject item{{"path", pathOf(w)}};
      if (auto *e = dynamic_cast<Edit *>(w)) {
        int row, col;
        e->getCursorPosition(&row, &col);
        item.insert("row", row);
        item.insert("col", col);
        item.insert("lang", e->language);
        item.insert("readCodec", QString::fromLatin1(e->doc.codec));
        if (e->dirty() || e->doc.path.isEmpty()) {
          if (e->property("draftId").toString().isEmpty()) {
            e->setProperty("draftId",
                           QUuid::createUuid().toString(QUuid::WithoutBraces));
            e->setProperty("draftBody", e->text());
          }
          item.insert("draftId", e->property("draftId").toString());
          item.insert("draft", e->property("draftBody").toString());
          item.insert("codec", QString::fromLatin1(e->doc.codec));
          item.insert("bom", QString::fromLatin1(e->doc.bom.toHex()));
          item.insert("digest", QString::fromLatin1(e->doc.digest.toHex()));
          item.insert("dirty", e->dirty());
        }
      } else if (auto *l = dynamic_cast<Log *>(w)) {
        item.insert("log", true);
        item.insert("view", l->saveView());
      }
      list.append(item);
    }
    QJsonObject state{{"tabs", list},
                      {"active", tabs->currentIndex()},
                      {"project", property("projectRoot").toString()}};
    if (asynchronous) {
      pending = false;
      writing =
          QtConcurrent::run([this, state] { return sessions.write(state); });
      return true;
    }
    if (!sessions.write(state)) {
      statusBar()->showMessage("草稿恢复文件写入失败，请手动保存", 5000);
      return false;
    }
    pending = false;
    return true;
  }
  void restore() {
    auto state = sessions.acquire();
    setProperty("projectRoot", state["project"].toString());
    for (auto entry : state["tabs"].toArray()) {
      auto item = entry.toObject();
      auto path = item["path"].toString();
      if (item.contains("draft")) {
        Document doc;
        doc.path = path;
        doc.text = item["draft"].toString();
        doc.codec = item["codec"].toString().toLatin1();
        doc.bom = QByteArray::fromHex(item["bom"].toString().toLatin1());
        doc.digest = QByteArray::fromHex(item["digest"].toString().toLatin1());
        addEdit(doc);
        edit()->formatDirty = item["dirty"].toBool();
      } else if (QFileInfo::exists(path)) {
        if (item["log"].toBool()) {
          open(path, true);
          if (auto *l = dynamic_cast<Log *>(tabs->currentWidget()))
            l->restoreView(item["view"].toObject());
        } else {
          try {
            addEdit(
                Document::open(path, item["readCodec"].toString().toLatin1()));
          } catch (const std::exception &) {
            open(path);
          }
        }
      } else
        continue;
      if (auto *e = edit()) {
        e->language = item["lang"].toString();
        e->applyLanguage();
        e->setCursorPosition(item["row"].toInt(), item["col"].toInt());
      }
    }
    if (!state.isEmpty())
      tabs->setCurrentIndex(
          qBound(0, state["active"].toInt(), qMax(0, tabs->count() - 1)));
    restoring = false;
    refresh();
    snapshot.setInterval(2000);
    connect(&snapshot, &QTimer::timeout, this, [this] {
      if (writing.isValid() && writing.isFinished() && !writing.result()) {
        pending = true;
        statusBar()->showMessage("草稿恢复文件写入失败，请手动保存", 5000);
      }
      if (pending)
        checkpoint(true);
    });
    snapshot.start();
  }
  void formats() {
    auto tool = [this](QToolButton *btn, const QString &tip) {
      btn->setAutoRaise(true);
      btn->setToolTip(tip);
      btn->setPopupMode(QToolButton::InstantPopup);
      btn->setMenu(new QMenu(btn));
      statusBar()->addPermanentWidget(btn);
      return btn->menu();
    };
    auto *language = tool(languageButton, "语法高亮");
    QList<QPair<QString, QString>> options = {{"自动识别", ""},
                                              {"纯文本", "txt"}};
    for (const auto &item : editorLanguages())
      options.append({item.title, item.extensions.first()});
    for (auto [name, ext] : options)
      language->addAction(name, this, [=] {
        if (auto *e = edit()) {
          e->language = ext;
          e->applyLanguage();
          changed();
          refresh();
        }
      });
    auto *eols = tool(eolButton, "转换整个文档的换行符");
    for (auto [name, mode] : {std::pair{"LF", Edit::EolUnix},
                              {"CRLF", Edit::EolWindows},
                              {"CR", Edit::EolMac}})
      eols->addAction(name, this, [=] {
        if (auto *e = edit()) {
          e->beginUndoAction();
          e->convertEols(mode);
          e->setEolMode(mode);
          e->endUndoAction();
          changed();
          refresh();
        }
      });
    auto *enc = tool(codecButton, "文件编码");
    auto *reopen = enc->addMenu("以此编码重新打开"),
         *convert = enc->addMenu("保存时使用编码");
    for (const auto &label :
         QStringList{"UTF-8", "UTF-8 BOM", "GBK", "UTF-16LE", "UTF-16BE",
                     "UTF-16LE 无BOM", "UTF-16BE 无BOM", "Windows-1252"}) {
      QByteArray codec = label.section(' ', 0, 0).toLatin1();
      convert->addAction(label, this, [=] {
        auto *e = edit();
        if (!e)
          return;
        try {
          encode(e->text(), codec);
          auto bom = QByteArray::fromHex(label == "UTF-8 BOM"  ? "efbbbf"
                                         : label == "UTF-16LE" ? "fffe"
                                         : label == "UTF-16BE" ? "feff"
                                                               : "");
          if (codec != e->doc.codec || bom != e->doc.bom) {
            e->doc.codec = codec;
            e->doc.bom = bom;
            e->formatDirty = true;
            changed();
            refresh();
          }
        } catch (const std::exception &err) {
          report(this, err);
        }
      });
      if (label.contains(' '))
        continue;
      reopen->addAction(label, this, [=] {
        auto *e = edit();
        if (!e || e->doc.path.isEmpty())
          return;
        try {
          auto doc = Document::open(e->doc.path, codec);
          if (e->dirty() &&
              QMessageBox::question(this, "重新打开",
                                    "放弃当前修改并重新读取磁盘文件？",
                                    QMessageBox::Yes | QMessageBox::No,
                                    QMessageBox::No) != QMessageBox::Yes)
            return;
          replaceTab(e, doc);
        } catch (const std::exception &err) {
          report(this, err);
        }
      });
    }
  }
  void replaceTab(Edit *e, const Document &doc) {
    int idx = tabs->indexOf(e);
    addEdit(doc);
    tabs->tabBar()->moveTab(tabs->currentIndex(), idx);
    tabs->removeTab(tabs->indexOf(e));
    e->deleteLater();
    changed();
    refresh();
  }
  Edit *edit() const { return dynamic_cast<Edit *>(tabs->currentWidget()); }
  void refresh() {
    auto *e = edit();
    saveAction->setEnabled(e);
    asAction->setEnabled(e);
    if (e) {
      auto name = e->doc.path.isEmpty() ? QString("未命名")
                                        : QFileInfo(e->doc.path).fileName();
      tabs->setTabText(tabs->currentIndex(), name + (e->dirty() ? " *" : ""));
    }
    for (auto *b : {codecButton, eolButton, languageButton})
      b->setVisible(e);
    if (e) {
      codecButton->setText(QString::fromLatin1(e->doc.codec) +
                           (e->doc.bom.isEmpty() ? "" : " BOM"));
      eolButton->setText(e->eolMode() == Edit::EolUnix      ? "LF"
                         : e->eolMode() == Edit::EolWindows ? "CRLF"
                                                            : "CR");
      languageButton->setText(e->lexer()
                                  ? QString::fromLatin1(e->lexer()->language())
                                  : QString("纯文本"));
      int line, col;
      e->getCursorPosition(&line, &col);
      statusBar()->showMessage(
          QString("行 %1    列 %2").arg(line + 1).arg(col + 1));
    } else
      statusBar()->showMessage(
          QDir::toNativeSeparators(pathOf(tabs->currentWidget())));
    setWindowTitle(tabs->tabText(tabs->currentIndex()) + " — 墨行 Inkline");
  }
  bool save(Edit *e, bool choose) {
    if (!e)
      return false;
    QString path = e->doc.path;
    if (path.isEmpty() || choose)
      path = QFileDialog::getSaveFileName(this, "保存文件", path);
    if (path.isEmpty())
      return false;
    try {
      auto old = e->doc.path;
      try {
        e->doc.save(path, e->text());
      } catch (const DiskConflict &conflict) {
        QMessageBox box(QMessageBox::Warning, "磁盘版本已变化",
                        "保存当前缓冲区会覆盖磁盘上的变化。",
                        QMessageBox::NoButton, this);
        auto *overwrite =
            box.addButton("保存当前版本", QMessageBox::DestructiveRole);
        auto *copy = box.addButton("另存为…", QMessageBox::ActionRole);
        auto *cancel = box.addButton(QMessageBox::Cancel);
        box.setDefaultButton(cancel);
        box.exec();
        if (box.clickedButton() == copy)
          return save(e, true);
        if (box.clickedButton() != overwrite)
          return false;
        e->doc.save(path, e->text(), conflict.revision);
      }
      if (old != e->doc.path)
        e->applyLanguage();
      e->formatDirty = false;
      e->setModified(false);
      remember(e->doc.path);
      watch(e->doc.path);
      changed();
      refresh();
      return true;
    } catch (const std::exception &err) {
      report(this, err);
      return false;
    }
  }
  bool allowClose(QWidget *tab) {
    auto *e = dynamic_cast<Edit *>(tab);
    if (!e || !e->dirty())
      return true;
    tabs->setCurrentWidget(e);
    auto ans = QMessageBox::question(
        this, "保存修改",
        QString("保存「%1」的修改？").arg(tabs->tabText(tabs->indexOf(e))),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
        QMessageBox::Cancel);
    return ans == QMessageBox::Discard ||
           (ans == QMessageBox::Save && save(e, false));
  }
  void addEdit(Document doc) {
    auto *e = new Edit(std::move(doc));
    watch(e->doc.path);
    editorPreferences(e);
    e->markerDefine(Edit::RightTriangle, 1);
    e->setMarkerBackgroundColor(QColor("#4085aa"), 1);
    e->setMarginWidth(1, 12);
    e->setMarginMarkerMask(1, 2);
    tabs->addTab(e, "未命名");
    tabs->setCurrentWidget(e);
    connect(e, &QsciScintilla::modificationChanged, this,
            [this] { refresh(); });
    refresh();
    connect(e, &Edit::textChanged, this, [this, e] {
      e->setProperty("draftId", QVariant());
      e->setProperty("draftBody", QVariant());
      changed();
    });
    connect(e, &Edit::cursorPositionChanged, this, [this] {
      changed();
      refresh();
    });
    changed();
    e->setFocus();
  }

protected:
  void closeEvent(QCloseEvent *ev) override {
    if (!checkpoint()) {
      for (int i = 0; i < tabs->count(); ++i)
        if (!allowClose(tabs->widget(i))) {
          ev->ignore();
          return;
        }
    }
    ev->accept();
  }

public:
  ~Window() override { writing.waitForFinished(); }
  Window() {
    resize(1100, 740);
    connect(&disk, &QFileSystemWatcher::fileChanged, this,
            [this](const QString &p) {
              diskChanges.insert(p);
              QTimer::singleShot(200, this, [this] { inspectDisk(); });
            });
    connect(qApp, &QGuiApplication::applicationStateChanged, this,
            [this] { QTimer::singleShot(0, this, [this] { inspectDisk(); }); });
    setCentralWidget(tabs);
    tabs->setDocumentMode(true);
    tabs->setTabsClosable(true);
    auto *f = menuBar()->addMenu("文件(&F)");
    f->addAction("新建", QKeySequence::New, this, [this] { addEdit({}); });
    f->addAction("打开…", QKeySequence::Open, this, [this] {
      open(QFileDialog::getOpenFileName(this, "打开文件"));
    });
    f->addAction("只读打开日志…", this, [this] {
      open(QFileDialog::getOpenFileName(this, "打开日志"), true);
    });
    saveAction = f->addAction("保存", QKeySequence::Save, this,
                              [this] { save(edit(), false); });
    asAction = f->addAction("另存为…", QKeySequence::SaveAs, this,
                            [this] { save(edit(), true); });
    f->addAction("全部保存", this, [this] {
      for (int i = 0; i < tabs->count(); ++i)
        if (auto *e = dynamic_cast<Edit *>(tabs->widget(i)); e && e->dirty()) {
          tabs->setCurrentWidget(e);
          if (!save(e, false))
            break;
        }
    });
    recent = f->addMenu("最近文件");
    connect(recent, &QMenu::aboutToShow, this, [this] {
      recent->clear();
      for (const auto &p : QSettings().value("recentFiles").toStringList())
        recent->addAction(p, this, [this, p] { open(p); });
    });
    f->addAction("重新打开关闭的标签", QKeySequence("Ctrl+Shift+T"), this,
                 [this] {
                   if (!closed.isEmpty()) {
                     auto entry = closed.takeLast();
                     if (entry.first.isEmpty())
                       addEdit({});
                     else
                       open(entry.first, entry.second);
                   }
                 });
    f->addAction("选择工作文件夹…", this, [this] {
      auto dir = QFileDialog::getExistingDirectory(
          this, "工作文件夹", property("projectRoot").toString());
      if (!dir.isEmpty()) {
        setProperty("projectRoot", dir);
        changed();
      }
    });
    f->addAction("关闭标签页", QKeySequence::Close, this, [this] {
      if (tabs->currentIndex() >= 0)
        emit tabs->tabCloseRequested(tabs->currentIndex());
    });
    f->addAction("退出", QKeySequence::Quit, this, &QWidget::close);
    auto *m = menuBar()->addMenu("编辑(&E)");
    auto basic = [&](const QString &title, auto method) {
      m->addAction(title, this, [this, method] {
        if (auto *e = edit())
          (e->*method)();
      });
    };
    basic("撤销", &Edit::undo);
    basic("重做", &Edit::redo);
    basic("剪切", &Edit::cut);
    basic("复制", &Edit::copy);
    basic("粘贴", &Edit::paste);
    // Let the focused widget handle standard editing shortcuts (also works in
    // log views).

    auto openPath = [this](QString p) { open(p); };
    installSearchTools(
        this, m, tabs, openPath,
        [this] {
          if (auto *l = dynamic_cast<Log *>(tabs->currentWidget()))
            l->showSearch();
        },
        [this](qint64 n, QString c, int length) {
          if (auto *l = dynamic_cast<Log *>(tabs->currentWidget()))
            l->gotoSearchHit(n, c, length);
        },
        [this](QString p) { open(p, true); });
    m->addAction("跳转到位置…", QKeySequence("Ctrl+G"), this, [this] {
      if (auto *l = dynamic_cast<Log *>(tabs->currentWidget())) {
        l->jumpDialog();
        return;
      }
      if (auto *e = edit()) {
        bool ok;
        auto t = QInputDialog::getText(this, "跳转", "行号或 行:列",
                                       QLineEdit::Normal, {}, &ok);
        auto parts = t.split(':');
        if (ok && parts.size() <= 2) {
          auto row = parts[0].toInt() - 1;
          auto col = parts.size() == 2 ? parts[1].toInt() - 1 : 0;
          if (row >= 0 && row < e->lines() && col >= 0) {
            e->setCursorPosition(row, col);
            e->ensureLineVisible(row);
          }
        }
      }
    });
    m->addAction("本地补全", QKeySequence("Ctrl+Space"), this, [this] {
      if (auto *e = edit())
        e->autoCompleteFromAll();
    });
    installEditCommands(this, m, tabs);
    installCodeTools(this, tabs, openPath);
    formats();
    auto *bar = addToolBar("文件");
    bar->setMovable(false);
    bar->setIconSize(QSize(16, 16));
    bar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    for (auto [name, icon] : {std::pair{"新建", Appearance::Symbol::New},
                              {"打开…", Appearance::Symbol::Open},
                              {"保存", Appearance::Symbol::Save}}) {
      for (auto *a : f->actions())
        if (a->text() == name) {
          a->setIcon(Appearance::icon(icon));
          bar->addAction(a);
          break;
        }
    }
    for (auto *a : m->actions())
      if (a->text().startsWith("查找…")) {
        a->setIcon(Appearance::icon(Appearance::Symbol::Search));
        bar->addSeparator();
        bar->addAction(a);
        break;
      }
    for (auto *a : findChildren<QAction *>())
      if (a->text() == "AI 代码建议…") {
        a->setIcon(Appearance::icon(Appearance::Symbol::Suggest));
        auto *spacing = new QWidget;
        spacing->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        bar->addWidget(spacing);
        bar->addAction(a);
        break;
      }
    connect(tabs, &QTabWidget::currentChanged, this, [this] {
      changed();
      refresh();
      QTimer::singleShot(0, this, [this] { inspectDisk(); });
    });
    connect(tabs, &QTabWidget::tabCloseRequested, this, [this](int i) {
      auto *w = tabs->widget(i);
      if (allowClose(w)) {
        closed.append({pathOf(w), dynamic_cast<Log *>(w) != nullptr});
        changed();
        tabs->removeTab(i);
        w->deleteLater();
        if (!tabs->count())
          addEdit({});
      }
    });
  }
  void open(const QString &path, bool log = false) {
    if (path.isEmpty())
      return;
    auto absolute = QFileInfo(path).absoluteFilePath();
    for (int i = 0; i < tabs->count(); ++i)
      if (pathOf(tabs->widget(i)) == absolute &&
          (dynamic_cast<Log *>(tabs->widget(i)) != nullptr) ==
              (log || QFileInfo(absolute).size() >= 32 * 1024 * 1024)) {
        tabs->setCurrentIndex(i);
        return;
      }
    try {
      if (log || QFileInfo(path).size() >= 32 * 1024 * 1024) {
        auto *tab = new Log(absolute);
        tab->setProperty("filePath", absolute);
        if (auto *v = tab->findChild<QPlainTextEdit *>("logPage")) {
          connect(v, &QPlainTextEdit::cursorPositionChanged, this,
                  [this] { changed(); });
          connect(v->verticalScrollBar(), &QScrollBar::valueChanged, this,
                  [this] { changed(); });
        }
        tabs->addTab(tab, QFileInfo(path).fileName() + " · 只读");
        tabs->setCurrentWidget(tab);
      } else {
        try {
          addEdit(Document::open(path));
        } catch (const EncodingError &err) {
          bool ok = false;
          auto enc = QInputDialog::getItem(
              this, "选择文件编码",
              QString::fromUtf8(err.what()) + "。使用哪种编码打开？",
              {"GBK", "UTF-16LE", "UTF-16BE", "Windows-1252", "UTF-8"}, 0,
              false, &ok);
          if (!ok)
            return;
          addEdit(Document::open(path, enc.toLatin1()));
        }
      }
      if (!restoring)
        remember(absolute);
    } catch (const std::exception &err) {
      report(this, err);
    }
  }
  void startSession(const QString &dir = {}) {
    sessions.directory = dir;
    restore();
  }
  void ensureTab() {
    if (!tabs->count())
      addEdit({});
  }
};
#include "../tests/appchecks.h"
class DesktopApp final : public QApplication {
public:
  using QApplication::QApplication;
  QStringList queued;
  std::function<void(QString)> openFile;
  bool event(QEvent *ev) override {
    if (ev->type() == QEvent::FileOpen) {
      auto path = static_cast<QFileOpenEvent *>(ev)->file();
      if (!path.isEmpty()) {
        if (openFile)
          openFile(path);
        else
          queued.append(path);
        return true;
      }
    }
    return QApplication::event(ev);
  }
};
int main(int argc, char **argv) {
  if (argc == 2 && QByteArray(argv[1]) == "--log-job")
    return runLogJob();
  if (argc == 3 && QByteArray(argv[1]) == "--search-worker") {
    QCoreApplication core(argc, argv);
    return runSearchWorker(core.arguments().at(2));
  }
  DesktopApp app(argc, argv);
  app.setOrganizationName("InkEdit");
  // Keep the original storage identity for settings, session locks and draft bodies.
  app.setApplicationName("InkEditNative");
  app.setApplicationDisplayName("墨行 Inkline");
  Appearance::apply(app);
  app.setWindowIcon(QIcon(":/inkline.png"));
  if (argc == 3 && app.arguments()[1] == "--verify-install")
    return verifyInstall(app.arguments()[2]);
  auto profile = qEnvironmentVariable("INKLINE_PROFILE", qEnvironmentVariable("INKEDIT_NATIVE_PROFILE"));
  if (!profile.isEmpty()) {
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, profile);
  }
  Window w;
  w.startSession(profile.isEmpty() ? QString()
                                   : QDir(profile).filePath("sessions"));
  app.openFile = [&w](QString path) {
    w.open(path);
    w.show();
    w.raise();
  };
  for (const auto &file : app.queued + app.arguments().sliced(1))
    w.open(file);
  w.ensureTab();
  w.show();
  int code = app.exec();
  app.openFile = {};
  return code;
}
