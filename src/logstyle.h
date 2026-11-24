#pragma once
#include <QRegularExpression>
#include <QSyntaxHighlighter>
#include <array>

namespace LogStyle {
enum Token { Text, Time, Debug, Info, Warn, Error, Source, Key };
inline QColor ink(int token) {
  static const std::array<QColor, 8> colors = {"#292e31", "#596975", "#6d5180",
                                               "#226662", "#895307", "#ac363b",
                                               "#375b78", "#536476"};
  return colors.at(token);
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
    } else if (m.capturedStart(3) >= 0)
      kind = Source;
    else if (m.capturedStart(4) >= 0)
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
      if (kind >= Info && kind <= Error)
        f.setFontWeight(QFont::DemiBold);
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
