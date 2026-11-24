#pragma once
#include "logstyle.h"
#include <Qsci/qscilexercustom.h>
#include <Qsci/qsciscintilla.h>
class LogLexer final : public QsciLexerCustom {
public:
  using QsciLexerCustom::QsciLexerCustom;
  const char *language() const override { return "日志"; }
  QString description(int style) const override {
    static const QStringList names = {"正文", "时间", "调试", "信息",
                                      "警告", "错误", "来源", "字段"};
    return names.value(style);
  }
  QColor defaultColor(int style) const override {
    return style >= 0 && style <= LogStyle::Key ? LogStyle::ink(style)
                                                : LogStyle::ink(0);
  }
  void styleText(int start, int end) override {
    if (!editor() || end <= start)
      return;
    auto bytes = editor()->bytes(start, end);
    bytes.truncate(end - start);
    const QString text = QString::fromUtf8(bytes);
    startStyling(start);
    int consumed = 0;
    auto span = [&](int n, int style) {
      if (n > 0)
        setStyling(n, style);
    };
    LogStyle::tokens(text, [&](int p, int n, int kind) {
      span(QStringView(text).mid(consumed, p - consumed).toUtf8().size(), 0);
      span(QStringView(text).mid(p, n).toUtf8().size(), kind);
      consumed = p + n;
    });
    span(QStringView(text).mid(consumed).toUtf8().size(), 0);
  }
};
