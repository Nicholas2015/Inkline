#pragma once
#include <QRegularExpression>

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
