#include "mainwindow.h"
#include "appearance.h"
#include "workspacefiles.h"
#include "codetools.h"
#include "configtools.h"
#include "editcommands.h"
#include "editor.h"
#include "logview.h"
#include "searchtools.h"
#include <QDockWidget>
#include <QTreeView>
#include <QFileSystemWatcher>
#include <QScrollBar>
#include <QSettings>
#include <QStyle>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>

QString Window::pathOf(QWidget *w) {
  return w ? w->property("filePath").toString() : QString();
}

void Window::formats() {
  auto *formatsBar = new QWidget(statusBar());
  formatsBar->setObjectName("documentFormats");
  auto *row = new QHBoxLayout(formatsBar);
  row->setContentsMargins(0, 0, 0, 0);
  row->setSpacing(4);
  statusBar()->addPermanentWidget(formatsBar);
  auto tool = [row](QToolButton *btn, const QString &tip) {
    btn->setAutoRaise(true);
    btn->setToolTip(tip);
    btn->setPopupMode(QToolButton::InstantPopup);
    btn->setMenu(new QMenu(btn));
    btn->setAccessibleName(tip);
    btn->setFocusPolicy(Qt::StrongFocus);
    row->addWidget(btn);
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
    })->setData(ext);
  connect(language, &QMenu::aboutToShow, this, [this, language] {
    if (auto *e = edit())
      for (auto *action : language->actions()) {
        action->setCheckable(true);
        action->setChecked(action->data().toString() == e->language);
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
    })->setData(int(mode));
  connect(eols, &QMenu::aboutToShow, this, [this, eols] {
    if (auto *e = edit())
      for (auto *action : eols->actions()) {
        action->setCheckable(true);
        action->setChecked(action->data().toInt() == int(e->eolMode()));
      }
  });
  auto *enc = tool(codecButton, "文件编码");
  auto *reopen = enc->addMenu("以此编码重新打开"),
       *convert = enc->addMenu("保存时使用编码");
  connect(convert, &QMenu::aboutToShow, this, [this, convert] {
    if (auto *e = edit()) {
      QString current = QString::fromLatin1(e->doc.codec);
      if (current.startsWith("UTF-16") && e->doc.bom.isEmpty())
        current += " 无BOM";
      else if (current == "UTF-8" && !e->doc.bom.isEmpty())
        current += " BOM";
      for (auto *action : convert->actions()) {
        action->setCheckable(true);
        action->setChecked(action->text() == current);
      }
    }
  });
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

Edit *Window::edit() const { return dynamic_cast<Edit *>(tabs->currentWidget()); }

void Window::refresh() {
  auto *e = edit();
  saveAction->setEnabled(e);
  asAction->setEnabled(e);
  if (e) {
    auto name = e->doc.path.isEmpty() ? QString("未命名")
                                      : QFileInfo(e->doc.path).fileName();
    tabs->setTabText(tabs->currentIndex(), name + (e->dirty() ? " *" : ""));
    tabs->setTabToolTip(tabs->currentIndex(), QDir::toNativeSeparators(e->doc.path));
  }
  codecButton->parentWidget()->setVisible(e);
  if (e) {
    codecButton->setText(QStringLiteral("编码  ") + QString::fromLatin1(e->doc.codec) +
                         (e->doc.bom.isEmpty() ? "" : " BOM"));
    eolButton->setText(QStringLiteral("换行  ") + (e->eolMode() == Edit::EolUnix      ? "LF"
                       : e->eolMode() == Edit::EolWindows ? "CRLF"
                                                          : "CR"));
    languageButton->setText(QStringLiteral("语法  ") + (e->lexer()
                                ? QString::fromUtf8(e->lexer()->language())
                                : QString("纯文本")));
    int line, col;
    e->getCursorPosition(&line, &col);
    statusBar()->showMessage(
        QString("行 %1    列 %2").arg(line + 1).arg(col + 1));
  } else
    statusBar()->showMessage(
        QDir::toNativeSeparators(pathOf(tabs->currentWidget())));
  setWindowTitle(tabs->tabText(tabs->currentIndex()) + " — 墨行 Inkline");
}

Window::Window() {
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
  tabs->tabBar()->setObjectName("documentStrip");
  tabs->tabBar()->setExpanding(false);
  tabs->setUsesScrollButtons(true);
  tabs->setElideMode(Qt::ElideNone);
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
  auto *chooseFolder = f->addAction("选择工作文件夹…", this, [this] {
    auto dir = QFileDialog::getExistingDirectory(
        this, "工作文件夹", property("projectRoot").toString());
    if (!dir.isEmpty()) {
      setWorkspaceFolder(dir);
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
  auto configAction = [&](const QString &title, const QString &id, bool pretty) {
    auto *action = m->addAction(title, this, [this, pretty] {
      auto *e = edit();
      const auto ext = e ? (e->language.isEmpty() ? QFileInfo(e->doc.path).suffix().toLower() : e->language) : QString();
      ConfigTools::Format format;
      if (ext == "json") format = ConfigTools::Format::Json;
      else if (ext == "yaml" || ext == "yml") format = ConfigTools::Format::Yaml;
      else if (ext == "toml") format = ConfigTools::Format::Toml;
      else {
        statusBar()->showMessage("请打开 JSON、YAML 或 TOML 文件，或在状态栏选择对应语言", 6000);
        return;
      }
      if (pretty && e->isReadOnly()) {
        statusBar()->showMessage("当前文档为只读", 4000);
        return;
      }
      if (e->property("configBusy").toBool()) {
        statusBar()->showMessage("正在处理当前配置", 3000);
        return;
      }
      e->setProperty("configBusy", true);
      const auto source = e->text();
      auto *watcher = new QFutureWatcher<ConfigTools::Result>(e);
      connect(e, &QsciScintilla::textChanged, watcher, [watcher] {
        watcher->setProperty("stale", true);
      });
      statusBar()->showMessage(pretty ? "正在格式化配置…" : "正在校验配置语法…", 4000);
      connect(watcher, &QFutureWatcher<ConfigTools::Result>::finished, this,
              [this, e, watcher, source, pretty, ext] {
        watcher->deleteLater();
        e->setProperty("configBusy", false);
        if (edit() != e) return;
        const auto currentExt = e->language.isEmpty() ? QFileInfo(e->doc.path).suffix().toLower() : e->language;
        if (watcher->property("stale").toBool() || currentExt != ext || (pretty && e->isReadOnly())) {
          statusBar()->showMessage("文档已更改，请重新运行配置命令", 5000);
          return;
        }
        const auto result = watcher->result();
        if (!result.valid() && result.line == 0) {
          statusBar()->showMessage(result.error, 6000);
          return;
        }
        if (!result.valid()) {
          const auto row = qBound(0, result.line - 1, qMax(0, e->lines() - 1));
          const auto col = qBound(0, result.column - 1, int(e->text(row).toUcs4().size()));
          e->setCursorPosition(row, col);
          e->ensureCursorVisible();
          e->setFocus();
          statusBar()->showMessage(QString("第 %1 行，第 %2 列：%3").arg(result.line).arg(result.column).arg(result.error));
        } else if (pretty && result.text != source) {
          int line, column;
          e->getCursorPosition(&line, &column);
          e->beginUndoAction();
          e->selectAll();
          e->replaceSelectedText(result.text);
          e->endUndoAction();
          e->setCursorPosition(qMin(line, e->lines() - 1), column);
          statusBar()->showMessage("配置已格式化", 5000);
        } else {
          statusBar()->showMessage(pretty ? "配置已符合格式" : "配置语法正确", 5000);
        }
      });
      watcher->setFuture(QtConcurrent::run([format, source, pretty] {
        return ConfigTools::process(format, source, pretty);
      }));
    });
    action->setObjectName(id);
  };
  configAction("校验配置语法", "validateConfig", false);
  configAction("格式化配置", "formatConfig", true);
  installEditCommands(this, m, tabs, [this](Edit *doc) { save(doc, false); });
  installCodeTools(this, tabs, openPath);
  auto *rail = new QWidget;
  rail->setObjectName("workspaceRail");
  auto *column = new QVBoxLayout(rail);
  column->setContentsMargins(10, 8, 10, 12);
  column->setSpacing(8);
  folderName->setProperty("section", true);
  folderName->setTextFormat(Qt::PlainText);
  folderName->setBuddy(folderTree);
  column->addWidget(folderName);
  folderName->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
  chooseWorkspace->setIcon(Appearance::icon(Appearance::Symbol::Open));
  connect(chooseWorkspace, &QPushButton::clicked, chooseFolder, &QAction::trigger);
  auto *openRecent = new QPushButton(Appearance::icon(Appearance::Symbol::Recent), "最近文件");
  openRecent->setObjectName("workspaceRecent");
  openRecent->setMenu(recent);
  auto *actions = new QVBoxLayout;
  actions->setSpacing(0);
  for (auto *button : {chooseWorkspace, openRecent}) {
    button->setProperty("workspaceAction", true);
    button->setAutoDefault(false);
    button->setIconSize(QSize(16, 16));
    actions->addWidget(button);
  }
  column->addLayout(actions);
  folderModel->setFilter(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
  folderTree->setObjectName("workspaceTree");
  folderTree->setAccessibleName("工作文件夹目录树");
  folderTree->setModel(folderModel);
  for (int i = 1; i < folderModel->columnCount(); ++i)
    folderTree->hideColumn(i);
  folderTree->setHeaderHidden(true);
  folderTree->setIndentation(14);
  folderTree->setUniformRowHeights(true);
  folderTree->setEditTriggers(QAbstractItemView::NoEditTriggers);
  connect(folderTree, &QTreeView::activated, this, [this](const QModelIndex &index) {
    const auto file = folderModel->fileInfo(index);
    if (file.isFile()) open(file.absoluteFilePath());
  });
  WorkspaceFiles::install(folderTree, folderModel, [this](QString path) { open(path); },
                          [this](QString from, QString to) { workspacePathMoved(from, to); });
  column->addWidget(folderTree);
  column->setStretchFactor(folderTree, 1);
  connect(folderModel, &QFileSystemModel::directoryLoaded, this, [this](const QString &dir) {
    // A tab can be activated before its parent directory finishes loading.
    const QString file = pathOf(tabs->currentWidget());
    if (!file.isEmpty() && QFileInfo(file).absolutePath() == dir)
      revealActiveFile();
  });
  column->addWidget(emptySpace, 1);
  workspace->setWidget(rail);
  setWorkspaceFolder({});
  workspace->setAllowedAreas(Qt::LeftDockWidgetArea);
  workspace->setFeatures(QDockWidget::DockWidgetClosable);
  addDockWidget(Qt::LeftDockWidgetArea, workspace);
  resizeDocks({workspace}, {192}, Qt::Horizontal);
  workspace->setVisible(QSettings().value("view/workspace", true).toBool());
  connect(workspace->toggleViewAction(), &QAction::triggered, this, [this](bool shown) {
    QSettings().setValue("view/workspace", shown);
  });
  formats();
  auto *bar = addToolBar("文件");
  bar->addAction(workspace->toggleViewAction());
  bar->addSeparator();
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
      bar->addSeparator();
      bar->addAction(a);
      break;
    }
  connect(tabs, &QTabWidget::currentChanged, this, [this] {
    revealActiveFile();
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

void Window::ensureTab() {
  if (!tabs->count())
    addEdit({});
}
