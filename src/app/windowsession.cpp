#include "mainwindow.h"
#include "editor.h"
#include "logview.h"
#include "appearance.h"
#include <QScrollBar>

void Window::changed() {
  if (!restoring)
    pending = true;
}

bool Window::checkpoint(bool asynchronous) {
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

void Window::restore() {
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

void Window::closeEvent(QCloseEvent *ev) {
  if (!checkpoint()) {
    for (int i = 0; i < tabs->count(); ++i)
      if (!allowClose(tabs->widget(i))) {
        ev->ignore();
        return;
      }
  }
  ev->accept();
}

Window::~Window() { writing.waitForFinished(); }

void Window::startSession(const QString &dir) {
  sessions.directory = dir;
  restore();
}
