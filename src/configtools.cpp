#include "configtools.h"
#include <toml.hpp>
#include <yaml.h>
#include <QCryptographicHash>
#include <QSet>


namespace ConfigTools {
namespace {
constexpr qsizetype formatLimit = 32 * 1024 * 1024;
Result tooLarge() { return {{}, "格式化结果超过编辑器的 32 MiB 限制", 0, 0}; }

bool horizontal(QChar c) { return c == ' ' || c == '\t'; }
bool digit(QChar c) { return c >= '0' && c <= '9'; }

// Parse the JSON grammar against source tokens. Numbers, duplicate members and
// string escapes retain their spelling even outside the machine numeric range.
class Json {
  const QString &s;
  bool pretty;
  qsizetype p = 0;
  QString out, eol;
  Result result;
  QChar peek() const { return p < s.size() ? s[p] : QChar(); }
  void whitespace() {
    while (horizontal(peek()) || peek() == '\n' || peek() == '\r') ++p;
  }
  bool fail(const QString &msg) {
    result.error = msg;
    for (qsizetype i = 0; i < p; ++i) {
      if (s[i] == '\r' || s[i] == '\n') {
        if (s[i] == '\r' && i + 1 < p && s[i + 1] == '\n') ++i;
        ++result.line;
        result.column = 1;
      } else if (!s[i].isLowSurrogate()) ++result.column;
    }
    return false;
  }
  void copy(qsizetype start) {
    if (pretty) out += QStringView(s).sliced(start, p - start);
  }
  bool indent(int depth) {
    if (!pretty) return true;
    if (out.size() + eol.size() + depth * 2 > formatLimit) {
      result = tooLarge();
      return false;
    }
    out += eol + QString(depth * 2, ' ');
    return true;
  }
  bool string() {
    auto start = p++;
    while (p < s.size()) {
      auto c = s[p];
      if (c == '"') { ++p; copy(start); return true; }
      if (c.unicode() < 0x20) return fail("字符串内含未转义的控制字符");
      ++p;
      if (c != '\\') continue;
      if (p >= s.size()) break;
      if (peek() == 'u') {
        ++p;
        for (int i = 0; i < 4; ++i) {
          auto h = peek().toLower();
          if (!digit(h) && !(h >= 'a' && h <= 'f')) return fail("需要四位十六进制 Unicode 转义");
          ++p;
        }
      } else if (QStringView(u"\"\\/bfnrt").contains(peek())) ++p;
      else return fail("无效的字符串转义");
    }
    return fail("字符串缺少结束引号");
  }
  bool number() {
    auto start = p;
    if (peek() == '-') ++p;
    if (!digit(peek())) return fail("需要数字");
    if (peek() == '0') ++p;
    else while (digit(peek())) ++p;
    if (peek() == '.') {
      ++p;
      if (!digit(peek())) return fail("小数点后需要数字");
      while (digit(peek())) ++p;
    }
    if (peek() == 'e' || peek() == 'E') {
      ++p;
      if (peek() == '+' || peek() == '-') ++p;
      if (!digit(peek())) return fail("指数需要数字");
      while (digit(peek())) ++p;
    }
    copy(start);
    return true;
  }
  bool value(int depth) {
    whitespace();
    if (peek() == '"') return string();
    if (peek() == '-' || digit(peek())) return number();
    for (auto literal : {QStringView(u"true"), QStringView(u"false"), QStringView(u"null")}) {
      if (QStringView(s).sliced(p).startsWith(literal)) {
        p += literal.size();
        if (pretty) out += literal;
        return true;
      }
    }
    const bool object = peek() == '{';
    if (!object && peek() != '[') return fail("需要 JSON 值");
    if (depth >= 512) return fail("嵌套层数超过 512");
    QChar close = object ? '}' : ']';
    if (pretty) out += peek();
    ++p;
    whitespace();
    if (peek() != close) {
      for (;;) {
        if (!indent(depth + 1)) return false;
        if (object) {
          if (peek() != '"') return fail("对象成员名需要双引号");
          if (!string()) return false;
          whitespace();
          if (peek() != ':') return fail("成员名后需要冒号");
          ++p;
          if (pretty) out += ": ";
        }
        if (!value(depth + 1)) return false;
        whitespace();
        if (peek() == close) break;
        if (peek() != ',') return fail(QString("需要逗号或 %1").arg(close));
        ++p;
        if (pretty) out += ',';
        whitespace();
      }
      if (!indent(depth)) return false;
    }
    ++p;
    if (pretty) out += close;
    return true;
  }
public:
  Json(const QString &source, bool format) : s(source), pretty(format) {
    eol = s.contains("\r\n") ? "\r\n" : s.contains('\r') ? "\r" : "\n";
  }
  Result run() {
    if (!value(0)) return result;
    whitespace();
    if (p != s.size()) { fail("JSON 值后有多余内容"); return result; }
    if (pretty && (s.endsWith('\n') || s.endsWith('\r'))) out += eol;
    result.text = pretty ? out : s;
    return result;
  }
};

Result tomlDocument(const QString &s, bool pretty) {
  const auto utf8 = s.toUtf8();
  try {
    (void)toml::parse(std::string_view(utf8.constData(), size_t(utf8.size())));
  } catch (const toml::parse_error &e) {
    const auto pos = e.source().begin;
    return {{}, QString::fromUtf8(e.description().data(), qsizetype(e.description().size())), int(pos.line), int(pos.column)};
  }
  if (!pretty) return {s, {}};
  QString out;
  qsizetype i = 0;
  while (i < s.size()) {
    auto c = s[i];
    if (c == '#') {
      const auto start = i++;
      while (i < s.size() && s[i] != '\n' && s[i] != '\r') ++i;
      out += QStringView(s).sliced(start, i - start);
    } else if (c == '"' || c == '\'') {
      const auto start = i;
      const bool multi = QStringView(s).sliced(i).startsWith(QString(3, c));
      i += multi ? 3 : 1;
      while (i < s.size()) {
        if (s[i] == '\\' && c == '"') { i += 2; continue; }
        if (s[i++] != c) continue;
        if (!multi) break;
        const auto quoteStart = i - 1;
        while (i < s.size() && s[i] == c) ++i;
        if (i - quoteStart >= 3) break;
      }
      out += QStringView(s).sliced(start, i - start);
    } else if (c == '=' || c == ',') {
      while (!out.isEmpty() && horizontal(out.back())) out.chop(1);
      if (c == '=') out += ' ';
      out += c;
      ++i;
      while (i < s.size() && horizontal(s[i])) ++i;
      if (i < s.size() && !QStringView(u"\r\n]}").contains(s[i])) out += ' ';
    } else { out += c; ++i; }
  }
  return {out, {}};
}

struct YamlParser {
  yaml_parser_t parser{};
  bool ready;
  explicit YamlParser(const QByteArray &bytes) : ready(yaml_parser_initialize(&parser)) {
    if (ready) yaml_parser_set_input_string(&parser,
      reinterpret_cast<const unsigned char *>(bytes.constData()), size_t(bytes.size()));
  }
  ~YamlParser() { if (ready) yaml_parser_delete(&parser); }
  Result error() const {
    return {{}, QString::fromUtf8(parser.problem ? parser.problem : "YAML parser initialization failed"),
            int(parser.problem_mark.line + 1), int(parser.problem_mark.column + 1)};
  }
};

// Event data also lets us verify that changing presentation leaves all scalar
// values, tags, anchors and collection boundaries intact.
Result yamlEvents(const QByteArray &bytes, QByteArray *digest) {
  YamlParser reader(bytes);
  if (!reader.ready) return reader.error();
  QSet<QByteArray> anchors;
  QCryptographicHash hash(QCryptographicHash::Sha256);
  auto add = [&](QByteArrayView field) {
    const quint64 size = quint64(field.size());
    hash.addData(QByteArrayView(reinterpret_cast<const char *>(&size), sizeof(size)));
    hash.addData(field);
  };
  for (;;) {
    yaml_event_t event;
    if (!yaml_parser_parse(&reader.parser, &event)) return reader.error();
    const auto type = event.type;
    const yaml_char_t *anchor = nullptr, *tag = nullptr;
    QByteArrayView value;
    switch (type) {
    case YAML_DOCUMENT_START_EVENT: anchors.clear(); break;
    case YAML_ALIAS_EVENT: anchor = event.data.alias.anchor; break;
    case YAML_SCALAR_EVENT:
      anchor = event.data.scalar.anchor; tag = event.data.scalar.tag;
      if (digest) value = QByteArrayView(reinterpret_cast<const char *>(event.data.scalar.value), event.data.scalar.length);
      break;
    case YAML_MAPPING_START_EVENT: anchor = event.data.mapping_start.anchor; tag = event.data.mapping_start.tag; break;
    case YAML_SEQUENCE_START_EVENT: anchor = event.data.sequence_start.anchor; tag = event.data.sequence_start.tag; break;
    default: break;
    }
    QByteArray name(reinterpret_cast<const char *>(anchor));
    const bool unknown = type == YAML_ALIAS_EVENT && !anchors.contains(name);
    if (anchor && type != YAML_ALIAS_EVENT) anchors.insert(name);
    if (digest) {
      const char kind = char(type);
      add(QByteArrayView(&kind, 1));
      add(name);
      add(QByteArrayView(reinterpret_cast<const char *>(tag)));
      add(value);
    }
    auto mark = event.start_mark;
    yaml_event_delete(&event);
    if (unknown) return {{}, "未定义的 YAML 别名 " + QString::fromUtf8(name), int(mark.line + 1), int(mark.column + 1)};
    if (type == YAML_STREAM_END_EVENT) {
      if (digest) *digest = hash.result();
      return {};
    }
  }
}

QString yamlIndent(const QString &s) {
  QString out;
  QList<qsizetype> levels{0};
  for (qsizetype start = 0; start < s.size();) {
    auto body = start;
    while (body < s.size() && s[body] == ' ') ++body;
    auto end = body;
    while (end < s.size() && !QStringView(u"\n\r\u0085\u2028\u2029").contains(s[end])) ++end;
    const bool structural = body < end && s[body] != '#';
    auto indent = body - start;
    if (structural) {
      while (levels.size() > 1 && levels.last() > indent) levels.removeLast();
      if (levels.last() < indent) levels.append(indent);
    }
    auto level = structural ? levels.size() - 1 : levels.indexOf(indent);
    if (level >= 0 && body < end) out += QString(level * 2, ' ');
    else out += QStringView(s).sliced(start, indent);
    if (end < s.size()) ++end;
    out += QStringView(s).sliced(body, end - body);
    start = end;
  }
  return out;
}

Result yamlDocument(const QString &s, bool pretty) {
  const auto bytes = s.toUtf8();
  QByteArray original;
  auto checked = yamlEvents(bytes, pretty ? &original : nullptr);
  if (!checked.valid()) return checked;
  if (!pretty) return {s, {}};
  YamlParser reader(bytes);
  if (!reader.ready) return reader.error();
  QString out;
  qsizetype copied = 0, offset = 0;
  size_t codepoint = 0;
  bool simpleIndent = true;
  for (;;) {
    yaml_token_t token;
    if (!yaml_parser_scan(&reader.parser, &token)) return reader.error();
    const auto type = token.type;
    const auto end = token.end_mark.index;
    if (type == YAML_SCALAR_TOKEN && token.start_mark.line != token.end_mark.line) simpleIndent = false;
    yaml_token_delete(&token);
    if (type == YAML_STREAM_END_TOKEN) break;
    if (type != YAML_VALUE_TOKEN && type != YAML_BLOCK_ENTRY_TOKEN && type != YAML_FLOW_ENTRY_TOKEN) continue;
    while (codepoint < end && offset < s.size()) {
      if (s[offset++].isHighSurrogate() && offset < s.size() && s[offset].isLowSurrogate()) ++offset;
      ++codepoint;
    }
    if (offset <= copied || offset >= s.size()) continue;
    auto next = offset;
    while (next < s.size() && horizontal(s[next])) ++next;
    if (next == s.size() || QStringView(u"\r\n\u0085\u2028\u2029,]}").contains(s[next])) continue;
    out += QStringView(s).sliced(copied, offset - copied);
    out += ' ';
    copied = next;
  }
  out += QStringView(s).sliced(copied);
  if (simpleIndent) {
    auto indented = yamlIndent(out);
    if (indented != out) {
      QByteArray after;
      if (yamlEvents(indented.toUtf8(), &after).valid() && after == original) return {indented, {}};
    }
  }
  if (out != s) {
    QByteArray after;
    if (!yamlEvents(out.toUtf8(), &after).valid() || after != original) return {s, {}};
  }
  return {out, {}};
}
}

Result process(Format format, const QString &source, bool pretty) {
  if (pretty && source.size() > formatLimit) return tooLarge();
  Result result;
  switch (format) {
  case Format::Json: result = Json(source, pretty).run(); break;
  case Format::Yaml: result = yamlDocument(source, pretty); break;
  case Format::Toml: result = tomlDocument(source, pretty); break;
  }
  if (pretty && (result.text.size() > formatLimit ||
                 (result.text.size() > formatLimit / 3 && result.text.toUtf8().size() > formatLimit)))
    return tooLarge();
  return result;
}
}
