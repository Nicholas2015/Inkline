#pragma once

#include <QString>
#include <functional>

class QAction;
class QMainWindow;
class QTabWidget;

QAction *installCodeTools(QMainWindow *window, QTabWidget *tabs,
                      std::function<void(QString)> openFile);
