#include "numberedlogpage.h"
#include "logstyle.h"
#include "logscan.h"
#include "logview.h"
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QLineEdit>
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
  auto *numbered=dynamic_cast<NumberedLogPage*>(page);
  if(!numbered)return 20;
  w.gotoByte(row.size()*15942);
  if(!wait([&]{return numbered->firstFileLine()==15943;}))return 21;
  auto *wrap=w.findChild<QCheckBox*>("logWrap");
  if(!wrap)return 24;
  wrap->setChecked(true);
  if(numbered->firstFileLine()!=15943 || numbered->lineWrapMode()!=QPlainTextEdit::WidgetWidth)return 25;
  QFile styled(tmp.filePath("styled.log"));
  if(!styled.open(QIODevice::WriteOnly))return 22;
  styled.write("2026-11-24 16:13:18 | sidecar | WARN | server.auth:event:162 | id=22\r\n");styled.close();
  Log formatted(styled.fileName());formatted.setHighlightWords({"WARN"});
  auto *styledPage=formatted.findChild<QPlainTextEdit*>("logPage");
  auto block=styledPage->document()->firstBlock();auto index=block.text().indexOf("WARN");bool combined=false;
  for(const auto &fmt:block.layout()->formats())
    if(fmt.start==index && fmt.length==4 && fmt.format.foreground().color()==LogStyle::ink(LogStyle::Warn) && fmt.format.background().color()==LogStyle::keywordColors[0])combined=true;
  if(!combined)return 23;
  QFile events(tmp.filePath("events.log"));
  if (!events.open(QIODevice::WriteOnly)) return 26;
  const QByteArray header = "2026-11-24 23:59:10 | sidecar | ERROR | trace_id=abc request_id=req session_id=42\n";
  const QByteArray stack = "Traceback:\n  frame one\n\n  frame two\n";
  events.write(header + stack);
  events.write("2026-11-24 23:59:11 | sidecar | ERROR | trace_id=abcd request_id=req session_id=42\n");
  events.close();
  Log eventView(events.fileName());
  eventView.showSearch();
  auto *eventDialog = eventView.findChild<QDialog *>();
  auto *eventList = eventDialog->findChild<QListWidget *>();
  QComboBox *eventTerms = nullptr;
  for (auto *combo : eventDialog->findChildren<QComboBox *>()) {
    if (combo->isEditable())
      eventTerms = combo;
    else
      combo->setCurrentIndex(1);
  }
  auto eventClick = [&](QString label) {
    for (auto *btn : eventDialog->findChildren<QPushButton *>())
      if (btn->text() == label && btn->isEnabled()) {
        btn->click();
        return true;
      }
    return false;
  };
  const QMap<QString, QString> criteria{{"level","error"},{"source","sidecar"},
      {"trace_id","abc"},{"request_id","req"},{"session_id","42"}};
  for (auto i = criteria.begin(); i != criteria.end(); ++i)
    eventDialog->findChild<QLineEdit *>("logFilter_" + i.key())->setText(i.value());
  if (!eventClick("查找") || !wait([&] { return eventList->count() == 1; })) return 27;
  auto *eventFollow = eventView.findChild<QToolButton *>("followTail");
  eventFollow->click();
  if (!events.open(QIODevice::Append)) return 28;
  events.write(header);
  events.close();
  if (!wait([&] { return eventList->count() == 2; })) return 29;
  eventFollow->click();
  for (auto i = criteria.begin(); i != criteria.end(); ++i)
    eventDialog->findChild<QLineEdit *>("logFilter_" + i.key())->clear();
  eventTerms->setEditText("frame two");
  if (!eventClick("查找") || !wait([&] { return eventList->count() == 1; })) return 30;
  eventList->setCurrentRow(0);
  if (!eventClick("复制完整事件") || !wait([&] {
      return QApplication::clipboard()->text() == QString::fromUtf8(header + stack);
    })) return 31;
  if (!eventClick("展开事件") || !wait([&] {
      auto *eventText = eventDialog->findChild<QPlainTextEdit *>("logEventText");
      return eventText && eventText->toPlainText() == QString::fromUtf8(header + stack);
    })) return 32;
  QFile dated(tmp.filePath("dated.log"));
  if (!dated.open(QIODevice::WriteOnly)) return 33;
  dated.write("23:59:59 undated\n2026-11-23 23:59:59 last night\n");
  const auto jsonOffset = dated.pos();
  dated.write("{\"payload\":{\"timestamp\":\"2099-01-01T00:00:00\"},\"message\":\"");
  dated.write(QByteArray(300000,'x'));
  dated.write("\",\"timestamp\":\"2026-11-24T01:00:00.110Z\"}\n");
  const auto expected = dated.pos();
  dated.write("2026-11-26 01:00:00.000 first qualifying row\n");
  dated.write("2026-11-24 01:00:00.111 exact match appears later\n");
  dated.close();
  Log byDate(dated.fileName());
  auto *dateStatus = byDate.findChild<QLabel *>("logPosition");
  byDate.gotoTimestamp("2026-11-24 01:00:00.111");
  if (!wait([&] { return byDate.saveView()["offset"].toInteger() == expected; })) return 34;
  byDate.gotoTimestamp("2026-11-24 01:00:00.110");
  if (!wait([&] { return byDate.saveView()["offset"].toInteger() == jsonOffset; })) return 35;
  byDate.gotoTimestamp("2099-01-01 00:00:00");
  if (!wait([&] { return dateStatus->text().contains("没有找到"); })) return 36;
  byDate.gotoTimestamp("2026-02-30 00:00:00");
  if (!dateStatus->text().contains("请输入")) return 37;
  byDate.gotoTimestamp("2099-01-01 00:00:00");
  for (auto *btn : byDate.findChildren<QPushButton *>())
    if (btn->text() == "取消定位") btn->click();
  if (!dateStatus->text().contains("已取消")) return 38;
  puts("Native log UI navigation, following, search, continuation, selection "
       "and cancellation passed.");
}

