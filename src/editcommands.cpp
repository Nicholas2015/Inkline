#include "editcommands.h"
#include "editor.h"
#include <Qsci/qscimacro.h>
#include <QSettings>

namespace {
enum class LineOperation { Sort, Deduplicate, DropEmpty, TrimTrailing };
enum class MacroCommand { Record, Stop, Play };
constexpr int bookmark = 1;
constexpr int bookmarkMask = 1 << bookmark;

void lineOperation(Edit *e, LineOperation op) {
  int first = 0, startCol = 0, last = e->lines() - 1, endCol = 0;
  if (e->hasSelectedText()) {
    e->getSelection(&first, &startCol, &last, &endCol);
    if (!endCol && last > first)
      --last;
  }
  if (last + 1 < e->lines())
    e->setSelection(first, 0, last + 1, 0);
  else
    e->setSelection(first, 0, last, e->text(last).size());
  const auto original = e->selectedText();
  QStringList lines, endings;
  QSet<QString> seen;
  static const QRegularExpression parts("([^\\r\\n]*)(\\r\\n|\\r|\\n|$)");
  auto iter = parts.globalMatch(original);
  while (iter.hasNext()) {
    auto match = iter.next();
    if (!match.capturedLength())
      break;
    auto line = match.captured(1);
    endings.append(match.captured(2));
    if (op == LineOperation::Deduplicate && seen.contains(line))
      continue;
    if (op == LineOperation::Deduplicate)
      seen.insert(line);
    if (op == LineOperation::DropEmpty && line.trimmed().isEmpty())
      continue;
    if (op == LineOperation::TrimTrailing)
      while (line.endsWith(' ') || line.endsWith('\t'))
        line.chop(1);
    lines.append(line);
  }
  if (op == LineOperation::Sort)
    lines.sort();
  QString result;
  for (qsizetype i = 0; i < lines.size(); ++i)
    result += lines[i] + (i + 1 == lines.size() ? endings.last() : endings[i]);
  if (result != original)
    e->replaceSelectedText(result);
}

void insertColumn(Edit *e) {
  int a, c, b, d;
  if (e->hasSelectedText())
    e->getSelection(&a, &c, &b, &d);
  else {
    e->getCursorPosition(&a, &c);
    b = a;
    d = c;
  }
  bool ok = false;
  auto text = QInputDialog::getText(e, "列插入", "插入内容",
                                    QLineEdit::Normal, {}, &ok);
  if (!ok || text.isEmpty())
    return;
  const int col = qMin(c, d);
  e->beginUndoAction();
  for (int row = a; row <= b; ++row) {
    auto line = e->text(row);
    while (line.endsWith('\r') || line.endsWith('\n'))
      line.chop(1);
    e->insertAt(QString(qMax<qsizetype>(0, col - line.size()), ' ') + text,
                row, qMin<int>(col, line.size()));
  }
  e->endUndoAction();
}

void toggleBookmark(Edit *e) {
  int row, column;
  e->getCursorPosition(&row, &column);
  if (e->markersAtLine(row) & bookmarkMask)
    e->markerDelete(row, bookmark);
  else
    e->markerAdd(row, bookmark);
}

void moveToBookmark(Edit *e, bool forward) {
  int row, column;
  e->getCursorPosition(&row, &column);
  int target = forward ? e->markerFindNext(row + 1, bookmarkMask)
                       : (row ? e->markerFindPrevious(row - 1, bookmarkMask) : -1);
  if (target < 0)
    target = forward ? e->markerFindNext(0, bookmarkMask)
                     : e->markerFindPrevious(e->lines() - 1, bookmarkMask);
  if (target >= 0) {
    e->setCursorPosition(target, 0);
    e->ensureLineVisible(target);
  }
}

void markMatchingLines(Edit *editor) {
  bool ok = false;
  const auto query = QInputDialog::getText(editor, "批量添加书签",
                                           "匹配的文字（区分大小写）",
                                           QLineEdit::Normal, {}, &ok);
  if (!ok || query.isEmpty())
    return;
  for (int row = 0; row < editor->lines(); ++row) {
    if (!(editor->markersAtLine(row) & bookmarkMask) &&
        editor->text(row).contains(query))
      editor->markerAdd(row, bookmark);
  }
}

void runMacro(Edit *e, MacroCommand cmd) {
  auto *rec = e->findChild<QsciMacro *>();
  if (!rec) {
    rec = new QsciMacro(e);
  }
  if (cmd == MacroCommand::Record) {
    rec->endRecording();
    rec->clear();
    rec->startRecording();
    e->setProperty("recording", true);
  }
  if (cmd != MacroCommand::Record && e->property("recording").toBool()) {
    rec->endRecording();
    QSettings().setValue("editor/macro", rec->save());
    e->setProperty("recording", false);
  }
  if (cmd == MacroCommand::Play && rec->load(QSettings().value("editor/macro").toString())) {
    e->beginUndoAction();
    rec->play();
    e->endUndoAction();
  }
}
} // namespace

void installEditCommands(QMainWindow *window, QMenu *menu, QTabWidget *tabs) {
  auto current = [tabs] { return dynamic_cast<Edit *>(tabs->currentWidget()); };
  auto cmd = [=](QMenu *m, const QString &label, std::function<void(Edit *)> fn,
                 QKeySequence key = {}) {
    auto *act = m->addAction(label, key, window, [=] {
      if (auto *e = current())
        fn(e);
    });
    QObject::connect(m, &QMenu::aboutToShow, act,
                     [=] { act->setEnabled(current() != nullptr); });
    return act;
  };
  auto *lines = menu->addMenu("行操作");
  for (auto [label, op] : {std::pair{"行排序", LineOperation::Sort},
                           {"删除重复行", LineOperation::Deduplicate},
                           {"删除空行", LineOperation::DropEmpty},
                           {"清除行尾空白", LineOperation::TrimTrailing}})
    cmd(lines, label, [op](Edit *e) { lineOperation(e, op); });
  for (auto [label, msg] : {std::pair{"复制当前行", Edit::SCI_LINEDUPLICATE},
                            {"上移选中行", Edit::SCI_MOVESELECTEDLINESUP},
                            {"下移选中行", Edit::SCI_MOVESELECTEDLINESDOWN}})
    cmd(lines, label, [msg](Edit *e) { e->SendScintilla(msg); });
  cmd(lines, "列插入…", insertColumn);

  auto *marks = menu->addMenu("文档书签");
  cmd(marks, "切换本行书签", toggleBookmark, QKeySequence("Ctrl+F2"));
  cmd(marks, "下个书签", [](Edit *e) { moveToBookmark(e, true); }, QKeySequence("F2"));
  cmd(marks, "上个书签", [](Edit *e) { moveToBookmark(e, false); }, QKeySequence("Shift+F2"));
  cmd(marks, "清除全部书签", [](Edit *e) { e->markerDeleteAll(bookmark); });
  cmd(marks, "按文字批量添加…", markMatchingLines);

  auto *macro = menu->addMenu("编辑宏");
  for (auto [label, command] : {std::pair{"开始录制", MacroCommand::Record},
                                {"停止录制", MacroCommand::Stop}, {"回放", MacroCommand::Play}})
    cmd(macro, label, [command](Edit *e) { runMacro(e, command); });
}
