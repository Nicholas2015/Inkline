#pragma once
#include <QString>
#include <functional>
class QAction;
class QMainWindow;
class QMenu;
class QTabWidget;
int runSearchWorker(const QString &requestFile);
QAction *installSearchTools(
    QMainWindow *window, QMenu *menu, QTabWidget *tabs,
    std::function<void(QString)> openFile, std::function<void()> logFind,
    std::function<void(qint64, QString, int)> jumpToByte = {},
    std::function<void(QString)> openLog = {});
