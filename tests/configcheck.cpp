#include "configtools.h"
#include <QCoreApplication>
#include <cstdio>

using namespace ConfigTools;

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  int failures = 0;
  auto check = [&](bool ok, const char *name) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", name); ++failures; }
  };
  auto format = [&](Format type, const QString &input, const QString &expected, const char *name) {
    const auto result = process(type, input, true);
    check(result.valid() && result.text == expected, name);
    check(process(type, result.text, true).text == result.text, "format is idempotent");
  };
  auto invalid = [&](Format type, const QString &input, int line, int column, const char *name) {
    const auto result = process(type, input, true);
    check(!result.valid() && result.text.isEmpty() && result.line == line && result.column == column, name);
  };
  format(Format::Json, R"({"x":18446744073709551617,"x":[1e999,"\u0041"]})",
         "{\n  \"x\": 18446744073709551617,\n  \"x\": [\n    1e999,\n    \"\\u0041\"\n  ]\n}", "exact JSON spelling");
  format(Format::Json, " 3.1415900\r\n", "3.1415900\r\n", "scalar and final CRLF");
  invalid(Format::Json, "[\r\n  1,]", 2, 5, "trailing comma");
  invalid(Format::Json, QStringLiteral("[\U0001F600]"), 1, 2, "unexpected value");
  invalid(Format::Json, QStringLiteral("[\"\U0001F600\",?]"), 1, 6, "Unicode diagnostic column");
  invalid(Format::Json, "01", 1, 2, "leading zero");
  invalid(Format::Json, "{\"x\":1e}", 1, 8, "missing exponent");
  const QString yaml = QStringLiteral("# 文\r\nname:   \"x,#:y\" # keep\r\nitems: [a,b]\r\nliteral: |\r\n  x:   y # body\r\n");
  format(Format::Yaml, yaml, QStringLiteral("# 文\r\nname: \"x,#:y\" # keep\r\nitems: [a, b]\r\nliteral: |\r\n  x:   y # body\r\n"), "YAML source preservation");
  format(Format::Yaml, QStringLiteral("\U0001F600:   42\n---\nx: &a [1,2]\ny: *a\n"),
         QStringLiteral("\U0001F600: 42\n---\nx: &a [1, 2]\ny: *a\n"), "YAML streams and Unicode columns");
  invalid(Format::Yaml, "a: 1\nb: *missing", 2, 4, "unknown YAML alias");
  check(!process(Format::Yaml, "a: [1,", false).valid(), "unclosed YAML flow sequence");
  const QString toml = "# x=y\na= [1,2, 3] # tail\nb= \"\"\"abc\n#x=y\nzz\"\"\"\n'k=1'= 'a,b'\n";
  format(Format::Toml, toml, "# x=y\na = [1, 2, 3] # tail\nb = \"\"\"abc\n#x=y\nzz\"\"\"\n'k=1' = 'a,b'\n", "TOML comments and multiline strings");
  format(Format::Toml, "s=\"\"\"line\"k\"\",=\"\"\"\"\"\n", "s = \"\"\"line\"k\"\",=\"\"\"\"\"\n", "TOML multiline terminal quotes");
  check(!process(Format::Toml, "x=1\nx=2", false).valid(), "duplicate TOML key");
  const auto broken = process(Format::Toml, "x=1\ny=]\n", true);
  check(!broken.valid() && broken.line == 2 && broken.column == 3, "TOML diagnostic");
  format(Format::Yaml, "root:\n    child:\n        flag:   yes\n", "root:\n  child:\n    flag: yes\n", "canonical YAML block indentation");
  const QString deep = QString(499, '[') + "[" + QString("0,").repeated(40000) + "0]" + QString(499, ']');
  const auto bounded = process(Format::Json, deep, true);
  check(!bounded.valid() && bounded.line == 0 && bounded.text.isEmpty(), "bound JSON formatting expansion");
  return failures != 0;
}
