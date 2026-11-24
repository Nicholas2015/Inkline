#include "numberedlogpage.h"
#include "logstyle.h"
#include "logfields.h"
#include <QTimeEdit>
#include "logview.h"
#include "appearance.h"
#include "document.h"
#include "logfile.h"
#include "logscan.h"
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFontDatabase>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProcess>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSettings>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <cmath>
#include <cstdio>
#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {
constexpr qint64 pageBytes = 128 * 1024;
QString displayText(QString s) {
  s.replace("\r\n", "\n");
  return s.replace('\r', '\n');
}
bool installExport(const QString &source, const QString &dest) {
#ifdef Q_OS_WIN
  return MoveFileExW(reinterpret_cast<LPCWSTR>(source.utf16()),
                     reinterpret_cast<LPCWSTR>(dest.utf16()),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
#else
  return std::rename(QFile::encodeName(source).constData(),
                     QFile::encodeName(dest).constData()) == 0;
#endif
}
QPushButton *button(QBoxLayout *row, const QString &text) {
  auto *b = new QPushButton(text);
  b->setAutoDefault(false);
  row->addWidget(b);
  return b;
}
} // namespace
struct Log::State {
  Log *owner;
  QString path;
  NumberedLogPage *view = new NumberedLogPage;
  LogStyle::Layer *highlight=new LogStyle::Layer(view->document());
  QComboBox *codec = new QComboBox;
  QLabel *position = new QLabel;
  QPushButton *more = nullptr, *go = nullptr, *stop = nullptr;
  QToolButton *follow = new QToolButton;
  QMenu *marks = nullptr;
  struct Mark {
    QDateTime birth;
    QByteArray prefix;
    QString label;
  };
  QMap<qint64, Mark> bookmarks;
  QDialog *dialog;
  QComboBox *mode = new QComboBox, *terms = new QComboBox;
  QCheckBox *regex = new QCheckBox("正则"),
            *sensitive = new QCheckBox("区分大小写");
  QCheckBox *timeRange = new QCheckBox("限定时段");
  QTimeEdit *fromTime = new QTimeEdit(QTime(0, 0)), *toTime = new QTimeEdit(QTime(23, 59, 59));
  QLabel *untilLabel = new QLabel("至");
  QLabel *status = new QLabel;
  QListWidget *hits = new QListWidget;
  QProcess *job = nullptr;
  QByteArray raw;
  qint64 start = 0, end = 0, continuation = -1, resume = -1, snapshotSize = -1;
  QDateTime snapshotTime, tailTime, snapshotBirth, tailBirth;
  qint64 tailSize = -1;
  bool filterActive = false;
  QVector<qint64> anchors;
  qint64 indexedSize = -1, indexedLines = 0;
  QDateTime indexedTime, indexedBirth;

  State(Log *w, const QString &filename)
      : owner(w), path(QFileInfo(filename).absoluteFilePath()),
        dialog(new QDialog(w)) {}
  QByteArray encoding() const { return codec->currentText().toLatin1(); }
  void notice(const QString &s) { position->setText(s); }
  void page(qint64 offset, qint64 until = -1, bool bottom = false) {
    try {
      auto bytes = readPage(path, offset, encoding());
      if (until >= offset)
        bytes.truncate(qMin<qint64>(bytes.size(), until - offset));
      raw = bytes;
      start = offset;
      end = start + bytes.size();
      view->setPage(path,offset,bytes,displayText(decode(bytes,encoding(),false)));
      if (bottom)
        view->moveCursor(QTextCursor::End);
      auto size = QFileInfo(path).size();
      position->setText(
          QString("只读    %1 – %2 / %3 字节").arg(start).arg(end).arg(size));
    } catch (const std::exception &e) {
      notice(QString::fromUtf8(e.what()));
    }
  }
  // Keep half of the old window so the same visual line can anchor the
  // viewport while Qt lays out the next (or preceding) part of the file.
  bool slide(int direction) {
    const bool forward = direction > 0;
    if ((forward && (end <= start || end >= QFileInfo(path).size())) ||
        (!forward && !start))
      return false;
    auto *bar = view->verticalScrollBar();
    const bool sparse = bar->maximum() == bar->minimum();
    const auto oldStart = start;
    auto top = view->cursorForPosition(QPoint(1, 1));
    const int topY = view->cursorRect(top).top();
    const int oldTop = top.position();
    const int oldPosition = view->textCursor().position();
    const int oldAnchor = view->textCursor().anchor();
    const int horizontal = view->horizontalScrollBar()->value();
    int shift = 0;
    try {
      qint64 target = forward ? start + (end - start) / 2
                              : qMax<qint64>(0, start - pageBytes / 2);
      auto offset = logAlignedStart(path, target, encoding());
      if (forward && offset <= start)
        offset = end;
      if (forward)
        shift =
            -displayText(decode(raw.left(offset - start), encoding(), false))
                 .size();
      page(offset);
      if (start == oldStart)
        return false;
      if (!forward)
        shift =
            displayText(decode(raw.left(oldStart - start), encoding(), false))
                .size();
    } catch (const std::exception &e) {
      notice(QString::fromUtf8(e.what()));
      return false;
    }
    auto *doc = view->document();
    const int last = doc->characterCount() - 1;
    const int mappedTop = oldTop + shift;
    if (sparse || mappedTop < 0 || mappedTop > last) {
      view->moveCursor(forward ? QTextCursor::Start : QTextCursor::End);
      return true;
    }
    // Measure the anchor after wrapping and font layout rather than assuming
    // one scrollbar step per logical record.
    QTextCursor anchor(doc);
    anchor.setPosition(mappedTop);
    view->setTextCursor(anchor);
    view->centerCursor();
    const auto rect = view->cursorRect(anchor);
    const int scroll = bar->value() + qRound(double(rect.top() - topY) /
                                             qMax(1, rect.height()));
    QTextCursor selection(doc);
    selection.setPosition(qBound(0, oldAnchor + shift, last));
    selection.setPosition(qBound(0, oldPosition + shift, last),
                          QTextCursor::KeepAnchor);
    view->setTextCursor(selection);
    bar->setValue(scroll);
    view->horizontalScrollBar()->setValue(horizontal);
    return false;
  }
  void visit(qint64 offset, qsizetype length = 0) {
    try {
      follow->setChecked(false);
      auto size = QFileInfo(path).size();
      offset = qBound<qint64>(0, offset, size);
      auto base = logAlignedStart(path, offset, encoding());
      if (offset == size)
        base = logAlignedStart(path, qMax<qint64>(0, size - pageBytes + 4096),
                               encoding());
      page(base);
      auto index =
          displayText(
              decode(raw.left(qBound<qint64>(0, offset - base, raw.size())),
                     encoding(), false))
              .size();
      auto c = view->textCursor();
      auto max = view->document()->characterCount() - 1;
      c.setPosition(qMin<qsizetype>(max, index));
      c.setPosition(qMin<qsizetype>(max, index + length),
                    QTextCursor::KeepAnchor);
      view->setTextCursor(c);
      view->centerCursor();
    } catch (const std::exception &e) {
      notice(QString::fromUtf8(e.what()));
    }
  }
  qint64 cursorOffset() const {
    const auto line = view->textCursor().blockNumber();
    qsizetype i = 0;
    for (int n = 0; n < line && i < raw.size(); ++n) {
      while (i < raw.size() && raw[i] != '\n' && raw[i] != '\r')
        ++i;
      if (i < raw.size() && raw[i] == '\r')
        ++i;
      if (i < raw.size() && raw[i] == '\n')
        ++i;
    }
    return start + i;
  }
  void populateMarks() {
    marks->clear();
    auto pos = cursorOffset();
    marks->addAction(bookmarks.contains(pos) ? "移除本行书签" : "标记本行",
                     owner, [this, pos] {
                       if (bookmarks.contains(pos))
                         bookmarks.remove(pos);
                       else
                         bookmarks.insert(
                             pos, {QFileInfo(path).birthTime(),
                                   raw.mid(pos - start, 40),
                                   view->textCursor().block().text().left(80)});
                     });
    if (hits->count())
      marks->addAction("将本批结果加入书签", owner, [this] {
        if (QFileInfo(path).birthTime() != snapshotBirth ||
            QFileInfo(path).size() < snapshotSize) {
          notice("日志已变化，请重新搜索");
          return;
        }
        auto req = request("marks");
        QJsonArray offsets;
        for (int i = 0; i < hits->count(); ++i)
          offsets.append(hits->item(i)->data(Qt::UserRole).toLongLong());
        req["hits"] = offsets;
        auto birth = snapshotBirth;
        launch(req, [this, birth](QJsonObject result) {
          for (auto v : result["marks"].toArray()) {
            auto m = v.toArray();
            auto prefix = QByteArray::fromBase64(m[1].toString().toLatin1());
            bookmarks.insert(
                m[0].toInteger(),
                {birth, prefix,
                 decode(prefix, encoding(), false).trimmed().left(55)});
          }
          status->setText("已加入书签");
        });
      });
    if (!bookmarks.isEmpty())
      marks->addAction("清除全部书签", owner, [this] { bookmarks.clear(); });
    marks->addSeparator();
    for (auto i = bookmarks.cbegin(); i != bookmarks.cend(); ++i)
      marks->addAction(QString("%1   %2").arg(i.key()).arg(i->label), owner,
                       [this, pos = i.key()] {
                         auto m = bookmarks.value(pos);
                         QFile f(path);
                         if (m.birth != QFileInfo(path).birthTime() ||
                             !f.open(QIODevice::ReadOnly) || !f.seek(pos) ||
                             f.read(m.prefix.size()) != m.prefix) {
                           bookmarks.remove(pos);
                           notice("书签位置的日志已经变化");
                           return;
                         }
                         visit(pos);
                       });
  }
  void updateControls() {
    go->setEnabled(!terms->currentText().isEmpty());
    stop->setEnabled(job != nullptr);
    more->setEnabled(!job && continuation >= 0);
  }
  void cancel() {
    if (job) {
      auto *old = job;
      job = nullptr;
      old->disconnect(owner);
      QObject::connect(old, &QProcess::finished, old, &QObject::deleteLater);
      old->kill();
      if (old->state() == QProcess::NotRunning)
        old->deleteLater();
      status->setText("已取消");
    }
    updateControls();
  }
  void reset() {
    cancel();
    hits->clear();
    continuation = -1;
    resume = -1;
    snapshotSize = -1;
    filterActive = false;
    regex->setEnabled(mode->currentIndex() == 1);
    sensitive->setEnabled(mode->currentIndex() == 1);
    const bool filtering = mode->currentIndex() != 0;
    timeRange->setVisible(filtering);
    fromTime->setVisible(filtering && timeRange->isChecked());
    untilLabel->setVisible(filtering && timeRange->isChecked());
    toTime->setVisible(filtering && timeRange->isChecked());
    terms->lineEdit()->setPlaceholderText(mode->currentIndex() == 2 ? "输入完整的 session_id" : "输入日志中的文字");
    status->clear();
    updateControls();
  }
  void tail() {
    QFileInfo info(path);
    if (!info.exists()) {
      notice("等待日志文件重新出现…");
      tailSize = -1;
      return;
    }
    auto size = info.size();
    if (tailSize != size || tailTime != info.lastModified() ||
        tailBirth != info.birthTime()) {
      tailSize = size;
      tailTime = info.lastModified();
      tailBirth = info.birthTime();
      try {
        page(logAlignedStart(path, qMax<qint64>(0, size - pageBytes + 4096),
                             encoding()),
             -1, true);
      } catch (const std::exception &e) {
        notice(QString::fromUtf8(e.what()));
      }
    }
    if (!filterActive || job)
      return;
    if (snapshotBirth != info.birthTime() || size < snapshotSize ||
        (size == snapshotSize && info.lastModified() != snapshotTime)) {
      search();
      return;
    }
    if (size <= snapshotSize || resume < 0 || continuation >= 0)
      return;
    while (hits->count() &&
           hits->item(hits->count() - 1)->data(Qt::UserRole).toLongLong() >=
               resume)
      delete hits->takeItem(hits->count() - 1);
    if (hits->count() >= 200) {
      continuation = resume;
      snapshotSize = size;
      snapshotTime = info.lastModified();
      status->setText("本批已满，可继续扫描追加内容");
      updateControls();
    } else
      search(resume, true);
  }
  QJsonObject request(QString kind) const {
    const bool filter = mode->currentIndex() != 0;
    return {{"kind", kind},
            {"path", path},
            {"codec", codec->currentText()},
            {"size", QFileInfo(path).size()},
            {"query", terms->currentText()},
            {"filter", filter},
            {"session", mode->currentIndex() == 2},
            {"fromSecond", filter && timeRange->isChecked() ? fromTime->time().msecsSinceStartOfDay() / 1000 : -1},
            {"toSecond", filter && timeRange->isChecked() ? toTime->time().msecsSinceStartOfDay() / 1000 : -1},
            {"regex", filter && regex->isChecked()},
            {"case", mode->currentIndex() != 1 || sensitive->isChecked()}};
  }
  void launch(QJsonObject req, std::function<void(QJsonObject)> done) {
    cancel();
    auto *proc = new QProcess(owner);
    QObject::connect(proc, &QObject::destroyed, [keepAlive = done] {});
    job = proc;
    updateControls();
    status->setText("处理中…");
    QObject::connect(proc, &QProcess::started, owner, [proc, req] {
      proc->write(QJsonDocument(req).toJson(QJsonDocument::Compact));
      proc->closeWriteChannel();
    });
    QObject::connect(proc, &QProcess::errorOccurred, owner,
                     [this, proc](QProcess::ProcessError e) {
                       if (job != proc || e != QProcess::FailedToStart)
                         return;
                       status->setText(proc->errorString());
                       notice(status->text());
                       job = nullptr;
                       proc->deleteLater();
                       updateControls();
                     });
    QObject::connect(
        proc, &QProcess::finished, owner,
        [this, proc, done](int code, QProcess::ExitStatus state) {
          if (job != proc)
            return;
          auto doc = QJsonDocument::fromJson(proc->readAllStandardOutput());
          auto result = doc.object();
          job = nullptr;
          proc->deleteLater();
          if (code || state != QProcess::NormalExit || !doc.isObject() ||
              result.contains("error")) {
            status->setText(result["error"].toString("日志任务失败"));
            notice(status->text());
          } else
            done(result);
          updateControls();
        });
    proc->start(QCoreApplication::applicationFilePath(), {"--log-job"});
  }
  void search(qint64 from = 0, bool append = false) {
    if (terms->currentText().isEmpty())
      return;
    QFileInfo info(path);
    if (from > 0 && !append &&
        (info.birthTime() != snapshotBirth || snapshotSize != info.size() ||
         snapshotTime != info.lastModified())) {
      reset();
      status->setText("日志已变化，请重新搜索");
      return;
    }
    if (!append)
      hits->clear();
    snapshotSize = info.size();
    snapshotTime = info.lastModified();
    snapshotBirth = info.birthTime();
    filterActive = mode->currentIndex() != 0;
    QString term = terms->currentText();
    auto history = QSettings().value("log/history").toStringList();
    history.removeAll(term);
    history.prepend(term);
    while (history.size() > 20)
      history.removeLast();
    QSettings().setValue("log/history", history);
    {
      QSignalBlocker block(terms);
      terms->clear();
      terms->addItems(history);
      terms->setEditText(term);
    }
    auto req = request("search");
    req["start"] = from;
    req["limit"] = 200 - hits->count();
    continuation = -1;
    launch(req, [this](QJsonObject r) {
      if (snapshotBirth != QFileInfo(path).birthTime() ||
          QFileInfo(path).size() < snapshotSize) {
        reset();
        status->setText("日志已被截断，请重新搜索");
        return;
      }
      for (auto v : r["hits"].toArray()) {
        auto h = v.toArray();
        auto *item = new QListWidgetItem(
            QString("字节 %1    %2").arg(h[0].toInteger()).arg(h[2].toString()),
            hits);
        item->setData(Qt::UserRole, h[0].toInteger());
        item->setData(Qt::UserRole + 1, h[1].toInteger());
      }
      continuation = r["more"].toInteger(-1);
      resume = r["resume"].toInteger(-1);
      status->setText(QString("%1 处匹配%2")
                          .arg(hits->count())
                          .arg(continuation >= 0 ? " · 可继续扫描" : ""));
    });
  }
  void output(bool all, bool clipboard) {
    if (terms->currentText().isEmpty() || (!all && !hits->count()))
      return;
    auto req = request(all ? "all" : "batch");
    if (!all && (snapshotBirth != QFileInfo(path).birthTime() ||
                 snapshotSize != QFileInfo(path).size() ||
                 snapshotTime != QFileInfo(path).lastModified())) {
      status->setText("日志已变化，请重新搜索");
      return;
    }
    if (all) {
      bool ok = false;
      int context = QInputDialog::getInt(
          dialog, "导出全部匹配行", "每处前后保留几行", 0, 0, 1000, 1, &ok);
      if (!ok)
        return;
      req["context"] = context;
    } else {
      QJsonArray offsets;
      for (int i = 0; i < hits->count(); ++i)
        offsets.append(hits->item(i)->data(Qt::UserRole).toLongLong());
      req["hits"] = offsets;
    }
    QString target;
    if (!clipboard) {
      target = QFileDialog::getSaveFileName(dialog, "导出匹配行");
      if (target.isEmpty())
        return;
      if (QFileInfo(target) == QFileInfo(path)) {
        status->setText("请选择与源日志不同的文件");
        return;
      }
    }
    auto dir = std::make_shared<QTemporaryDir>(
        (clipboard ? QDir::tempPath() : QFileInfo(target).absolutePath()) +
        "/.inkline-XXXXXX");
    if (!dir->isValid()) {
      status->setText("无法创建临时文件");
      return;
    }
    auto temp = dir->filePath("matches");
    req["output"] = temp;
    auto enc = encoding();
    launch(req, [this, dir, temp, target, clipboard, enc](QJsonObject) {
      if (clipboard) {
        QFile f(temp);
        if (f.size() > 32 * 1024 * 1024) {
          status->setText("匹配行超过 32 MiB，请导出到文件");
          return;
        }
        if (!f.open(QIODevice::ReadOnly)) {
          status->setText(f.errorString());
          return;
        }
        QApplication::clipboard()->setText(decode(f.readAll(), enc, false));
        status->setText("已复制匹配行");
      } else
        status->setText(installExport(temp, target) ? "已导出匹配行"
                                                    : "无法替换目标文件");
    });
  }
};

Log::Log(const QString &file) : d(std::make_unique<State>(this, file)) {
  auto &s = *d;
  setProperty("filePath", s.path);
  auto *layout = new QVBoxLayout(this);
  auto *bar = new QHBoxLayout;
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  bar->setContentsMargins(12, 4, 12, 4);
  bar->setSpacing(8);
  auto *jump = button(bar, "跳转…");
  auto *bookmark = new QToolButton;
  bookmark->setText("书签");
  bookmark->setPopupMode(QToolButton::InstantPopup);
  s.marks = new QMenu(bookmark);
  bookmark->setMenu(s.marks);
  bar->addWidget(bookmark);
  auto *wrap = new QCheckBox("自动换行");
  wrap->setObjectName("logWrap");
  bar->addWidget(wrap);
  bar->addStretch();
  s.follow->setText("跟随最新");
  s.follow->setObjectName("followTail");
  s.follow->setCheckable(true);
  bar->addWidget(s.follow);
  auto *find = button(bar, "查找…");
  layout->addLayout(bar);
  layout->addWidget(s.view, 1);
  auto *footer = new QHBoxLayout;
  footer->setContentsMargins(12, 2, 8, 2);
  footer->addWidget(s.position, 1);
  footer->addWidget(s.codec);
  layout->addLayout(footer);
  s.codec->addItems({"UTF-8", "GBK"});
  s.codec->setObjectName("logEncoding");
  s.view->setObjectName("logPage");
  s.position->setObjectName("logPosition");
  s.view->setReadOnly(true);
  s.view->setUndoRedoEnabled(false);
  s.view->setLineWrapMode(QPlainTextEdit::NoWrap);
  s.view->setFont(Appearance::codeFont());
  s.dialog->setWindowTitle("日志检索 — " + QFileInfo(s.path).fileName());
  s.dialog->resize(700, 420);
  auto *panel = new QVBoxLayout(s.dialog);
  panel->setContentsMargins(20,16,20,16);
  panel->setSpacing(12);
  auto *body = new QHBoxLayout;
  panel->addLayout(body, 1);
  auto *conditions = new QVBoxLayout;
  auto *output = new QVBoxLayout;
  body->setSpacing(16);
  body->addLayout(conditions, 2);
  body->addLayout(output, 3);
  auto *row = conditions;
  auto *heading = new QLabel("匹配结果");
  heading->setProperty("section", true);
  output->addWidget(heading);
  s.mode->addItems({"查找文字", "筛选匹配行", "筛选会话"});
  s.terms->setEditable(true);
  s.terms->setInsertPolicy(QComboBox::NoInsert);
  s.terms->addItems(QSettings().value("log/history").toStringList());
  s.terms->setEditText({});
  s.terms->lineEdit()->setMaxLength(4096);
  row->addWidget(s.mode);
  row->addWidget(s.terms);
  auto *filters = new QVBoxLayout;
  filters->addWidget(s.regex);filters->addWidget(s.sensitive);filters->addWidget(s.timeRange);
  for (auto *edit : {s.fromTime, s.toTime}) {
    edit->setDisplayFormat("HH:mm:ss");
    edit->setToolTip(edit == s.fromTime ? "起始时间（含）" : "结束时间（含，可跨午夜）");
    if (edit == s.toTime)
      filters->addWidget(s.untilLabel);
    edit->setAccessibleName(edit == s.fromTime ? "起始时间" : "结束时间");
    filters->addWidget(edit);
    connect(edit, &QTimeEdit::timeChanged, this, [this] { d->reset(); });
  }
  connect(s.timeRange, &QCheckBox::toggled, this, [this] { d->reset(); });
  filters->addStretch();
  conditions->addLayout(filters, 1);
  s.sensitive->setChecked(true);
  auto *commands = new QHBoxLayout;
  conditions->addLayout(commands);
  s.go = button(commands, "查找");
  s.go->setProperty("primary", true);
  s.terms->lineEdit()->setPlaceholderText("输入日志中的文字");
  s.stop = button(commands, "取消");
  s.hits->setObjectName("logMatches");
  s.hits->setAccessibleName("日志匹配结果");
  s.hits->setAlternatingRowColors(true);
  s.hits->setFont(Appearance::codeFont());
  output->addWidget(s.hits, 1);
  auto *nav = new QHBoxLayout;
  output->addWidget(s.status);
  nav->addStretch();
  auto *back = button(nav, "上一处"), *forward = button(nav, "下一处");
  s.more = button(nav, "继续扫描");
  output->addLayout(nav);
  auto *exports = new QHBoxLayout;
  exports->addStretch();
  auto *copy = button(exports, "复制本批匹配行"),
       *save = button(exports, "导出本批…"),
       *all = button(exports, "导出全部…");
  panel->addLayout(exports);
  connect(copy, &QPushButton::clicked, this,
          [this] { d->output(false, true); });
  connect(save, &QPushButton::clicked, this,
          [this] { d->output(false, false); });
  connect(all, &QPushButton::clicked, this, [this] { d->output(true, false); });
  connect(back, &QPushButton::clicked, this, [this] {
    d->hits->setCurrentRow(qMax(0, d->hits->currentRow() - 1));
  });
  connect(forward, &QPushButton::clicked, this, [this] {
    d->hits->setCurrentRow(
        qMin(d->hits->count() - 1, d->hits->currentRow() + 1));
  });
  connect(s.more, &QPushButton::clicked, this, [this] {
    if (d->continuation >= 0)
      d->search(d->continuation);
  });
  auto locate = [this](QListWidgetItem *h) {
            if (!h)
              return false;
            QFileInfo info(d->path);
            if (info.birthTime() != d->snapshotBirth ||
                info.size() < d->snapshotSize ||
                (info.size() == d->snapshotSize &&
                 info.lastModified() != d->snapshotTime)) {
              d->status->setText("日志已变化，请重新搜索");
              return false;
            }
            d->visit(h->data(Qt::UserRole).toLongLong(),
                     h->data(Qt::UserRole + 1).toLongLong());
    return true;
  };
  connect(s.hits, &QListWidget::currentItemChanged, this, locate);
  s.hits->setToolTip("双击或按回车，在原日志中查看");
  connect(s.hits, &QListWidget::itemActivated, this, [this, locate](QListWidgetItem *item) {
    if (locate(item)) {
      d->dialog->accept();
      d->view->setFocus();
    }
  });
  s.view->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(s.view, &QWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
    auto cursor = d->view->cursorForPosition(pos);
    QString id = sessionAt(cursor.block().text(), cursor.positionInBlock());
    std::unique_ptr<QMenu> menu(d->view->createStandardContextMenu());
    if (!id.isEmpty()) {
      menu->addSeparator();
      menu->addAction("只看此会话", this, [this, id] {
        d->mode->setCurrentIndex(2);
        d->terms->setEditText(id);
        showSearch();
        d->search();
      });
    }
    menu->exec(d->view->viewport()->mapToGlobal(pos));
  });
  connect(s.go, &QPushButton::clicked, this, [this] { d->search(); });
  connect(s.stop, &QPushButton::clicked, this, [this] { d->cancel(); });
  connect(s.dialog, &QDialog::finished, this, [this] { d->cancel(); });
  connect(s.terms->lineEdit(), &QLineEdit::returnPressed, this,
          [this] { d->search(); });
  connect(s.terms, &QComboBox::editTextChanged, this, [this] { d->reset(); });
  connect(s.mode, &QComboBox::currentIndexChanged, this,
          [this] { d->reset(); });
  connect(s.regex, &QCheckBox::toggled, this, [this] { d->reset(); });
  connect(s.sensitive, &QCheckBox::toggled, this, [this] { d->reset(); });
  connect(s.codec, &QComboBox::currentIndexChanged, this, [this] {
    d->reset();
    d->visit(d->start);
  });
  connect(wrap, &QCheckBox::toggled, this, [this](bool checked) {
    d->view->setLineWrapMode(checked ? QPlainTextEdit::WidgetWidth
                                     : QPlainTextEdit::NoWrap);
  });
  connect(s.marks, &QMenu::aboutToShow, this, [this] { d->populateMarks(); });
  connect(find, &QPushButton::clicked, this, &Log::showSearch);
  connect(jump, &QPushButton::clicked, this, &Log::jumpDialog);
  auto *timer = new QTimer(this);
  timer->setInterval(500);
  connect(timer, &QTimer::timeout, this, [this] { d->tail(); });
  connect(s.follow, &QToolButton::toggled, this, [this, timer](bool on) {
    if (on) {
      d->tailSize = -1;
      d->tail();
      timer->start();
    } else
      timer->stop();
  });
  connect(s.view->verticalScrollBar(), &QScrollBar::sliderMoved, this,
          [this](int v) {
            if (v < d->view->verticalScrollBar()->maximum())
              d->follow->setChecked(false);
          });
  s.view->viewport()->installEventFilter(this);
  s.view->installEventFilter(this);
  s.reset();
  s.page(0);
}
Log::~Log() {
  d->view->viewport()->removeEventFilter(this);
  d->view->removeEventFilter(this);
  d->cancel();
}
QString Log::filePath() const { return d->path; }
QJsonObject Log::saveView() const {
  const auto cursor = d->view->textCursor();
  return {{"offset", d->start},
          {"cursor", cursor.position()},
          {"anchor", cursor.anchor()},
          {"vertical", d->view->verticalScrollBar()->value()},
          {"horizontal", d->view->horizontalScrollBar()->value()},
          {"encoding", d->codec->currentText()},
          {"wrap", d->view->lineWrapMode() == QPlainTextEdit::WidgetWidth},
          {"follow", d->follow->isChecked()}};
}
void Log::restoreView(const QJsonObject &state) {
  if (state.isEmpty())
    return;
  d->follow->setChecked(false);
  auto codec =
      d->codec->findText(state["encoding"].toString(), Qt::MatchFixedString);
  if (codec >= 0)
    d->codec->setCurrentIndex(codec);
  findChild<QCheckBox *>("logWrap")->setChecked(state["wrap"].toBool());
  auto size = QFileInfo(d->path).size();
  auto offset = state["offset"].toInteger();
  if (offset > size)
    d->visit(size);
  else
    d->page(qMax<qint64>(0, offset));
  auto cursor = d->view->textCursor();
  auto max = d->view->document()->characterCount() - 1;
  cursor.setPosition(qBound(0, state["anchor"].toInt(), max));
  cursor.setPosition(qBound(0, state["cursor"].toInt(), max),
                     QTextCursor::KeepAnchor);
  d->view->setTextCursor(cursor);
  d->view->verticalScrollBar()->setValue(state["vertical"].toInt());
  d->view->horizontalScrollBar()->setValue(state["horizontal"].toInt());
  d->follow->setChecked(state["follow"].toBool());
}
void Log::setHighlightWords(const QStringList &words){d->highlight->setWords(words);}
void Log::showSearch() {
  d->dialog->show();
  d->dialog->raise();
  d->dialog->activateWindow();
  d->terms->setFocus();
  d->terms->lineEdit()->selectAll();
}
void Log::gotoByte(qint64 offset, const QString &codec) {
  gotoSearchHit(offset, codec, 0);
}
void Log::gotoSearchHit(qint64 offset, const QString &codec, int matchLength) {
  if (!codec.isEmpty()) {
    auto idx = d->codec->findText(codec, Qt::MatchFixedString);
    if (idx >= 0 && idx != d->codec->currentIndex()) {
      QSignalBlocker blocked(d->codec);
      d->codec->setCurrentIndex(idx);
      d->reset();
    }
  }
  d->visit(offset, matchLength);
}
void Log::gotoLine(qint64 line) {
  auto req = d->request("line");
  req["line"] = line;
  const QFileInfo info(d->path);
  const auto size = info.size();
  const auto modified = info.lastModified(), birth = info.birthTime();
  const bool cached = size == d->indexedSize && modified == d->indexedTime &&
                      birth == d->indexedBirth && !d->anchors.isEmpty();
  if (cached) {
    if (line < 1 || line > d->indexedLines) {
      d->notice(QString("请输入 1 至 %1 的行号").arg(d->indexedLines));
      return;
    }
    req["anchor"] = d->anchors[(line - 1) / 1024];
  }
  d->follow->setChecked(false);
  d->notice(cached ? "正在定位行号…" : "正在建立行号索引…");
  d->launch(req, [this, line, size, modified, birth](QJsonObject r) {
    QFileInfo fresh(d->path);
    if (fresh.size() != size || fresh.lastModified() != modified ||
        fresh.birthTime() != birth) {
      d->notice("日志在定位期间发生变化，请重试");
      return;
    }
    if (r.contains("anchors")) {
      const auto offsets = r["anchors"].toArray();
      d->anchors.clear();
      d->anchors.reserve(offsets.size());
      for (auto v : offsets)
        d->anchors.append(v.toInteger());
      d->indexedSize = size;
      d->indexedTime = modified;
      d->indexedBirth = birth;
      d->indexedLines = r["lines"].toInteger();
    }
    const auto at=r["offset"].toInteger();gotoByte(at);d->view->confirmFirstLine(at,line);
  });
}
void Log::jumpDialog() {
  bool ok = false;
  auto input =
      QInputDialog::getText(this, "跳转到文件位置",
                            "行号输入 #128，也可输入 50%、1 GB、64 KiB",
                            QLineEdit::Normal, {}, &ok)
          .trimmed();
  if (!ok)
    return;
  if (input.startsWith('#')) {
    auto line = input.mid(1).toLongLong(&ok);
    if (ok && line > 0) {
      gotoLine(line);
      return;
    }
  } else {
    static const QRegularExpression pattern(
        R"(^(\d+(?:\.\d+)?)\s*(%|[kmgt]i?b?|b)?$)",
        QRegularExpression::CaseInsensitiveOption);
    auto m = pattern.match(input);
    if (m.hasMatch()) {
      auto size = QFileInfo(d->path).size();
      auto unit = m.captured(2).toLower();
      long double value = m.captured(1).toDouble();
      if (unit == "%")
        value = value / 100 * size;
      else if (!unit.isEmpty() && unit != "b") {
        auto power = QStringLiteral("kmgt").indexOf(unit[0]) + 1;
        value *= std::pow(unit.contains('i') ? 1024. : 1000., power);
      }
      if (value >= 0 && value <= size) {
        gotoByte(qint64(value));
        return;
      }
    }
  }
  QMessageBox::information(this, "无法跳转",
                           "请输入有效的行号或文件范围内的位置");
}
bool Log::eventFilter(QObject *obj, QEvent *e) {
  auto *bar = d->view->verticalScrollBar();
  int direction = 0;
  if (e->type() == QEvent::Wheel) {
    auto *wheel = static_cast<QWheelEvent *>(e);
    auto delta = wheel->pixelDelta().y();
    if (!delta)
      delta = wheel->angleDelta().y();
    if (!wheel->modifiers())
      direction = (delta < 0) - (delta > 0);
  } else if (e->type() == QEvent::KeyPress) {
    auto *k = static_cast<QKeyEvent *>(e);
    if (!k->modifiers()) {
      if (k->key() == Qt::Key_Up || k->key() == Qt::Key_PageUp)
        direction = -1;
      if (k->key() == Qt::Key_Down || k->key() == Qt::Key_PageDown)
        direction = 1;
    }
  }
  if (direction < 0)
    d->follow->setChecked(false);
  const int margin = qMax(2, bar->pageStep());
  if ((direction > 0 && bar->maximum() - bar->value() <= margin) ||
      (direction < 0 && bar->value() - bar->minimum() <= margin)) {
    if (d->slide(direction)) {
      e->accept();
      return true;
    }
  }
  return QWidget::eventFilter(obj, e);
}
