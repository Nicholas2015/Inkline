#include "codetools.h"
#include "modelproviders.h"
#include <QListWidget>
#include "appearance.h"
#include <Qsci/qsciscintilla.h>
#include <Qsci/qscilexer.h>
#include <Qsci/qscistyle.h>
#include <QtNetwork>
#include <QtWidgets>

using Editor = QsciScintilla;
void installLanguageTools(QMainWindow *, QTabWidget *,
                          std::function<void(QString)>);

namespace {
using Ai::endpoint;
using Ai::model;
using Ai::ModelClient;

QString contextBefore(Editor *e, int limit) {
  long pos = e->SendScintilla(Editor::SCI_GETCURRENTPOS);
  long start = qMax(0L, pos - 4L * limit);
  QByteArray bytes(pos - start + 1, '\0');
  e->SendScintilla(Editor::SCI_GETTEXTRANGE, start, pos, bytes.data());
  return QString::fromUtf8(bytes.constData(), pos - start).right(limit);
}
void replace(Editor *e, QString value) {
  value.replace("\r\n", "\n").replace('\r', '\n');
  if (e->eolMode() == Editor::EolWindows)
    value.replace("\n", "\r\n");
  if (e->eolMode() == Editor::EolMac)
    value.replace('\n', '\r');
  e->SendScintilla(Editor::SCI_REPLACESEL, value.toUtf8().constData());
}

class Inline : public QObject {
  Editor *e;
  ModelClient *client;
  QTimer delay;
  QLabel *hint;
  QString suggestion;
  long anchor = -1;
  int marker, handle = -1;
  bool accepting = false;
  quint64 generation = 0;
  QPointer<QNetworkReply> pending;

  bool eligible() const {
    const auto dest = endpoint();
    return !dest.isEmpty() && QSettings().value("ai/inlineEndpoint").toString() == dest &&
           e->lexer() && QString::fromUtf8(e->lexer()->language()) != "日志" && !e->isReadOnly();
  }
  void clear() {
    ++generation;
    delay.stop();
    suggestion.clear();
    hint->hide();
    if (handle >= 0) {
      const int row = e->markerLine(handle);
      if (row >= 0) e->clearAnnotations(row);
      e->markerDeleteHandle(handle);
      handle = -1;
    }
    if (pending && pending->isRunning()) pending->abort();
    pending.clear();
    anchor = -1;
  }
  void showSuggestion(QString text, long pos) {
    if (text.isEmpty()) return;
    const long row = e->SendScintilla(Editor::SCI_LINEFROMPOSITION, pos);
    if (text.contains('\n') || pos != e->SendScintilla(Editor::SCI_GETLINEENDPOSITION, row)) {
      // Virtual annotation rows take up display space without covering source
      // text, modifying the buffer, or putting anything on the undo stack.
      if (marker < 0 || e->SendScintilla(Editor::SCI_ANNOTATIONGETTEXT, row) > 0) return;
      handle = e->markerAdd(row, marker);
      if (handle < 0) return;
      QsciStyle gray(244, "Inline completion", QColor("#616b72"), e->lexer()->defaultPaper(),
                     e->lexer()->defaultFont());
      e->annotate(row, text, gray);
    } else {
      hint->setFont(e->lexer()->defaultFont());
      hint->setText(text);
      hint->adjustSize();
      hint->move(e->SendScintilla(Editor::SCI_POINTXFROMPOSITION, 0UL, pos),
                 e->SendScintilla(Editor::SCI_POINTYFROMPOSITION, 0UL, pos));
      hint->show();
    }
    suggestion = std::move(text);
    anchor = pos;
    e->SendScintilla(Editor::SCI_AUTOCCANCEL);
  }
  void request() {
    if (!eligible() || !e->hasFocus() || e->hasSelectedText()) return;
    long pos = e->SendScintilla(Editor::SCI_GETCURRENTPOS);
    const long end = qMin(e->SendScintilla(Editor::SCI_GETLENGTH), pos + 4096L);
    QByteArray bytes(end - pos + 1, '\0');
    e->SendScintilla(Editor::SCI_GETTEXTRANGE, pos, end, bytes.data());
    auto right = QString::fromUtf8(bytes.constData(), end - pos).left(1024);
    if (!right.isEmpty() && right.back().isHighSurrogate()) right.chop(1);
    const bool shareRight = QSettings().value("ai/inlineContextEndpoint").toString() == endpoint();
    const auto suffix = shareRight ? right : QString();
    auto prefix = contextBefore(e, 4096 - suffix.size());
    if (!prefix.isEmpty() && prefix.front().isLowSurrogate()) prefix.remove(0, 1);
    const auto context = QJsonDocument(QJsonObject{{"prefix", prefix}, {"suffix", suffix}})
                             .toJson(QJsonDocument::Compact);
    const auto token = generation;
    pending = client->ask(this,
        QString("当前语言：%1。只返回应插入 prefix 与 suffix 之间的代码，可以包含多行；不要重复已有内容。")
            .arg(QString::fromUtf8(e->lexer()->language())), QString::fromUtf8(context),
        [this, token, pos, right](QString answer, QString err) {
          if (token != generation || !eligible() || !err.isEmpty() || !e->hasFocus() ||
              e->hasSelectedText() || pos != e->SendScintilla(Editor::SCI_GETCURRENTPOS)) return;
          answer.replace("\r\n", "\n").replace('\r', '\n');
          QString suffix = right;
          suffix.replace("\r\n", "\n").replace('\r', '\n');
          for (auto n = qMin(suffix.size(), answer.size()); n > 0; --n) {
            if (QStringView(answer).endsWith(QStringView(suffix).left(n))) {
              answer.chop(n);
              break;
            }
          }
          showSuggestion(answer, pos);
        });
  }

protected:
  bool eventFilter(QObject *, QEvent *event) override {
    if (event->type() == QEvent::FocusIn && eligible()) delay.start();
    if (event->type() == QEvent::FocusOut || event->type() == QEvent::Hide) clear();
    if (event->type() != QEvent::KeyPress) return false;
    auto key = static_cast<QKeyEvent *>(event);
    if (key->key() == Qt::Key_Escape && (!suggestion.isEmpty() || delay.isActive() ||
                                          (pending && pending->isRunning()))) {
      clear();
      return true;
    }
    if (suggestion.isEmpty()) return false;
    bool word = key->key() == Qt::Key_Right && key->modifiers() == Qt::AltModifier;
    bool all = key->key() == Qt::Key_Tab && key->modifiers() == Qt::NoModifier;
    if (!word && !all) return false;
    if (!eligible() || e->hasSelectedText() || anchor != e->SendScintilla(Editor::SCI_GETCURRENTPOS)) {
      clear();
      return false;
    }
    auto length = suggestion.size();
    if (word) {
      static const QRegularExpression nextWord(QStringLiteral(R"(^\s*(?:[\p{L}\p{M}\p{N}_]+|\S))"),
                                              QRegularExpression::UseUnicodePropertiesOption);
      auto match = nextWord.match(suggestion);
      if (match.hasMatch()) length = match.capturedLength();
    }
    const auto value = suggestion.left(length);
    const auto rest = suggestion.mid(length);
    QScopedValueRollback<bool> guard(accepting, true);
    clear();
    replace(e, value);
    showSuggestion(rest, e->SendScintilla(Editor::SCI_GETCURRENTPOS));
    return true;
  }

public:
  Inline(Editor *editor, ModelClient *models)
      : QObject(editor), e(editor), client(models), hint(new QLabel(editor->viewport())),
        marker(editor->markerDefine(Editor::Invisible)) {
    hint->setTextFormat(Qt::PlainText);
    hint->setStyleSheet("color:#616b72;background:transparent;");
    hint->setAttribute(Qt::WA_TransparentForMouseEvents);
    hint->setFont(e->font());
    hint->hide();
    delay.setSingleShot(true);
    delay.setInterval(650);
    connect(&delay, &QTimer::timeout, this, [this] { request(); });
    connect(e, &Editor::textChanged, this, [this] { if (!accepting) refresh(); });
    connect(e, &Editor::cursorPositionChanged, this, [this] {
      if (!accepting && (anchor != e->SendScintilla(Editor::SCI_GETCURRENTPOS) || e->hasSelectedText()))
        refresh();
    });
    connect(e, &Editor::selectionChanged, this, [this] { if (e->hasSelectedText()) clear(); });
    connect(e->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { if (!accepting) clear(); });
    connect(e->horizontalScrollBar(), &QScrollBar::valueChanged, this, [this] { if (!accepting) clear(); });
    e->installEventFilter(this);
  }
  void refresh() {
    clear();
    if (eligible() && e->hasFocus()) delay.start();
  }
};

void compose(Editor *e, QTabWidget *tabs, ModelClient *client, bool next) {
  const long selectionStart = e->SendScintilla(Editor::SCI_GETSELECTIONSTART);
  const long selectionEnd = e->SendScintilla(Editor::SCI_GETSELECTIONEND);
  const long caret = e->SendScintilla(Editor::SCI_GETCURRENTPOS);
  if (selectionEnd - selectionStart > 32768) {
    QMessageBox::information(tabs, "AI 代码建议",
                             "所选代码过长。请将选区缩小至 32 KiB 以内。");
    return;
  }
  QDialog d(tabs->window());
  d.setWindowTitle(next ? "建议下一处修改" : "AI 代码建议");
  d.resize(680, 600);
  auto box = new QVBoxLayout(&d);
  box->setContentsMargins(20, 16, 20, 16);
  box->setSpacing(12);
  QTabWidget pages;
  pages.setObjectName("assistantPages");
  pages.tabBar()->setObjectName("assistantNav");
  QWidget prompt, config;
  QPlainTextEdit review, context;
  QLineEdit instruction(next ? "找出下一处值得修改的地方，返回 JSON "
                               "对象，before 为唯一的原文，after 为替换文本。"
                        : e->hasSelectedText()
                            ? "改进以下选中的代码。"
                            : "根据上下文续写光标后的代码。");
  ModelProviders providers(client->credentials);
  auto form = new QFormLayout(&config);
  form->addRow(&providers);
  form->setContentsMargins(16, 16, 16, 16);
  form->setVerticalSpacing(12);
  form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  context.setFont(Appearance::codeFont());
  review.setFont(Appearance::codeFont());
  auto layout = new QVBoxLayout(&prompt);
  layout->setContentsMargins(16, 16, 16, 16);
  layout->setSpacing(8);
  auto *instructionLabel = new QLabel("指令");
  instructionLabel->setProperty("section", true);
  instructionLabel->setBuddy(&instruction);
  layout->addWidget(instructionLabel);
  layout->addWidget(&instruction);
  auto *contextRow = new QHBoxLayout;
  auto *contextLabel = new QLabel("代码上下文");
  contextLabel->setProperty("section", true);
  contextLabel->setBuddy(&context);
  contextRow->addWidget(contextLabel);
  contextRow->addStretch();
  layout->addLayout(contextRow);
  layout->addWidget(&context, 1);
  if (e->hasSelectedText())
    context.setPlainText(e->selectedText());
  else if (next) {
    int line, index;
    e->getCursorPosition(&line, &index);
    QString sample;
    for (int i = qMax(0, line - 10); i < qMin(e->lines(), line + 31); ++i)
      sample += e->text(i);
    context.setPlainText(sample.left(8192));
  } else
    context.setPlainText(contextBefore(e, 4096));
  QPushButton attach("附加标签页…");
  attach.setProperty("quiet", true);
  if (!next)
    contextRow->addWidget(&attach);
  QObject::connect(&attach, &QPushButton::clicked, &d, [&] {
    QStringList labels;
    QList<Editor *> docs;
    for (int i = 0; i < tabs->count(); ++i)
      if (auto doc = qobject_cast<Editor *>(tabs->widget(i)); doc && doc != e) {
        labels << QString("%1  %2").arg(i + 1).arg(tabs->tabText(i));
        docs << doc;
      }
    if (docs.isEmpty())
      return;
    bool ok;
    auto label = QInputDialog::getItem(&d, "附加标签页", "代码来源", labels, 0,
                                       false, &ok);
    if (ok) {
      auto doc = docs[labels.indexOf(label)];
      context.appendPlainText(
          "\n[" + label + "]\n" +
          (doc->hasSelectedText() ? doc->selectedText() : doc->text())
              .left(4096));
    }
  });
  pages.addTab(&prompt, "编写指令");
  pages.addTab(&review, "审阅建议");
  pages.addTab(&config, "模型设置");
  pages.tabBar()->hide();
  auto *sections = new QListWidget;
  sections->setAccessibleName("AI 建议步骤");
  sections->setObjectName("assistantSections");
  sections->setFixedWidth(108);
  for (int i = 0; i < pages.count(); ++i) sections->addItem(pages.tabText(i));
  sections->setCurrentRow(pages.currentIndex());
  QObject::connect(sections, &QListWidget::currentRowChanged, &pages, &QTabWidget::setCurrentIndex);
  QObject::connect(&pages, &QTabWidget::currentChanged, sections, [sections](int index) { sections->setCurrentRow(index); });
  auto *workArea = new QHBoxLayout;
  workArea->addWidget(sections);
  workArea->addWidget(&pages, 1);
  box->addLayout(workArea, 1);
  QLabel notice("生成时会将指令和上方代码发送到所选服务。");
  notice.setProperty("secondary", true);
  notice.setTextFormat(Qt::PlainText);
  notice.setWordWrap(true);
  box->addWidget(&notice);
  QDialogButtonBox buttons;
  auto send = buttons.addButton("发送并生成", QDialogButtonBox::ActionRole);
  send->setProperty("primary", true);
  auto apply = buttons.addButton("应用建议", QDialogButtonBox::ActionRole);
  apply->setEnabled(false);
  QObject::connect(&pages, &QTabWidget::currentChanged, &buttons, [send, apply, &pages, &review](int) {
    for (auto *button : {send, apply}) {
      button->setProperty("primary", (pages.currentWidget() == &review) == (button == apply));
      button->style()->unpolish(button);
      button->style()->polish(button);
      button->update();
    }
  });
  buttons.addButton(QDialogButtonBox::Close)->setText("关闭");
  box->addWidget(&buttons);
  QObject::connect(&buttons, &QDialogButtonBox::rejected, &d, &QDialog::reject);
  bool changed = false;
  QPointer<QNetworkReply> activeReply;
  QObject::connect(e, &Editor::textChanged, &d, [&] {
    changed = true;
    apply->setEnabled(false);
  });
  QObject::connect(e, &QObject::destroyed, &d, &QDialog::reject);
  auto saveModel = [&] {
    auto error = providers.save();
    if (!error.isEmpty()) {
      pages.setCurrentWidget(&config);
      notice.setText(error);
      return false;
    }
    return true;
  };
  auto *saveSettings = new QPushButton("保存模型设置");
  form->addRow(QString(), saveSettings);
  QObject::connect(saveSettings, &QPushButton::clicked, &d, [&] {
    if (saveModel()) notice.setText("模型设置已保存");
  });
  QObject::connect(send, &QPushButton::clicked, &d, [&] {
    if (!saveModel()) return;
    if (instruction.text().trimmed().isEmpty() ||
        context.toPlainText().toUtf8().size() > 32768) {
      pages.setCurrentWidget(&prompt);
      notice.setText("请填写指令，并将上下文缩小至 32 KiB 以内。");
      return;
    }
    send->setEnabled(false);
    apply->setEnabled(false);
    notice.setText("正在生成…");
    activeReply =
        client->ask(&d, instruction.text(), context.toPlainText(),
                    [&](QString answer, QString error) {
                      send->setEnabled(true);
                      if (!error.isEmpty()) {
                        notice.setText(error);
                        return;
                      }
                      review.setPlainText(answer);
                      pages.setCurrentWidget(&review);
                      apply->setEnabled(!changed);
                      notice.setText(changed ? "文档已变化，请重新打开。"
                                             : "可以编辑建议后应用。");
                    });
  });
  QObject::connect(apply, &QPushButton::clicked, &d, [&] {
    if (changed || e->isReadOnly())
      return;
    if (e->SendScintilla(Editor::SCI_GETSELECTIONSTART) != selectionStart ||
        e->SendScintilla(Editor::SCI_GETSELECTIONEND) != selectionEnd ||
        e->SendScintilla(Editor::SCI_GETCURRENTPOS) != caret) {
      apply->setEnabled(false);
      notice.setText("光标或选区已变化，请重新打开。");
      return;
    }
    QString answer = review.toPlainText();
    if (next) {
      auto change = QJsonDocument::fromJson(answer.toUtf8()).object();
      QString before = change.value("before").toString();
      auto content = e->text();
      before.replace("\r\n", "\n").replace('\r', '\n');
      // Escape fragments individually so line endings may differ from the
      // prompt preview.
      QStringList fragments;
      for (const auto &line : before.split('\n'))
        fragments << QRegularExpression::escape(line);
      QRegularExpression re(
          "(?=(" + fragments.join("(?:\\r\\n|(?<!\\r)\\n|\\r(?!\\n))") + "))");
      auto hits = re.globalMatch(content);
      if (before.isEmpty() || !change["after"].isString() || !hits.hasNext()) {
        notice.setText("需要 before/after JSON 且原文必须唯一匹配。");
        return;
      }
      auto hit = hits.next();
      if (hits.hasNext()) {
        notice.setText("原文有多个匹配位置。");
        return;
      }
      answer = change["after"].toString();
      QMessageBox confirm(QMessageBox::Question, "确认替换",
                          "原文\n" + hit.captured(1) + "\n\n替换为\n" + answer,
                          QMessageBox::Ok | QMessageBox::Cancel, &d);
      confirm.setTextFormat(Qt::PlainText);
      confirm.setDefaultButton(QMessageBox::Cancel);
      if (confirm.exec() != QMessageBox::Ok)
        return;
      auto start = content.left(hit.capturedStart(1)).toUtf8().size();
      e->SendScintilla(Editor::SCI_SETSEL, start,
                       start + hit.captured(1).toUtf8().size());
    }
    replace(e, answer);
    d.accept();
  });
  d.exec();
  if (activeReply && activeReply->isRunning()) {
    QObject::disconnect(activeReply, nullptr, &d, nullptr);
    activeReply->abort();
  }
}
} // namespace

QAction *installCodeTools(QMainWindow *window, QTabWidget *tabs,
                      std::function<void(QString)> openFile) {
  installLanguageTools(window, tabs, std::move(openFile));
  auto client = new ModelClient(window);
  auto menu = window->menuBar()->addMenu("AI");
  auto composeAction = menu->addAction("AI 代码建议…");
  composeAction->setShortcut(QKeySequence("Ctrl+Alt+I"));
  auto next = menu->addAction("建议下一处修改…");
  auto toggle = menu->addAction("输入时显示 AI 灰字补全");
  toggle->setCheckable(true);
  toggle->setToolTip("Tab 接受全部，Alt+Right 接受下一个词，Esc 忽略");
  auto controllers = std::make_shared<QHash<Editor *, Inline *>>();
  auto editor = [tabs] {
    return qobject_cast<Editor *>(tabs->currentWidget());
  };
  for (auto action : {composeAction, next})
    QObject::connect(action, &QAction::triggered, menu, [=] {
      if (auto e = editor(); e && !e->isReadOnly())
        compose(e, tabs, client, action == next);
    });
  auto attach = [=] {
    auto *e = editor();
    if (!e || controllers->contains(e) || QSettings().value("ai/inlineEndpoint").toString().isEmpty())
      return;
    controllers->insert(e, new Inline(e, client));
    QObject::connect(e, &QObject::destroyed, menu, [controllers, e] { controllers->remove(e); });
  };
  QObject::connect(tabs, &QTabWidget::currentChanged, menu, [attach](int) { attach(); });
  attach();
  QObject::connect(toggle, &QAction::triggered, menu, [=](bool enable) {
    if (enable) {
      if (!Ai::validEndpoint(endpoint()) || model().isEmpty()) {
        QMessageBox::information(window, "AI 灰字补全", "请先在 AI 代码建议的“模型设置”页保存接口与模型名称。");
        toggle->setChecked(false);
        return;
      }
      if (QMessageBox::question(window, "启用 AI 灰字补全？",
          "在代码或配置文件中暂停输入后，将自动发送光标前后合计最多 4096 个字符到 " +
          endpoint() + "。设置适用于之后打开的代码标签。Tab 接受全部，Alt+Right 接受下一个词，Esc 忽略。",
          QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes) {
        toggle->setChecked(false);
        return;
      }
    }
    QSettings().setValue("ai/inlineEndpoint", enable ? endpoint() : QString());
    QSettings().setValue("ai/inlineContextEndpoint", enable ? endpoint() : QString());
    attach();
    if (auto *e = editor()) e->setFocus();
    for (auto *controller : *controllers) controller->refresh();
  });
  QObject::connect(menu, &QMenu::aboutToShow, menu, [=] {
    auto e = editor();
    for (auto a : {composeAction, next})
      a->setEnabled(e && !e->isReadOnly());
    toggle->setChecked(!endpoint().isEmpty() && QSettings().value("ai/inlineEndpoint").toString() == endpoint());
  });
  return composeAction;
}
