#include "editcommands.h"
#include "editor.h"
#include "logview.h"
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QSplitter>
#include <QSyntaxHighlighter>
#include <QTextBlock>
#include <array>
#include <functional>

void editorPreferences(Edit *e) {
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
static void highlightKeywords(QWidget *tab) {
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

static void openSplitReader(QMainWindow *window, QTabWidget *tabs) {
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
Edit *linkedPane(Edit *source) {
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

static void openSplitEditor(QMainWindow *window, Edit *source, std::function<void(Edit *)> save) {
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

void installViewCommands(QMainWindow *window, QTabWidget *tabs, std::function<void(Edit *)> save) {
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
