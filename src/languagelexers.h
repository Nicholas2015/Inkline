#pragma once
#include <Qsci/qscilexercpp.h>
#include <Qsci/qscilexercustom.h>
#include <Qsci/qsciscintilla.h>
#include <iterator>

// Go and Zig share the C-family punctuation and comment rules.
class BracedLexer final : public QsciLexerCPP {
  bool go;
public:
  BracedLexer(QObject *parent, bool isGo) : QsciLexerCPP(parent), go(isGo) {}
  const char *language() const override { return go ? "Go" : "Zig"; }
  const char *keywords(int set) const override {
    if (set != 1) return nullptr;
    return go ? "break default func interface select case defer go map struct chan else goto package switch const fallthrough if range type continue for import return var bool byte complex64 complex128 error float32 float64 int int8 int16 int32 int64 rune string uint uint8 uint16 uint32 uint64 uintptr any comparable true false nil iota"
              : "addrspace align allowzero and anyframe anytype asm async await break callconv catch comptime const continue defer else enum errdefer error export extern fn for if inline linksection noalias noinline nosuspend opaque or orelse packed pub resume return struct suspend switch test threadlocal try union unreachable usingnamespace var volatile while null undefined true false void bool usize isize u8 u16 u32 u64 i8 i16 i32 i64 f32 f64";
  }
  void refreshProperties() override {
    QsciLexerCPP::refreshProperties();
    emit propertyChanged("lexer.cpp.backquoted.strings", go ? "1" : "0");
  }
};

class RustLexer final : public QsciLexer {
public:
  using QsciLexer::QsciLexer;
  const char *language() const override { return "Rust"; }
  const char *lexer() const override { return "rust"; }
  const char *keywords(int set) const override {
    if (set == 1) return "as async await break const continue crate dyn else enum extern false fn for if impl in let loop match mod move mut pub ref return self Self static struct super trait true type unsafe use where while yield gen";
    if (set == 2) return "bool char str i8 i16 i32 i64 i128 isize u8 u16 u32 u64 u128 usize f32 f64 String Vec Option Result Some None Ok Err";
    return nullptr;
  }
  QString description(int n) const override {
    static const char *const names[] = {"Text","Block comment","Line comment","Doc comment","Line doc","Number","Keyword","Type","Word 3","Word 4","Word 5","Word 6","Word 7","String","Raw string","Character","Operator","Identifier","Lifetime","Macro","Error","Byte string","Raw byte string","Byte character"};
    return n >= 0 && n < int(std::size(names)) ? QString::fromLatin1(names[n]) : QString();
  }
  QColor defaultColor(int n) const override {
    if (n >= 1 && n <= 4) return QColor("#66716c");
    if (n == 5) return QColor("#995018");
    if (n == 6 || n == 19) return QColor("#703fa0");
    if (n == 7 || n == 18) return QColor("#185d94");
    if ((n >= 13 && n <= 15) || n >= 21) return QColor("#236640");
    return QColor("#292d30");
  }
};

class TomlLexer final : public QsciLexerCustom {
  enum Token { Text, Key, Table, String, Number, Literal, Comment };
public:
  using QsciLexerCustom::QsciLexerCustom;
  const char *language() const override { return "TOML"; }
  QString description(int id) const override {
    static const char *const names[] = {"Text","Key","Table","String","Number or date","Boolean","Comment"};
    return id >= Text && id <= Comment ? QString::fromLatin1(names[id]) : QString();
  }
  QColor defaultColor(int id) const override {
    static const char *const colors[] = {"#292d30","#185d94","#60408b","#236640","#995018","#703fa0","#66716c"};
    return QColor(colors[id >= Text && id <= Comment ? id : Text]);
  }
  void styleText(int start, int end) override {
    auto *ed = editor();
    if (!ed || start >= end) return;
    int line = ed->SendScintilla(ed->SCI_LINEFROMPOSITION, start);
    const int first = ed->SendScintilla(ed->SCI_POSITIONFROMLINE, line);
    const int lastLine = ed->SendScintilla(ed->SCI_LINEFROMPOSITION, end - 1);
    int stop = ed->SendScintilla(ed->SCI_POSITIONFROMLINE, lastLine + 1);
    if (stop < 0) stop = ed->length();
    auto bytes = ed->bytes(first, stop);
    bytes.truncate(stop - first);
    startStyling(first);
    char multi = line ? char(ed->SendScintilla(ed->SCI_GETLINESTATE, line - 1)) : 0;
    bool leading = true;
    for (int i = 0; i < bytes.size();) {
      int begin = i;
      char ch = bytes[i];
      Token token = Text;
      if (ch == '\n' || ch == '\r') {
        ++i;
        if (ch == '\r' && i < bytes.size() && bytes[i] == '\n') ++i;
        ed->SendScintilla(ed->SCI_SETLINESTATE, line++, int(multi));
        leading = true;
      } else if (multi || ch == '"' || ch == '\'') {
        const char quote = multi ? multi : ch;
        if (!multi) {
          ++i;
          if (bytes.mid(i, 2) == QByteArray(2, quote)) { multi = quote; i += 2; }
        }
        while (i < bytes.size() && bytes[i] != '\n' && bytes[i] != '\r') {
          if (quote == '"' && bytes[i] == '\\' && i+1 < bytes.size() &&
              bytes[i+1] != '\r' && bytes[i+1] != '\n') { i += 2; continue; }
          if (bytes[i] == quote && (!multi || bytes.mid(i, 3) == QByteArray(3, quote))) {
            i += multi ? 3 : 1;
            multi = 0;
            break;
          }
          ++i;
        }
        int next = i;
        while (next < bytes.size() && (bytes[next]==' ' || bytes[next]=='\t')) ++next;
        token = !multi && next < bytes.size() && bytes[next] == '=' ? Key : String;
        leading = false;
      } else if (ch == '#' || (leading && ch == '[')) {
        token = ch == '#' ? Comment : Table;
        do { ++i; } while (i < bytes.size() && bytes[i]!='\r' && bytes[i]!='\n' &&
                           (token == Comment || bytes[i] != '#'));
        leading = false;
      } else if (QByteArrayView(" \t=[]{},").contains(ch)) {
        ++i;
        if (ch!=' ' && ch!='\t') leading = false;
      } else {
        do { ++i; } while (i < bytes.size() && !QByteArrayView(" \t\r\n=[]{},#\"'").contains(bytes[i]));
        auto word = QByteArrayView(bytes).sliced(begin, i - begin);
        int next = i;
        while (next < bytes.size() && (bytes[next]==' ' || bytes[next]=='\t')) ++next;
        if (next < bytes.size() && bytes[next] == '=') token = Key;
        else if (word == "true" || word == "false") token = Literal;
        else if ((ch >= '0' && ch <= '9') || ch == '+' || ch == '-' || word == "nan" || word == "inf") token = Number;
        leading = false;
      }
      setStyling(i - begin, token);
    }
    if (line <= lastLine) ed->SendScintilla(ed->SCI_SETLINESTATE, line, int(multi));
  }
};

class CLexer final : public QsciLexerCPP {
public:
  using QsciLexerCPP::QsciLexerCPP;
  const char *language() const override { return "C"; }
  const char *keywords(int set) const override {
    return set == 1 ? "auto break case char const continue default do double else enum extern float for goto if inline int long register restrict return short signed sizeof static struct switch typedef union unsigned void volatile while _Alignas _Alignof _Atomic _Bool _Complex _Generic _Imaginary _Noreturn _Static_assert _Thread_local alignas alignof bool constexpr false nullptr static_assert thread_local true typeof typeof_unqual" : nullptr;
  }
};

class ConfLexer final : public QsciLexer {
public:
  using QsciLexer::QsciLexer;
  const char *language() const override { return "Conf"; }
  const char *lexer() const override { return "conf"; }
  const char *keywords(int set) const override {
    return set == 1 ? "server location listen server_name root index http events include upstream proxy_pass worker_processes worker_connections access_log error_log types return rewrite set user load_module directory virtualhost documentroot listen servername" : nullptr;
  }
  QString description(int n) const override {
    static const char *const names[] = {"Text","Comment","Number","Identifier","Extension","Parameter","String","Operator","IP address","Directive"};
    return n >= 0 && n < int(std::size(names)) ? QString::fromLatin1(names[n]) : QString();
  }
  QColor defaultColor(int n) const override {
    if (n == 1) return QColor("#66716c");
    if (n == 2 || n == 8) return QColor("#995018");
    if (n == 6) return QColor("#236640");
    if (n == 9 || n == 5) return QColor("#703fa0");
    if (n == 3 || n == 4) return QColor("#185d94");
    return QColor("#292d30");
  }
};
