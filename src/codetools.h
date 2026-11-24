#pragma once

#include <QString>
#include <functional>

class QMainWindow;
class QTabWidget;

void installCodeTools(QMainWindow *window, QTabWidget *tabs,
                      std::function<void(QString)> openFile);
