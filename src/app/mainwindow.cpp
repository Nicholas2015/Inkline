#include "mainwindow.h"
#include "appearance.h"
#include "workspacefiles.h"
#include "editor.h"
#include "logview.h"
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
  installMenusAndToolbar();
  setupWorkspace();
  formats();
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

void Window::setupWorkspace() {
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
}
