#include "appearance.h"
#include "workspacefiles.h"
#include "codetools.h"
#include "configtools.h"
#include "editcommands.h"
#include "editor.h"
#include "logscan.h"
#include "logview.h"
#include "searchtools.h"
#include "sessionstore.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDockWidget>
#include <QFileSystemModel>
#include <QTreeView>
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
  QDockWidget *workspace = new QDockWidget("工作区", this);
  QTreeView *folderTree = new QTreeView;
  QFileSystemModel *folderModel = new QFileSystemModel(folderTree);
  QLabel *folderName = new QLabel;
  QWidget *emptySpace = new QWidget;
  QPushButton *chooseWorkspace = new QPushButton;
  void revealActiveFile() {
    const QString file = pathOf(tabs->currentWidget());
    const auto rootIndex = folderTree->rootIndex();
    const QString root = property("projectRoot").toString();
    if (!root.isEmpty() && !file.isEmpty() && rootIndex.isValid()) {
      const QString relative = QDir(root).relativeFilePath(file);
      if (!QDir::isAbsolutePath(relative) && relative != ".." && !relative.startsWith("../")) {
        const auto target = folderModel->index(file);
        if (target.isValid()) {
          QList<QModelIndex> parents;
          auto ancestor = target.parent();
          while (ancestor.isValid() && ancestor != rootIndex) {
            parents.prepend(ancestor);
            ancestor = ancestor.parent();
          }
          if (ancestor == rootIndex) {
            for (const auto &parent : parents) folderTree->expand(parent);
            folderTree->setCurrentIndex(target);
            folderTree->scrollTo(target, QAbstractItemView::EnsureVisible);
            return;
          }
        }
      }
    }
    folderTree->selectionModel()->clear();
  }
  void workspacePathMoved(const QString &from, const QString &to) {
    auto rebase = [&](const QString &path) { return WorkspaceFiles::relocated(path, from, to); };
    for (int i = 0; i < tabs->count(); ++i) {
      auto *tab = tabs->widget(i);
      auto old = pathOf(tab), path = rebase(old);
      if (old == path) continue;
      if (disk.files().contains(old)) disk.removePath(old);
      diskChanges.remove(old);
      if (auto *editor = dynamic_cast<Edit *>(tab)) {
        editor->doc.path = path;
        editor->applyLanguage();
        watch(path);
        tabs->setTabText(i, QFileInfo(path).fileName() + (editor->dirty() ? " *" : ""));
      } else if (auto *log = dynamic_cast<Log *>(tab)) {
        log->relocateFile(path);
        tabs->setTabText(i, QFileInfo(path).fileName());
      }
      tabs->setTabToolTip(i, QDir::toNativeSeparators(path));
    }
    QSettings settings;
    auto paths = settings.value("recentFiles").toStringList();
    for (auto &path : paths) path = rebase(path);
    paths.removeDuplicates();
    settings.setValue("recentFiles", paths);
    for (auto &entry : closed) entry.first = rebase(entry.first);
    changed();
    refresh();
    revealActiveFile();
  }
  void setWorkspaceFolder(const QString &path) {
    QFileInfo folder(path);
    const bool valid = !path.isEmpty() && folder.isDir();
    const QString root = valid ? folder.absoluteFilePath() : QString();
    setProperty("projectRoot", root);
    if (valid) {
      folderTree->setRootIndex(folderModel->setRootPath(root));
      folderName->setText(folder.fileName().isEmpty() ? QDir::toNativeSeparators(root) : folder.fileName());
    } else {
      folderName->setText("工作文件夹");
    }
    folderName->setToolTip(QDir::toNativeSeparators(root));
    folderTree->setVisible(valid);
    emptySpace->setVisible(!valid);
    chooseWorkspace->setText(valid ? "更换文件夹…" : "打开工作文件夹…");
    revealActiveFile();
  }
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
        auto *row = new QSplitter(d);
        (new QVBoxLayout(d))->addWidget(row);
        row->setChildrenCollapsible(false);
        auto addVersion = [&](const QString &title, const QString &content) {
          auto *pane = new QWidget;
          auto *column = new QVBoxLayout(pane);
          auto *label = new QLabel(title);
          label->setProperty("section", true);
          auto *view = new QPlainTextEdit(content);
          view->setFont(Appearance::codeFont());
          label->setBuddy(view);
          column->addWidget(label);
          view->setReadOnly(true);
          column->addWidget(view);
          row->addWidget(pane);
        };
        addVersion("当前编辑内容", e->text());
        addVersion("磁盘版本", Document::open(p).text);
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
    setWorkspaceFolder(state["project"].toString());
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
      revealActiveFile();
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
        int index = tabs->addTab(tab, Appearance::icon(Appearance::Symbol::Lock), QFileInfo(path).fileName());
        tabs->setTabToolTip(index, QString("只读日志  ·  %1").arg(QDir::toNativeSeparators(absolute)));
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
