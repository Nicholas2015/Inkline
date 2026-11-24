#pragma once
#include <QApplication>
#include <QFile>
#include <QFontDatabase>
#include <QPainter>
#include <QProxyStyle>
#include <QStyleOption>

namespace Appearance {
inline QFont codeFont() {
  QFont f = QFontDatabase::systemFont(QFontDatabase::FixedFont);
  f.setFamilies({"Cascadia Mono", "Menlo", "Consolas", f.family()});
  f.setPointSize(11);
  return f;
}
enum class Symbol { New, Open, Save, Search, Suggest };
inline QIcon icon(Symbol id) {
  QIcon result;
  for (int scale : {1, 2, 3}) {
    QPixmap pix(16 * scale, 16 * scale);
    pix.fill(Qt::transparent);
    QPainter p(&pix);
    p.scale(scale, scale);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(QColor("#44494d"), 1.35, Qt::SolidLine, Qt::RoundCap,
                  Qt::RoundJoin));
    auto line = [&](std::initializer_list<QPointF> points) {
      p.drawPolyline(points.begin(), int(points.size()));
    };
    switch (id) {
    case Symbol::New:
      line({{3, 1.5}, {9, 1.5}, {13, 5.5}, {13, 14.5}, {3, 14.5}, {3, 1.5}});
      line({{9, 2}, {9, 5.5}, {12.5, 5.5}});
      break;
    case Symbol::Open:
      line({{1.5, 12}, {1.5, 3}, {6, 3}, {8, 5}, {13, 5}});
      line({{1.5, 13}, {4.5, 7}, {15, 7}, {12.5, 13}, {1.5, 13}});
      break;
    case Symbol::Save:
      line({{2, 2}, {11, 2}, {14, 5}, {14, 14}, {2, 14}, {2, 2}});
      line({{5, 2}, {5, 6}, {10, 6}, {10, 2}});
      line({{5, 14}, {5, 9}, {11, 9}, {11, 14}});
      break;
    case Symbol::Search:
      p.drawEllipse(QRectF(2, 2, 8.5, 8.5));
      line({{9.5, 9.5}, {14, 14}});
      break;
    case Symbol::Suggest:
      line({{3, 6}, {1, 8}, {3, 10}});
      line({{13, 6}, {15, 8}, {13, 10}});
      line({{9.5, 3}, {6.5, 13}});
      break;
    }
    p.end();
    pix.setDevicePixelRatio(scale);
    result.addPixmap(pix);
  }
  return result;
}
class ChromeStyle final : public QProxyStyle {
public:
  ChromeStyle() : QProxyStyle("Fusion") {}
  void drawPrimitive(PrimitiveElement element, const QStyleOption *opt,
                     QPainter *p,
                     const QWidget *widget = nullptr) const override {
    if (element != PE_IndicatorTabClose) {
      QProxyStyle::drawPrimitive(element, opt, p, widget);
      return;
    }
    p->save();
    p->setRenderHint(QPainter::Antialiasing);
    p->translate(opt->rect.center() + QPointF(.5, .5));
    if (opt->state & State_MouseOver) {
      p->setPen(Qt::NoPen);
      p->setBrush(QColor("#e0e2e3"));
      p->drawRoundedRect(QRectF(-7, -7, 14, 14), 3, 3);
    }
    p->setPen(QPen(QColor("#60666a"), 1.35, Qt::SolidLine, Qt::RoundCap));
    p->drawLine(QPointF(-3, -3), QPointF(3, 3));
    p->drawLine(QPointF(3, -3), QPointF(-3, 3));
    p->restore();
  }
};
inline void apply(QApplication &app) {
  app.setStyle(new ChromeStyle);
  QFont ui = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
#ifdef Q_OS_WIN
  ui.setFamilies({"Segoe UI", "Microsoft YaHei UI"});
#elif defined(Q_OS_MACOS)
  ui.setFamilies({ui.family(), "PingFang SC"});
#endif
  ui.setPointSize(10);
  app.setFont(ui);
  auto palette = app.style()->standardPalette();
  for (auto [role, hex] : {std::pair{QPalette::Window, "#f6f6f5"},
                           {QPalette::WindowText, "#292d30"},
                           {QPalette::Base, "#ffffff"},
                           {QPalette::AlternateBase, "#f7f8f7"},
                           {QPalette::Text, "#292d30"},
                           {QPalette::Button, "#f5f5f4"},
                           {QPalette::ButtonText, "#353a3c"},
                           {QPalette::Highlight, "#475862"},
                           {QPalette::HighlightedText, "#ffffff"},
                           {QPalette::PlaceholderText, "#70777b"}})
    palette.setColor(role, QColor(hex));
  palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#85898c"));
  app.setPalette(palette);
  QFile theme(":/graphite.qss");
  if (theme.open(QIODevice::ReadOnly))
    app.setStyleSheet(QString::fromUtf8(theme.readAll()));
}
} // namespace Appearance
