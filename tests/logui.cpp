#include "logscan.h"
#include "logview.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <QScrollBar>
#include <QTextBlock>
#include <QWheelEvent>
#include <QToolButton>
#include <cstdio>
int main(int argc, char **argv) {
  if (argc == 2 && QByteArray(argv[1]) == "--log-job")
    return runLogJob();
  QApplication app(argc, argv);
  QCoreApplication::setApplicationName("Inkline Log Regression");
  QTemporaryDir tmp;
  QString name = tmp.filePath("ui.log");
  QFile f(name);
  if (!f.open(QIODevice::WriteOnly))
    return 1;
  auto row = QString("第一行 abc123 00000\r\n").toUtf8();
  for (int i = 0; i < 20000; ++i)
    f.write(QString("第一行 abc123 %1\r\n").arg(i, 5, 10, QChar('0')).toUtf8());
  f.close();
  Log w(name);
  w.resize(800, 500);
  w.show();
  auto *page = w.findChild<QPlainTextEdit *>("logPage");
  auto *follow = w.findChild<QToolButton *>("followTail");
  auto *list = w.findChild<QListWidget *>("logMatches");
  auto click = [&](QString name) {
    for (auto *btn : w.findChildren<QPushButton *>())
      if (btn->text() == name) {
        btn->click();
        return true;
      }
    return false;
  };
  auto wait = [&](auto predicate) {
    QElapsedTimer t;
    t.start();
    while (!predicate() && t.elapsed() < 5000)
      QTest::qWait(10);
    return predicate();
  };
  if (!page || !follow || !list || click("下一段") || click("上一段")) return 2;
  auto wheel = [](QPlainTextEdit *view, int delta) {
    const QPoint local = view->viewport()->rect().center();
    QWheelEvent event(local, view->viewport()->mapToGlobal(local), {}, QPoint(0,delta),
                      Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(view->viewport(), &event);
  };
  auto visibleLine = [&] {
    return page->cursorForPosition(QPoint(1,1)).block().text().right(5).toInt();
  };
  auto *bar = page->verticalScrollBar();
  QTest::qWait(15);
  bar->setValue(bar->maximum());
  const int beforeDown = visibleLine();
  wheel(page,-120);
  const int afterDown = visibleLine();
  if (w.saveView()["offset"].toInteger() <= 0 || afterDown <= beforeDown ||
      afterDown-beforeDown > 12) {
    qWarning("Downward overlap discontinuity: %d -> %d", beforeDown,afterDown);
    return 12;
  }
  bar->setValue(bar->minimum());
  const int beforeUp = visibleLine();
  wheel(page,120);
  const int afterUp = visibleLine();
  if (w.saveView()["offset"].toInteger() != 0 || afterUp >= beforeUp ||
      beforeUp-afterUp > 12) {
    qWarning("Upward overlap discontinuity: %d -> %d", beforeUp,afterUp);
    return 13;
  }
  QFile sparse(tmp.filePath("long.log"));
  if (!sparse.open(QIODevice::WriteOnly)) return 14;
  sparse.write(QByteArray(350000,'a')); sparse.close();
  Log huge(sparse.fileName()); huge.resize(500,300); huge.show();
  auto *longPage = huge.findChild<QPlainTextEdit *>("logPage");
  QTest::qWait(15);
  QKeyEvent down(QEvent::KeyPress,Qt::Key_PageDown,Qt::NoModifier); QApplication::sendEvent(longPage,&down);
  if (huge.saveView()["offset"].toInteger() <= 0 || longPage->toPlainText().size()>128*1024) return 15;
  QKeyEvent up(QEvent::KeyPress,Qt::Key_PageUp,Qt::NoModifier); QApplication::sendEvent(longPage,&up);
  if (huge.saveView()["offset"].toInteger()!=0) return 16;
  w.gotoSearchHit(10, "UTF-8", 3);
  if (page->textCursor().selectedText() != "abc")
    return 3;
  auto saved = w.saveView();
  w.gotoByte(0);
  w.restoreView(saved);
  if (page->textCursor().selectedText() != "abc" ||
      w.saveView()["encoding"] != "UTF-8")
    return 11;
  w.gotoLine(15000);
  if (!wait([&] {
        return w.findChild<QLabel *>("logPosition")
            ->text()
            .contains(QString::number(row.size() * 14999));
      }))
    return 4;
  follow->click();
  if (!f.open(QIODevice::Append))
    return 5;
  f.write("tail marker");
  f.close();
  if (!wait([&] { return page->toPlainText().endsWith("tail marker"); }))
    return 6;
  wheel(page,120);
  if (follow->isChecked()) return 17;
  w.showSearch();
  auto *dialog = w.findChild<QDialog *>();
  QComboBox *terms = nullptr, *mode = nullptr;
  for (auto *combo : dialog->findChildren<QComboBox *>()) {
    if (combo->isEditable())
      terms = combo;
    else
      mode = combo;
  }
  mode->setCurrentIndex(1);
  terms->setEditText("abc[0-9]+");
  for (auto *box : dialog->findChildren<QCheckBox *>())
    if (box->text() == "正则")
      box->setChecked(true);
  click("查找");
  if (!wait([&] { return list->count() == 200; }))
    return 7;
  list->setCurrentRow(0);
  if (page->textCursor().selectedText() != "abc123")
    return 8;
  click("继续扫描");
  if (!wait([&] {
        return list->count() == 200 &&
               list->item(0)->data(Qt::UserRole).toLongLong() > 200 * 10;
      }))
    return 9;
  terms->setEditText("none");
  click("查找");
  click("取消");
  QTest::qWait(100);
  if (list->count())
    return 10;
  puts("Native log UI navigation, following, search, continuation, selection "
       "and cancellation passed.");
}

