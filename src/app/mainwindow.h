#pragma once
#include "document.h"
#include "sessionstore.h"
#include <QDockWidget>
#include <QFileSystemModel>
#include <QFileSystemWatcher>
#include <QFuture>
#include <QLabel>
#include <QMainWindow>
#include <QPushButton>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>

class Edit;
class Window final : public QMainWindow {
public:
  Window();
  ~Window() override;
  void open(const QString &path, bool log = false);
  void startSession(const QString &dir = {});
  void ensureTab();

protected:
  void closeEvent(QCloseEvent *ev) override;

private:
  void installMenusAndToolbar();
  void installFileMenu(QToolBar *bar);
  QAction *installEditorMenu();
  void processConfiguration(bool pretty);
  void setupWorkspace();
  void revealActiveFile();
  void workspacePathMoved(const QString &from, const QString &to);
  void setWorkspaceFolder(const QString &path);
  void watch(const QString &p);
  void inspectDisk();
  static QString pathOf(QWidget *w);
  void remember(const QString &path);
  void changed();
  bool checkpoint(bool asynchronous = false);
  void restore();
  void formats();
  void replaceTab(Edit *e, const Document &doc);
  Edit *edit() const;
  void refresh();
  bool save(Edit *e, bool choose);
  bool allowClose(QWidget *tab);
  void addEdit(Document doc);

  QTabWidget *tabs = new QTabWidget;
  QDockWidget *workspace = new QDockWidget("工作区", this);
  QTreeView *folderTree = new QTreeView;
  QFileSystemModel *folderModel = new QFileSystemModel(folderTree);
  QLabel *folderName = new QLabel;
  QWidget *emptySpace = new QWidget;
  QPushButton *chooseWorkspace = new QPushButton;
  QAction *saveAction, *asAction;
  SessionStore sessions;
  QTimer snapshot;
  QFileSystemWatcher disk;
  QSet<QString> diskChanges;
  bool checkingDisk = false;
  bool pending = false, restoring = true;
  QList<QPair<QString, bool>> closed;
  QMenu *recent = nullptr;
  QToolButton *codecButton = new QToolButton, *eolButton = new QToolButton,
              *languageButton = new QToolButton;
  QFuture<bool> writing;

};
