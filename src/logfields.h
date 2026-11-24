#pragma once
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

inline const QRegularExpression &logClock() {
  static const QRegularExpression clock(QStringLiteral(R"(^\s*\[?(?:\d{4}[-/]\d{2}[-/]\d{2}[ T])?(\d{2}:\d{2}:\d{2})(?!\d))"));
  return clock;
}

// Compare the calendar date and clock printed in the log, independent of the workstation's zone.
inline qint64 logDateTimeKey(const QString &value) {
  static const QRegularExpression pattern(QStringLiteral(
      R"(^\s*\[?(\d{4})[-/](\d{2})[-/](\d{2})[ T](\d{2}):(\d{2}):(\d{2})(?:[.,](\d{1,9}))?(?!\d))"));
  const auto m = pattern.match(value);
  if (!m.hasMatch()) return -1;
  const QDate date(m.captured(1).toInt(), m.captured(2).toInt(), m.captured(3).toInt());
  const QTime time(m.captured(4).toInt(), m.captured(5).toInt(), m.captured(6).toInt(),
                   m.captured(7).left(3).leftJustified(3, '0').toInt());
  return date.isValid() && time.isValid()
             ? date.toJulianDay() * 86400000 + time.msecsSinceStartOfDay() : -1;
}

inline bool matchesLogFields(const QString &line, const QJsonObject &wanted) {
  QJsonObject fields;
  const bool json = QStringView(line).trimmed().startsWith('{');
  if (json)
    fields = QJsonDocument::fromJson(line.toUtf8()).object();
  if (fields.isEmpty()) {
    // A plain-text field's exact value must appear literally in the record.
    // Reject sparse matches before extracting every key from every line.
    for (auto i = wanted.begin(); i != wanted.end(); ++i) {
      auto sensitivity = i.key() == "level" ? Qt::CaseInsensitive : Qt::CaseSensitive;
      if (!line.contains(i.value().toString(), sensitivity))
        return false;
    }
    static const QRegularExpression kv(QStringLiteral(
        R"rx((?:^|[\s|,;])([\w.-]+)\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s,;|]+)))rx"));
    auto pairs = kv.globalMatch(line);
    while (pairs.hasNext()) {
      const auto m = pairs.next();
      fields.insert(m.captured(1), m.captured(m.lastCapturedIndex()));
    }
    const auto parts = line.split('|', Qt::KeepEmptyParts);
    if (parts.size() >= 3 && logClock().match(parts[0]).hasMatch()) {
      if (!fields.contains("source"))
        fields.insert("source", parts[1].trimmed());
      if (!fields.contains("level"))
        fields.insert("level", parts[2].trimmed());
    }
    if (!fields.contains("level")) {
      static const QRegularExpression level(
          R"(^\s*(?:\[?(?:\d{4}[-/]\d{2}[-/]\d{2}[ T])?\d{2}:\d{2}:\d{2}(?:[.,]\d+)?(?:Z|[+-]\d{2}:?\d{2})?\]?\s+)?\[?(TRACE|DEBUG|INFO|NOTICE|WARN(?:ING)?|ERROR|FATAL|CRITICAL)\b)",
          QRegularExpression::CaseInsensitiveOption);
      fields.insert("level", level.match(line).captured(1));
    }
  }
  for (auto i = wanted.begin(); i != wanted.end(); ++i) {
    if (fields.value(i.key()).toVariant().toString().compare(
            i.value().toString(), i.key() == "level" ? Qt::CaseInsensitive : Qt::CaseSensitive))
      return false;
  }
  return true;
}

inline QString sessionPattern(const QString &id) {
  return QStringLiteral(R"rx((?<![\w.-])(?:"session_id"\s*:\s*"(%1)"|session_id\s*=\s*(%1)(?![\w.:@/-])))rx").arg(id);
}

inline QString sessionAt(const QString &line, int column) {
  static const QRegularExpression field(sessionPattern("[\\w.:@/-]+"));
  auto matches = field.globalMatch(line);
  while (matches.hasNext()) {
    auto m = matches.next();
    if (column >= m.capturedStart() && column < m.capturedEnd())
      return m.captured(m.capturedStart(1) >= 0 ? 1 : 2);
  }
  return {};
}
