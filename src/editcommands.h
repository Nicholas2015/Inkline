#pragma once
#include <functional>

class Edit;
class QMainWindow;
class QMenu;
class QTabWidget;

void editorPreferences(Edit *editor);
Edit *linkedPane(Edit *source);
void installEditCommands(QMainWindow *window, QMenu *menu, QTabWidget *tabs);
void installViewCommands(QMainWindow *window, QTabWidget *tabs, std::function<void(Edit *)> save);
