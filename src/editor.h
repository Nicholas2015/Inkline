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
      {"Python", {"py", "pyw"}, createLexer<QsciLexerPython>},
      {"C / C++",
       {"cpp", "c", "h", "cc", "cxx", "hpp", "hh", "ino"},
       createLexer<QsciLexerCPP>},
      {"JavaScript",
       {"js", "jsx", "mjs", "cjs"},
       createLexer<QsciLexerJavaScript>},
      {"Java", {"java"}, createLexer<QsciLexerJava>},
      {"C#", {"cs"}, createLexer<QsciLexerCSharp>},
      {"Bash", {"sh", "bash", "zsh"}, createLexer<QsciLexerBash>},
      {"JSON", {"json", "jsonc"}, createLexer<QsciLexerJSON>},
      {"YAML", {"yaml", "yml"}, createLexer<QsciLexerYAML>},
      {"INI / Properties",
       {"ini", "cfg", "conf", "properties", "env"},
       createLexer<QsciLexerProperties>},
      {"HTML", {"html", "htm"}, createLexer<QsciLexerHTML>},
      {"XML", {"xml", "plist", "svg"}, createLexer<QsciLexerXML>},
      {"CSS", {"css"}, createLexer<QsciLexerCSS>},
      {"SQL", {"sql"}, createLexer<QsciLexerSQL>},
      {"Markdown", {"md", "markdown"}, createLexer<QsciLexerMarkdown>}};
  return languages;
}
class Edit final : public QsciScintilla {
public:
  Document doc;
  bool formatDirty = false;
  QString language;
  bool dirty() const { return formatDirty || isModified(); }
  explicit Edit(Document document) : doc(std::move(document)) {
    setUtf8(true);
    setFrameShape(QFrame::NoFrame);
    setScrollWidth(1);
    setScrollWidthTracking(true);
    setExtraAscent(2);
    setExtraDescent(2);
    applyLanguage();
    setMarginLineNumbers(0, true);
    setMarginWidth(0, "00000");
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
    const auto ext =
        language.isEmpty() ? QFileInfo(doc.path).suffix().toLower() : language;
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
          if (c.hslSaturation() > 25)
            style->setColor(QColor::fromHsl(c.hslHue(),
                                            qMin(76, c.hslSaturation()),
                                            qBound(77, c.lightness(), 98)),
                            i);
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
    setMarginsFont(font);
    setMarginsBackgroundColor(QColor("#f5f6f5"));
    setMarginsForegroundColor(QColor("#687174"));
    setCaretLineBackgroundColor(QColor("#f4f6f6"));
    setCaretLineVisible(true);
    setSelectionBackgroundColor(QColor("#d6e2e7"));
    setSelectionForegroundColor(QColor("#1f2c33"));
    setAutoCompletionSource(style ? AcsAll : AcsDocument);
    setAutoCompletionThreshold(2);
  }
};
