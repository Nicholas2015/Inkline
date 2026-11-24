#include <QCryptographicHash>
#include "mainwindow.h"
#include "editcommands.h"
#include "appearance.h"
#include "workspacefiles.h"
#include <QScrollBar>

void Window::revealActiveFile() {
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

void Window::workspacePathMoved(const QString &from, const QString &to) {
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

void Window::setWorkspaceFolder(const QString &path) {
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

void Window::watch(const QString &p) {
  if (!p.isEmpty() && QFileInfo::exists(p) && !disk.files().contains(p))
    disk.addPath(p);
}

void Window::inspectDisk() {
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

void Window::remember(const QString &path) {
  if (path.isEmpty())
    return;
  auto paths = QSettings().value("recentFiles").toStringList();
  paths.removeAll(path);
  paths.prepend(path);
  QSettings().setValue("recentFiles", paths.mid(0, 20));
}

void Window::replaceTab(Edit *e, const Document &doc) {
  int idx = tabs->indexOf(e);
  addEdit(doc);
  tabs->tabBar()->moveTab(tabs->currentIndex(), idx);
  tabs->removeTab(tabs->indexOf(e));
  e->deleteLater();
  changed();
  refresh();
}

bool Window::save(Edit *e, bool choose) {
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

bool Window::allowClose(QWidget *tab) {
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

void Window::addEdit(Document doc) {
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

void Window::open(const QString &path, bool log) {
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
