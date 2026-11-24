#include "columnheader.h"
#include "loglexer.h"
#include "languagelexers.h"
#include <Qsci/qscilexerruby.h>
#include <Qsci/qscilexerlua.h>
#include <Qsci/qscilexercmake.h>
#include <Qsci/qscilexermakefile.h>
#include <Qsci/qscilexerdiff.h>
#include <Qsci/qscilexerbatch.h>
#pragma once
#include "appearance.h"
#include "document.h"
#include "logfile.h"
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFutureWatcher>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenuBar>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStatusBar>
#include <QTabWidget>
#include <QVBoxLayout>
#include <Qsci/qscilexercpp.h>
#include <Qsci/qscilexerjson.h>
#include <Qsci/qscilexerproperties.h>
#include <Qsci/qscilexerpython.h>
#include <Qsci/qsciscintilla.h>
#include <QtConcurrentRun>

static void report(QWidget *owner, const std::exception &e) {
  QMessageBox::warning(owner, "墨行 Inkline", QString::fromUtf8(e.what()));
}
#include <Qsci/qsciapis.h>
#include <Qsci/qscilexerbash.h>
#include <Qsci/qscilexercsharp.h>
#include <Qsci/qscilexercss.h>
#include <Qsci/qscilexerhtml.h>
#include <Qsci/qscilexerjava.h>
#include <Qsci/qscilexerjavascript.h>
#include <Qsci/qscilexermarkdown.h>
#include <Qsci/qscilexersql.h>
#include <Qsci/qscilexerxml.h>
#include <Qsci/qscilexeryaml.h>
struct EditorLanguage {
  QString title;
  QStringList extensions;
  QsciLexer *(*create)(QObject *);
};
template <class Lexer> QsciLexer *createLexer(QObject *parent) {
  return new Lexer(parent);
}
inline const QList<EditorLanguage> &editorLanguages() {
  static const QList<EditorLanguage> languages = {
      {"日志", {"log", "trace"}, createLexer<LogLexer>},
      {"Python", {"py", "pyw", "pyi"}, createLexer<QsciLexerPython>},
      {"C", {"c"}, createLexer<CLexer>},
      {"C++",
       {"cpp", "h", "hxx", "ipp", "tpp", "cc", "cxx", "hpp", "hh", "ino"},
       createLexer<QsciLexerCPP>},
      {"Go", {"go"}, [](QObject *p) -> QsciLexer * { return new BracedLexer(p, true); }},
      {"Zig", {"zig", "zon"}, [](QObject *p) -> QsciLexer * { return new BracedLexer(p, false); }},
      {"Rust", {"rs"}, createLexer<RustLexer>},
      {"TOML", {"toml"}, createLexer<TomlLexer>},
      {"Conf", {"conf"}, createLexer<ConfLexer>},
      {"Ruby", {"rb"}, createLexer<QsciLexerRuby>},
      {"Lua", {"lua"}, createLexer<QsciLexerLua>},
      {"CMake", {"cmake"}, createLexer<QsciLexerCMake>},
      {"Makefile", {"mk", "mak"}, createLexer<QsciLexerMakefile>},
      {"Diff", {"diff", "patch"}, createLexer<QsciLexerDiff>},
      {"Batch", {"bat", "cmd"}, createLexer<QsciLexerBatch>},
      {"JavaScript",
       {"js", "jsx", "mjs", "cjs"},
       createLexer<QsciLexerJavaScript>},
      {"Java", {"java"}, createLexer<QsciLexerJava>},
      {"C#", {"cs"}, createLexer<QsciLexerCSharp>},
      {"Bash", {"sh", "bash", "zsh"}, createLexer<QsciLexerBash>},
      {"JSON", {"json", "jsonc", "jsonl", "ndjson"}, createLexer<QsciLexerJSON>},
      {"YAML", {"yaml", "yml"}, createLexer<QsciLexerYAML>},
      {"INI / Properties",
       {"ini", "cfg", "properties", "env"},
       createLexer<QsciLexerProperties>},
      {"HTML", {"html", "htm"}, createLexer<QsciLexerHTML>},
      {"XML", {"xml", "plist", "svg"}, createLexer<QsciLexerXML>},
      {"CSS", {"css"}, createLexer<QsciLexerCSS>},
      {"SQL", {"sql"}, createLexer<QsciLexerSQL>},
      {"Markdown", {"md", "markdown"}, createLexer<QsciLexerMarkdown>}};
  return languages;
}
class Edit final : public QsciScintilla {
  ColumnHeader header{this};
  QFrame divider{this};
  void layoutColumns() {
    const auto digits = QString::number(lines()).size();
    int numberWidth =
        qMax(48, fontMetrics().horizontalAdvance('9') * int(digits) + 16);
    if (marginWidth(0) != numberWidth)
      setMarginWidth(0, numberWidth);
    int left = numberWidth + marginWidth(1);
    auto h = header.sizeHint().height();
    if (viewportMargins().top() != h)
      setViewportMargins(0, h, 0, 0);
    header.columns(left, dynamic_cast<LogLexer *>(lexer()) ? "日志内容"
                         : lexer()                         ? "代码"
                                                           : "文本");
    divider.setGeometry(left - 1, viewport()->y(), 1, viewport()->height());
    divider.raise();
  }

protected:
  void resizeEvent(QResizeEvent *event) override {
    QsciScintilla::resizeEvent(event);
    layoutColumns();
  }

public:
  Document doc;
  bool formatDirty = false;
  QString language;
  bool dirty() const { return formatDirty || isModified(); }
  explicit Edit(Document document) : doc(std::move(document)) {
    setUtf8(true);
    setFrameShape(QFrame::NoFrame);
    SendScintilla(SCI_SETMARGINLEFT, 0UL, 6L);
    setScrollWidth(1);
    setScrollWidthTracking(true);
    setExtraAscent(2);
    setExtraDescent(2);
    applyLanguage();
    setMarginLineNumbers(0, true);
    divider.setStyleSheet("background:#e3e1da;");
    divider.setAttribute(Qt::WA_TransparentForMouseEvents);
    connect(this, &QsciScintilla::linesChanged, this,
            [this] { layoutColumns(); });
    setAutoIndent(true);
    setIndentationsUseTabs(false);
    setTabWidth(4);
    setBraceMatching(SloppyBraceMatch);
    auto crlf = doc.text.count("\r\n");
    auto lf = doc.text.count('\n') - crlf, cr = doc.text.count('\r') - crlf;
    setEolMode(crlf && crlf >= qMax(cr, lf) ? EolWindows
               : cr > lf                    ? EolMac
                                            : EolUnix);
    setText(doc.text);
    setModified(false);
  }
  void applyLanguage() {
    setProperty("filePath", doc.path);
    auto ext =
        language.isEmpty() ? QFileInfo(doc.path).suffix().toLower() : language;
    if (language.isEmpty() &&
        QFileInfo(doc.path).fileName().contains(
            QRegularExpression(R"(\.(?:log|trace)(?:\.\d+)+$)",
                               QRegularExpression::CaseInsensitiveOption)))
      ext = "log";
    if (language.isEmpty()) {
      const auto filename = QFileInfo(doc.path).fileName().toLower();
      if (filename == "cmakelists.txt") ext = "cmake";
      else if (filename == "makefile" || filename == "gnumakefile") ext = "mk";
      else if (filename == ".env" || filename.startsWith(".env.")) ext = "env";
      else if (ext == "conf") {
        static const QRegularExpression setting(QStringLiteral(R"(^\s*(?:\[[^\]\r\n]+\]|[\w.-]+\s*=))"),
                                               QRegularExpression::MultilineOption);
        if (setting.match(doc.text.left(8192)).hasMatch()) ext = "ini";
      }
    }
    QsciLexer *style = nullptr;
    for (const auto &item : editorLanguages())
      if (item.extensions.contains(ext)) {
        style = item.create(this);
        break;
      }
    auto font = Appearance::codeFont();
    auto *old = lexer();
    if (style) {
      style->setDefaultFont(font);
      style->setDefaultPaper(Qt::white);
      for (int i = 0; i < 128; ++i)
        if (!style->description(i).isEmpty()) {
          auto c = style->defaultColor(i);
          if (!dynamic_cast<LogLexer *>(style) && c.hslSaturation() > 25)
            style->setColor(QColor::fromHsl(c.hslHue(),
                                            qMin(180, c.hslSaturation()),
                                            qBound(77, c.lightness(), 98)),
                            i);
        }
      // Keep structural keys distinct from values on the white editor surface.
      if (auto *json = dynamic_cast<QsciLexerJSON *>(style)) {
        json->setHighlightComments(ext == "jsonc");
        json->setColor(QColor("#185d94"), QsciLexerJSON::Property);
        json->setColor(QColor("#236640"), QsciLexerJSON::String);
        json->setColor(QColor("#9a5018"), QsciLexerJSON::Number);
        json->setColor(QColor("#703fa0"), QsciLexerJSON::Keyword);
        json->setColor(QColor("#66716c"), QsciLexerJSON::CommentLine);
        json->setColor(QColor("#66716c"), QsciLexerJSON::CommentBlock);
      } else if (auto *yaml = dynamic_cast<QsciLexerYAML *>(style)) {
        yaml->setColor(QColor("#236640"), QsciLexerYAML::Default);
        yaml->setColor(QColor("#185d94"), QsciLexerYAML::Identifier);
        yaml->setColor(QColor("#703fa0"), QsciLexerYAML::Keyword);
        yaml->setColor(QColor("#9a5018"), QsciLexerYAML::Number);
        yaml->setColor(QColor("#236640"), QsciLexerYAML::Reference);
        yaml->setColor(QColor("#66716c"), QsciLexerYAML::Comment);
      }
      if (auto *props = dynamic_cast<QsciLexerProperties *>(style)) {
        props->setColor(QColor("#185d94"), QsciLexerProperties::Key);
        props->setColor(QColor("#236640"), QsciLexerProperties::Default);
        props->setColor(QColor("#703fa0"), QsciLexerProperties::Section);
      }
      auto *api = new QsciAPIs(style);
      QSet<QString> words;
      for (int group : {1, 2}) {
        const char *keys = style->keywords(group);
        if (keys)
          for (const auto &word :
               QString::fromLatin1(keys).split(' ', Qt::SkipEmptyParts))
            words.insert(word);
      }
      for (auto &word : words)
        api->add(word);
      api->prepare();
    }
    setLexer(style);
    if (old)
      old->deleteLater();
    if (style)
      style->setFont(font);
    else
      setFont(font);
    if (auto *log = dynamic_cast<LogLexer *>(style)) {
      for (int token = LogStyle::Text; token <= LogStyle::Function; ++token) {
        const auto paper = LogStyle::levelBackground(token);
        if (paper.isValid()) log->setPaper(paper, token);
        auto face = font;
        face.setWeight(LogStyle::weight(token));
        log->setFont(face, token);
      }
    }
    setMarginsFont(font);
    layoutColumns();
    setMarginsBackgroundColor(QColor("#f6f5f2"));
    setMarginsForegroundColor(QColor("#52524d"));
    const bool logView = dynamic_cast<LogLexer *>(style) != nullptr;
    setCaretLineBackgroundColor(QColor(logView ? "#77888e" : "#faf9f6"));
    SendScintilla(SCI_SETCARETLINEBACKALPHA, logView ? 16 : SC_ALPHA_NOALPHA);
    setCaretLineVisible(true);
    setSelectionBackgroundColor(QColor("#d6e2e7"));
    setSelectionForegroundColor(QColor("#1f2c33"));
    setAutoCompletionSource(style ? AcsAll : AcsDocument);
    setAutoCompletionThreshold(2);
  }
};
