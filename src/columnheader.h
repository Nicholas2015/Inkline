#pragma once
#include <QFontDatabase>
#include <QPainter>
#include <QWidget>
class ColumnHeader final : public QWidget {
  int gutter = 0;
  QString caption;

public:
  explicit ColumnHeader(QWidget *parent) : QWidget(parent) {
    setFont(QFontDatabase::systemFont(QFontDatabase::GeneralFont));
    setAccessibleName("行号栏与内容栏");
    setAttribute(Qt::WA_TransparentForMouseEvents);
  }
  void columns(int width, const QString &title) {
    gutter = width;
    caption = title;
    setGeometry(0, 0, parentWidget()->width(), sizeHint().height());
    update();
  }
  QSize sizeHint() const override {
    return {0, qMax(22, fontMetrics().height() + 6)};
  }

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter p(this);
    p.fillRect(rect(), QColor("#faf9f6"));
    p.setPen(QColor("#52524d"));
    p.drawText(QRect(0, 0, gutter - 7, height()),
               Qt::AlignRight | Qt::AlignVCenter, "行号");
    p.drawText(rect().adjusted(gutter + 8, 0, 0, 0),
               Qt::AlignLeft | Qt::AlignVCenter, caption);
    p.setPen(QColor("#e3e1da"));
    p.drawLine(gutter - 1, 0, gutter - 1, height());
    p.drawLine(0, height() - 1, width(), height() - 1);
  }
};
