#pragma once
#include "columnheader.h"
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QPlainTextEdit>
#include <QTextBlock>
#include <QtConcurrentRun>
#include <atomic>
#include <memory>

// UTF-8 and GBK both encode CR and LF as single ASCII bytes.
inline qint64 breaksIn(QByteArrayView bytes, bool previousCR) {
  return bytes.count('\n') + bytes.count('\r') - bytes.count("\r\n") -
         (previousCR && !bytes.empty() && bytes.front() == '\n');
}
class NumberedLogPage final : public QPlainTextEdit {
  struct Gutter final : QWidget {
    NumberedLogPage *page;
    explicit Gutter(NumberedLogPage *p) : QWidget(p), page(p) {
      setAccessibleName("日志行号");
      setToolTip("原文件行号，从 1 开始");
    }
    void paintEvent(QPaintEvent *) override { page->paintNumbers(this); }
  };
  ColumnHeader header{this};
  Gutter gutter{this};
  qint64 first = 1, offset = 0, size = -1;
  QByteArray pageBytes;
  QString path;
  QDateTime modified, birth;
  struct Count {
    qint64 line = -1;
    QMap<qint64, qint64> anchors;
  };
  QMap<qint64, qint64> checkpoints{{0, 1}};
  std::shared_ptr<std::atomic_bool> stop;
  QFutureWatcher<Count> worker;
  qint64 requested = 0;
  int numberWidth() const {
    auto digits =
        QString::number(qMax<qint64>(1, first) + blockCount() - 1).size();
    return qMax(48, fontMetrics().horizontalAdvance('9') * int(digits) + 18);
  }
  void layoutColumns() {
    int w = numberWidth(), h = header.sizeHint().height();
    if (viewportMargins() != QMargins(w, h, 0, 0))
      setViewportMargins(w, h, 0, 0);
    header.columns(w, "日志内容");
    gutter.setGeometry(0, viewport()->y(), w, viewport()->height());
    gutter.update();
  }
  void paintNumbers(QWidget *target) {
    QPainter p(target);
    p.setFont(font());
    p.fillRect(target->rect(), QColor("#f2f4f4"));
    p.setPen(QColor("#e3e1da"));
    p.drawLine(target->width() - 1, 0, target->width() - 1, target->height());
    for (auto b = firstVisibleBlock(); b.isValid(); b = b.next()) {
      auto y = blockBoundingGeometry(b).translated(contentOffset()).top();
      if (y >= target->height())
        break;
      if (!b.isVisible())
        continue;
      p.setPen(QColor("#52524d"));
      p.drawText(QRectF(2, y, target->width() - 10, fontMetrics().height()),
                 Qt::AlignRight | Qt::AlignVCenter,
                 first > 0 ? QString::number(first + b.blockNumber())
                           : QStringLiteral("…"));
    }
  }
  void schedule() {
    if (stop || first >= 0)
      return;
    requested = offset;
    auto it = checkpoints.upperBound(offset);
    --it;
    auto pos = it.key(), line = it.value(), target = offset;
    auto flag = std::make_shared<std::atomic_bool>(false);
    stop = flag;
    worker.setFuture(
        QtConcurrent::run([fileName = path, pos, line, target, flag]() mutable {
          Count result;
          QFile f(fileName);
          if (!f.open(QIODevice::ReadOnly) || !f.seek(qMax<qint64>(0, pos - 1)))
            return result;
          bool cr = pos > 0 && f.read(1) == "\r";
          constexpr qint64 stride = 1024 * 1024;
          while (pos < target && !flag->load()) {
            auto n = qMin(target - pos, stride - pos % stride);
            auto data = f.read(n);
            if (data.size() != n)
              return Count{};
            line += breaksIn(data, cr);
            cr = data.endsWith('\r');
            pos += n;
            if (pos % stride == 0)
              result.anchors.insert(pos, line);
          }
          if (!flag->load())
            result.line = line;
          return result;
        }));
  }

public:
  NumberedLogPage() {
    setFrameShape(QFrame::NoFrame);
    connect(this, &QPlainTextEdit::updateRequest, this,
            [this] { gutter.update(); });
    connect(&worker, &QFutureWatcher<Count>::finished, this, [this] {
      auto result = worker.result();
      bool cancelled = stop->load();
      stop.reset();
      if (!cancelled && result.line > 0) {
        checkpoints.insert(result.anchors);
        if (requested == offset)
          first = result.line;
      }
      layoutColumns();
      if (cancelled || requested != offset)
        schedule();
    });
  }
  ~NumberedLogPage() override {
    if (stop)
      stop->store(true);
  }
  qint64 firstFileLine() const { return first; }
  void confirmFirstLine(qint64 at, qint64 line) {
    if (offset == at) {
      if (stop)
        stop->store(true);
      first = line;
      layoutColumns();
    }
  }
  void setPage(const QString &fileName, qint64 next, const QByteArray &raw,
               const QString &text) {
    QFileInfo info(fileName);
    bool same = (fileName == path && info.birthTime() == birth &&
                 (info.size() > size ||
                  (info.size() == size && info.lastModified() == modified)));
    qint64 base = -1;
    if (!same) {
      checkpoints = {{0, 1}};
      if (stop)
        stop->store(true);
    }
    if (next == 0)
      base = 1;
    else if (same && first > 0) {
      if (next >= offset && next - offset <= pageBytes.size())
        base = first +
               breaksIn(QByteArrayView(pageBytes).first(next - offset), false);
      else if (next < offset && offset - next <= raw.size())
        base =
            first - breaksIn(QByteArrayView(raw).first(offset - next), false);
    }
    path = fileName;
    birth = info.birthTime();
    modified = info.lastModified();
    size = info.size();
    offset = next;
    first = base;
    pageBytes = raw;
    setPlainText(text);
    layoutColumns();
    schedule();
  }

protected:
  void resizeEvent(QResizeEvent *event) override {
    QPlainTextEdit::resizeEvent(event);
    layoutColumns();
  }
};
