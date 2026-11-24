#pragma once
#include "editor.h"
#include "logview.h"
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QSplitter>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <Qsci/qscimacro.h>
#include <array>
#include <functional>

inline void editorPreferences(Edit *e) {
  QSettings s;
  e->setWhitespaceVisibility(s.value("view/spaces", false).toBool()
                                 ? QsciScintilla::WsVisible
                                 : QsciScintilla::WsInvisible);
  e->setEolVisibility(s.value("view/eol", false).toBool());
  e->setWrapMode(s.value("view/wrap", false).toBool()
                     ? QsciScintilla::WrapWord
                     : QsciScintilla::WrapNone);
  e->setIndentationsUseTabs(s.value("view/tabs", false).toBool());
  e->setTabWidth(s.value("view/width", 4).toInt());
}
inline void lineOperation(Edit *e, int op) {
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
    if (op == 1 && seen.contains(line))
      continue;
    if (op == 1)
      seen.insert(line);
    if (op == 2 && line.trimmed().isEmpty())
      continue;
    if (op == 3)
      while (line.endsWith(' ') || line.endsWith('\t'))
        line.chop(1);
    lines.append(line);
  }
  if (op == 0)
    lines.sort();
  QString result;
  for (qsizetype i = 0; i < lines.size(); ++i)
    result += lines[i] + (i + 1 == lines.size() ? endings.last() : endings[i]);
  if (result != original)
    e->replaceSelectedText(result);
}
inline void highlightKeywords(QWidget *tab) {
  auto *editor = dynamic_cast<Edit *>(tab);
  auto *log = dynamic_cast<Log *>(tab);
  if (!editor && !log)
    return;
  bool ok = false;
  const QString value = QInputDialog::getMultiLineText(
      tab, "多关键词高亮", "每行一个关键词，留空清除",
      tab->property("highlightWords").toStringList().join('\n'), &ok);
  if (!ok)
    return;
  auto words = value.split('\n', Qt::SkipEmptyParts);
  words.removeDuplicates();
  tab->setProperty("highlightWords", words);
  if (log) {
    log->setHighlightWords(words);
    return;
  }
  const auto bytes = editor->text().toUtf8();
  constexpr int firstIndicator = 12;
  for (size_t i = 0; i < LogStyle::keywordColors.size(); ++i) {
    const int id = firstIndicator + int(i);
    editor->indicatorDefine(Edit::StraightBoxIndicator, id);
    editor->setIndicatorForegroundColor(LogStyle::keywordColors[i], id);
    editor->SendScintilla(Edit::SCI_SETINDICATORCURRENT, id);
    editor->SendScintilla(Edit::SCI_INDICATORCLEARRANGE, 0, bytes.size());
  }
  for (qsizetype i = 0; i < words.size(); ++i) {
    const auto word = words[i].toUtf8();
    editor->SendScintilla(Edit::SCI_SETINDICATORCURRENT,
                          int(firstIndicator + i % LogStyle::keywordColors.size()));
    qsizetype offset = 0;
    while ((offset = bytes.indexOf(word, offset)) >= 0) {
      editor->SendScintilla(Edit::SCI_INDICATORFILLRANGE, offset, word.size());
      offset += word.size();
    }
  }
}

inline void openSplitReader(QMainWindow *window, QTabWidget *tabs) {
  if (!tabs->count())
    return;
  QDialog dialog(window);
  dialog.setWindowTitle("双栏阅读");
  dialog.resize(980, 640);
  auto *split = new QSplitter;
  (new QVBoxLayout(&dialog))->addWidget(split);
  for (int side = 0; side < 2; ++side) {
    auto *panel = new QWidget;
    split->addWidget(panel);
    auto *column = new QVBoxLayout(panel);
    auto *picker = new QComboBox;
    auto *header = new QHBoxLayout;
    auto *label = new QLabel(side ? "右侧文档" : "左侧文档");
    label->setBuddy(picker);
    header->addWidget(label);
    header->addWidget(picker, 1);
    column->addLayout(header);
    for (int i = 0; i < tabs->count(); ++i)
      picker->addItem(tabs->tabText(i));
    auto display = [column, tabs](int index) {
      if (auto *previous = column->takeAt(1)) {
        delete previous->widget();
        delete previous;
      }
      QWidget *doc = tabs->widget(index);
      if (auto *log = dynamic_cast<Log *>(doc)) {
        auto *reader = new Log(log->filePath());
        column->addWidget(reader);
        reader->restoreView(log->saveView());
      } else if (auto *editor = dynamic_cast<Edit *>(doc)) {
        auto *reader = new QPlainTextEdit;
        reader->setReadOnly(true);
        reader->setFont(editor->font());
        reader->setPlainText(editor->text());
        int row, character;
        editor->getCursorPosition(&row, &character);
        const auto block = reader->document()->findBlockByNumber(row);
        if (block.isValid())
          reader->setTextCursor(QTextCursor(block));
        column->addWidget(reader);
      }
    };
    picker->setCurrentIndex((tabs->currentIndex() + side) % tabs->count());
    display(picker->currentIndex());
    QObject::connect(picker, &QComboBox::currentIndexChanged, panel, display);
  }
  dialog.exec();
}
// Each pane has its own caret and viewport; Scintilla owns a single shared buffer and undo stack.
inline Edit *linkedPane(Edit *source) {
  Document metadata;
  metadata.path = source->doc.path;
  auto *pane = new Edit(std::move(metadata));
  if (!source->language.isEmpty()) {
    pane->language = source->language;
    pane->applyLanguage();
  }
  pane->setDocument(source->document());
  pane->setEolMode(source->eolMode());
  pane->setReadOnly(source->isReadOnly());
  editorPreferences(pane);
  int row, col;
  source->getCursorPosition(&row, &col);
  pane->setCursorPosition(row, col);
  pane->setFirstVisibleLine(source->firstVisibleLine());
  return pane;
}

inline void openSplitEditor(QMainWindow *window, Edit *source, std::function<void(Edit *)> save) {
  QDialog dialog(window);
  const auto title = source->doc.path.isEmpty() ? QString("未命名") : QFileInfo(source->doc.path).fileName();
  dialog.setWindowTitle("双栏编辑 — " + title);
  dialog.resize(1000, 640);
  auto *layout = new QVBoxLayout(&dialog);
  auto *split = new QSplitter;
  split->setChildrenCollapsible(false);
  auto *left = linkedPane(source);
  split->addWidget(left);
  split->addWidget(linkedPane(source));
  layout->addWidget(split, 1);
  auto *footer = new QHBoxLayout;
  footer->addStretch();
  auto *saveButton = new QPushButton("保存");
  saveButton->setShortcut(QKeySequence::Save);
  footer->addWidget(saveButton);
  auto *done = new QPushButton("关闭");
  footer->addWidget(done);
  layout->addLayout(footer);
  QObject::connect(saveButton, &QPushButton::clicked, &dialog, [=] { save(source); });
  QObject::connect(done, &QPushButton::clicked, &dialog, &QDialog::accept);
  QObject::connect(source, &QObject::destroyed, &dialog, &QDialog::reject);
  left->setFocus();
  dialog.exec();
}

inline void installEditCommands(QMainWindow *window, QMenu *menu,
                                QTabWidget *tabs, std::function<void(Edit *)> save) {
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
  QStringList labels = {"行排序", "删除重复行", "删除空行", "清除行尾空白"};
  for (int i = 0; i < labels.size(); ++i)
    cmd(lines, labels[i], [i](Edit *e) { lineOperation(e, i); });
  for (auto [label, msg] : {std::pair{"复制当前行", Edit::SCI_LINEDUPLICATE},
                            {"上移选中行", Edit::SCI_MOVESELECTEDLINESUP},
                            {"下移选中行", Edit::SCI_MOVESELECTEDLINESDOWN}})
    cmd(lines, label, [msg](Edit *e) { e->SendScintilla(msg); });
  cmd(lines, "列插入…", [](Edit *e) {
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
  });
  auto *marks = menu->addMenu("文档书签");
  for (int i = 0; i < 4; ++i)
    cmd(marks,
        QStringList{"切换本行书签", "下个书签", "上个书签", "清除全部书签"}[i],
        [i](Edit *e) {
          constexpr int mark = 1, mask = 1 << mark;
          int row, col;
          e->getCursorPosition(&row, &col);
          if (i == 0) {
            if (e->markersAtLine(row) & mask)
              e->markerDelete(row, mark);
            else
              e->markerAdd(row, mark);
          }
          if (i == 3)
            e->markerDeleteAll(mark);
          if (i == 1 || i == 2) {
            int t = i == 1 ? e->markerFindNext(row + 1, mask)
                           : (row ? e->markerFindPrevious(row - 1, mask) : -1);
            if (t < 0)
              t = i == 1 ? e->markerFindNext(0, mask)
                         : e->markerFindPrevious(e->lines() - 1, mask);
            if (t >= 0) {
              e->setCursorPosition(t, 0);
              e->ensureLineVisible(t);
            }
          }
        });
  const QList<QKeySequence> keys = {QKeySequence("Ctrl+F2"), QKeySequence("F2"),
                                    QKeySequence("Shift+F2")};
  for (int j = 0; j < keys.size(); ++j)
    marks->actions()[j]->setShortcut(keys[j]);
  cmd(marks, "按文字批量添加…", [](Edit *editor) {
    bool ok = false;
    const auto query = QInputDialog::getText(editor, "批量添加书签",
                                             "匹配的文字（区分大小写）",
                                             QLineEdit::Normal, {}, &ok);
    if (!ok || query.isEmpty())
      return;
    constexpr int mark = 1;
    for (int row = 0; row < editor->lines(); ++row) {
      if (!(editor->markersAtLine(row) & (1 << mark)) &&
          editor->text(row).contains(query))
        editor->markerAdd(row, mark);
    }
  });
  auto *macro = menu->addMenu("编辑宏");
  for (int i = 0; i < 3; ++i)
    cmd(macro, QStringList{"开始录制", "停止录制", "回放"}[i], [i](Edit *e) {
      auto *rec = e->findChild<QsciMacro *>();
      if (!rec) {
        rec = new QsciMacro(e);
      }
      if (i == 0) {
        rec->endRecording();
        rec->clear();
        rec->startRecording();
        e->setProperty("recording", true);
      }
      if (i != 0 && e->property("recording").toBool()) {
        rec->endRecording();
        QSettings().setValue("editor/macro", rec->save());
        e->setProperty("recording", false);
      }
      if (i == 2 && rec->load(QSettings().value("editor/macro").toString())) {
        e->beginUndoAction();
        rec->play();
        e->endUndoAction();
      }
    });
  auto *view = window->menuBar()->addMenu("视图(&V)");
  auto apply = [tabs] {
    for (int n = 0; n < tabs->count(); ++n)
      if (auto *e = dynamic_cast<Edit *>(tabs->widget(n)))
        editorPreferences(e);
  };
  for (auto [label, key] : {std::pair{"显示空白符", "view/spaces"},
                            {"显示行尾符", "view/eol"},
                            {"自动换行", "view/wrap"},
                            {"使用 Tab 缩进", "view/tabs"}}) {
    auto *a = view->addAction(label);
    a->setCheckable(true);
    a->setChecked(QSettings().value(key, false).toBool());
    QObject::connect(a, &QAction::toggled, window, [=](bool on) {
      QSettings().setValue(key, on);
      apply();
    });
  }
  auto *indent = view->addMenu("缩进宽度");
  for (int n : {2, 4, 8})
    indent->addAction(QString::number(n), window, [=] {
      QSettings().setValue("view/width", n);
      apply();
    });
  view->addAction("多关键词高亮…", window,
                  [tabs] { highlightKeywords(tabs->currentWidget()); });
  auto *splitEdit = view->addAction("双栏编辑当前文档…", window, [=] {
    if (auto *e = dynamic_cast<Edit *>(tabs->currentWidget()); e && !e->isReadOnly())
      openSplitEditor(window, e, save);
  });
  QObject::connect(view, &QMenu::aboutToShow, window, [=] {
    auto *e = dynamic_cast<Edit *>(tabs->currentWidget());
    splitEdit->setEnabled(e && !e->isReadOnly());
  });
  view->addAction("双栏阅读…", window, [=] { openSplitReader(window, tabs); });
}
