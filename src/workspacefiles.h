#pragma once
#include <QApplication>
#include <QClipboard>
#include <QFileSystemModel>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QTreeView>
#include <functional>

namespace WorkspaceFiles {
inline bool validName(const QString &name) {
  return !name.trimmed().isEmpty() && name != "." && name != ".." &&
         !name.contains('/') && !name.contains('\\') && !QDir::isAbsolutePath(name)
#ifdef Q_OS_WIN
         && !name.contains(':') && !name.endsWith('.') && !name.endsWith(' ')
#endif
         ;
}

// Rebase paths by component so renaming "api" never captures "api-test".
inline QString relocated(const QString &path, const QString &from, const QString &to) {
  if (path.isEmpty()) return path;
  const QString relative = QDir(from).relativeFilePath(path);
  if (relative == ".") return to;
  if (relative == ".." || relative.startsWith("../") || QDir::isAbsolutePath(relative)) return path;
  return QDir::cleanPath(QDir(to).filePath(relative));
}

inline void install(QTreeView *tree, QFileSystemModel *model,
                    std::function<void(QString)> open,
                    std::function<void(QString, QString)> moved) {
  tree->setContextMenuPolicy(Qt::CustomContextMenu);
  QObject::connect(tree, &QTreeView::customContextMenuRequested, tree,
      [=](const QPoint &pos) {
    if (!tree->rootIndex().isValid()) return;
    const auto index = tree->indexAt(pos);
    const auto root = model->filePath(tree->rootIndex());
    const auto path = index.isValid() ? model->filePath(index) : root;
    const QFileInfo info(path);
    const QDir parent(info.isDir() ? path : info.absolutePath());
    QMenu menu(tree);
    auto *file = menu.addAction("新建文件…");
    auto *folder = menu.addAction("新建文件夹…");
    auto *rename = menu.addAction("重命名…");
    rename->setEnabled(index.isValid());
    menu.addSeparator();
    auto *copy = menu.addAction("复制相对路径");
    const auto *picked = menu.exec(tree->viewport()->mapToGlobal(pos));
    if (!picked) return;
    if (picked == copy) {
      QApplication::clipboard()->setText(QDir(root).relativeFilePath(path));
      return;
    }
    auto base = picked == rename ? info.dir() : parent;
    const auto name = QInputDialog::getText(tree, picked->text(), "名称", QLineEdit::Normal,
                                            picked == rename ? info.fileName() : QString());
    if (name.isEmpty()) return;
    if (!validName(name)) {
      QMessageBox::warning(tree, "名称无效", "请输入单个文件或文件夹名称。");
      return;
    }
    const QString dest = base.absoluteFilePath(name);
    if (picked == rename && path == dest) return;
    if (QFileInfo::exists(dest) || QFileInfo(dest).isSymLink()) {
      QMessageBox::warning(tree, "名称已占用", name);
      return;
    }
    if (picked == rename) {
      if (base.rename(info.fileName(), name)) moved(path, dest);
      else QMessageBox::warning(tree, "重命名失败", path);
    } else if (picked == folder) {
      if (!base.mkdir(name)) QMessageBox::warning(tree, "新建文件夹失败", dest);
      else if (index.isValid() && info.isDir()) tree->expand(index);
    } else if (picked == file) {
      QFile created(dest);
      if (!created.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        QMessageBox::warning(tree, "新建文件失败", created.errorString());
        return;
      }
      created.close();
      open(dest);
    }
  });
}
} // namespace WorkspaceFiles
