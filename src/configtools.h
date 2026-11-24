#pragma once
#include <QString>

namespace ConfigTools {
enum class Format { Json, Yaml, Toml };
struct Result {
  QString text;
  QString error;
  int line = 1;
  int column = 1; // One-based Unicode character column.
  bool valid() const { return error.isEmpty(); }
};
Result process(Format format, const QString &source, bool pretty);
}
