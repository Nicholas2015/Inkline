#include "mainwindow.h"
#include "appearance.h"
#include "codetools.h"
#include "configtools.h"
#include "editcommands.h"
#include "editor.h"
#include "logview.h"
#include "searchtools.h"
#include <QToolBar>
#include <QSettings>

namespace {
void addToolAction(QToolBar *bar, QAction *action, Appearance::Symbol icon) {
  action->setIcon(Appearance::icon(icon));
  bar->addAction(action);
}
}

void Window::installMenusAndToolbar() {
  auto *bar = addToolBar("文件");
  bar->setMovable(false);
  bar->setIconSize(QSize(16, 16));
  bar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  bar->addAction(workspace->toggleViewAction());
  bar->addSeparator();
  installFileMenu(bar);
  auto *find = installEditorMenu();
  installViewCommands(this, tabs, [this](Edit *doc) { save(doc, false); });
  auto *compose = installCodeTools(this, tabs, [this](QString path) { open(path); });
  bar->addSeparator();
  addToolAction(bar, find, Appearance::Symbol::Search);
  bar->addSeparator();
  addToolAction(bar, compose, Appearance::Symbol::Suggest);
}

void Window::installFileMenu(QToolBar *bar) {
  auto *menu = menuBar()->addMenu("文件(&F)");
  auto *create = menu->addAction("新建", QKeySequence::New, this, [this] { addEdit({}); });
  auto *openFile = menu->addAction("打开…", QKeySequence::Open, this, [this] {
    open(QFileDialog::getOpenFileName(this, "打开文件"));
  });
  menu->addAction("只读打开日志…", this, [this] {
    open(QFileDialog::getOpenFileName(this, "打开日志"), true);
  });
  saveAction = menu->addAction("保存", QKeySequence::Save, this,
                            [this] { save(edit(), false); });
  asAction = menu->addAction("另存为…", QKeySequence::SaveAs, this,
                          [this] { save(edit(), true); });
  menu->addAction("全部保存", this, [this] {
    for (int i = 0; i < tabs->count(); ++i)
      if (auto *e = dynamic_cast<Edit *>(tabs->widget(i)); e && e->dirty()) {
        tabs->setCurrentWidget(e);
        if (!save(e, false))
          break;
      }
  });
  recent = menu->addMenu("最近文件");
  connect(recent, &QMenu::aboutToShow, this, [this] {
    recent->clear();
    for (const auto &p : QSettings().value("recentFiles").toStringList())
      recent->addAction(p, this, [this, p] { open(p); });
  });
  menu->addAction("重新打开关闭的标签", QKeySequence("Ctrl+Shift+T"), this,
               [this] {
                 if (!closed.isEmpty()) {
                   auto entry = closed.takeLast();
                   if (entry.first.isEmpty())
                     addEdit({});
                   else
                     open(entry.first, entry.second);
                 }
               });
  auto *chooseFolder = menu->addAction("选择工作文件夹…", this, [this] {
    auto dir = QFileDialog::getExistingDirectory(
        this, "工作文件夹", property("projectRoot").toString());
    if (!dir.isEmpty()) {
      setWorkspaceFolder(dir);
      changed();
    }
  });
  menu->addAction("关闭标签页", QKeySequence::Close, this, [this] {
    if (tabs->currentIndex() >= 0)
      emit tabs->tabCloseRequested(tabs->currentIndex());
  });
  menu->addAction("退出", QKeySequence::Quit, this, &QWidget::close);
  connect(chooseWorkspace, &QPushButton::clicked, chooseFolder, &QAction::trigger);
  addToolAction(bar, create, Appearance::Symbol::New);
  addToolAction(bar, openFile, Appearance::Symbol::Open);
  addToolAction(bar, saveAction, Appearance::Symbol::Save);
}

QAction *Window::installEditorMenu() {
  auto *menu = menuBar()->addMenu("编辑(&E)");
  auto basic = [&](const QString &title, auto method) {
    menu->addAction(title, this, [this, method] {
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
  auto *find = installSearchTools(
      this, menu, tabs, openPath,
      [this] {
        if (auto *l = dynamic_cast<Log *>(tabs->currentWidget()))
          l->showSearch();
      },
      [this](qint64 n, QString c, int length) {
        if (auto *l = dynamic_cast<Log *>(tabs->currentWidget()))
          l->gotoSearchHit(n, c, length);
      },
      [this](QString p) { open(p, true); });
  menu->addAction("跳转到位置…", QKeySequence("Ctrl+G"), this, [this] {
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
  menu->addAction("本地补全", QKeySequence("Ctrl+Space"), this, [this] {
    if (auto *e = edit())
      e->autoCompleteFromAll();
  });
  auto configAction = [&](const QString &title, const QString &id, bool pretty) {
    auto *action = menu->addAction(title, this, [this, pretty] {
      processConfiguration(pretty);
    });
    action->setObjectName(id);
  };
  configAction("校验配置语法", "validateConfig", false);
  configAction("格式化配置", "formatConfig", true);
  installEditCommands(this, menu, tabs);
  return find;
}

void Window::processConfiguration(bool pretty) {
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
}
