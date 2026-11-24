#pragma once
#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <array>

namespace LogStyle {
enum Token { Text, Time, Debug, Info, Warn, Error, Source, Key, Function };
inline QColor ink(int token) {
  static const std::array<QColor, 9> colors = {"#292e31", "#536571", "#542194",
                                               "#005640", "#703c00", "#91162f",
                                               "#475b67", "#536476", "#164e76"};
  return colors.at(token);
}
inline QColor levelBackground(int token) {
  switch (token) {
  case Debug:
    return QColor("#ebe0f7");
  case Info:
    return QColor("#d4ecdf");
  case Warn:
    return QColor("#ffdc88");
  case Error:
    return QColor("#ffb7c2");
  default:
    return {};
  }
}
inline QFont::Weight weight(int token) {
  if (token >= Debug && token <= Error)
    return QFont::Bold;
  return token == Function ? QFont::DemiBold : QFont::Normal;
}
// Match structured fields rather than tinting the entire record.
template <typename Visitor> void tokens(const QString &s, Visitor visit) {
  static const QRegularExpression rx(
      QStringLiteral(
          R"(\b(\d{4}[-/]\d{2}[-/]\d{2}[ T])?\d{2}:\d{2}:\d{2}(?:[.,]\d{1,9})?(?:Z|[+-]\d{2}:?\d{2})?\b|\b(TRACE|DEBUG|INFO|NOTICE|WARN(?:ING)?|ERROR|FATAL|CRITICAL)\b|\b([a-z_][\w.]{0,160}:[a-z_][\w.]{0,100}:\d{1,9})\b|\b([a-z_][\w.-]{0,64})(?==))"),
      QRegularExpression::CaseInsensitiveOption);
  auto matches = rx.globalMatch(s);
  while (matches.hasNext()) {
    auto m = matches.next();
    int kind = Time;
    if (m.capturedStart(2) >= 0) {
      const auto level = m.captured(2).toUpper();
      kind = (level == "TRACE" || level == "DEBUG")   ? Debug
             : (level == "INFO" || level == "NOTICE") ? Info
             : level.startsWith("WARN")               ? Warn
                                                      : Error;
    } else if (m.capturedStart(3) >= 0) {
      const auto source = m.capturedView(3);
      const auto left = source.indexOf(':') + 1;
      const auto right = source.lastIndexOf(':');
      visit(m.capturedStart(), left, Source);
      visit(m.capturedStart() + left, right - left, Function);
      visit(m.capturedStart() + right, source.size() - right, Source);
      continue;
    } else if (m.capturedStart(4) >= 0)
      kind = Key;
    visit(m.capturedStart(), m.capturedLength(), kind);
  }
}
inline const std::array<QColor, 5> keywordColors = {
    "#FFE1A1", "#B8DEFF", "#E3CCF4", "#BFE8C8", "#FFCDCA"};
class Layer final : public QSyntaxHighlighter {
  QStringList words;

public:
  explicit Layer(QTextDocument *doc) : QSyntaxHighlighter(doc) {}
  void setWords(const QStringList &value) {
    words = value;
    rehighlight();
  }
  void highlightBlock(const QString &s) override {
    tokens(s, [this](int pos, int n, int kind) {
      QTextCharFormat f;
      f.setForeground(ink(kind));
      const auto paper = levelBackground(kind);
      if (paper.isValid())
        f.setBackground(paper);
      f.setFontWeight(weight(kind));
      setFormat(pos, n, f);
    });
    for (qsizetype i = 0; i < words.size(); ++i) {
      const auto &word = words[i];
      if (word.isEmpty())
        continue;
      qsizetype pos = 0;
      while ((pos = s.indexOf(word, pos)) >= 0) {
        const int end = pos + word.size();
        while (pos < end) {
          auto f = format(pos);
          int next = pos + 1;
          while (next < end && format(next) == f)
            ++next;
          f.setBackground(keywordColors[i % keywordColors.size()]);
          setFormat(pos, next - pos, f);
          pos = next;
        }
      }
    }
  }
};
} // namespace LogStyle
