#include "../src/codetools.h"
#include "../src/editor.h"
#include "../src/pythonindex.h"
#include <Qsci/qscilexerpython.h>
#include <Qsci/qsciscintilla.h>
#include <QtNetwork>
#include <QtWidgets>
#include <cstdio>
#include <iostream>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

static int server(QByteArray expectedFamily) {
#ifdef _WIN32
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  QHash<QString, QString> buffers;
  for (;;) {
    std::string line;
    if (!std::getline(std::cin, line))
      return 0;
    if (line.rfind("Content-Length:", 0) != 0)
      return 1;
    int len = std::stoi(line.substr(15));
    if (!std::getline(std::cin, line))
      return 2;
    QByteArray data(len, '\0');
    std::cin.read(data.data(), len);
    auto msg = QJsonDocument::fromJson(data).object();
    auto method = msg["method"].toString();
    QJsonValue result;
    if (method == "initialize")
      result =
          QJsonObject{{"capabilities", QJsonObject{{"textDocumentSync", 2}}}};
    else if (method == "textDocument/didOpen") {
      const auto doc = msg["params"].toObject()["textDocument"].toObject();
      auto id = doc["languageId"].toString().toLatin1();
      if (id != expectedFamily && !(id == "c" && expectedFamily == "cpp")) return 5;
      const auto uri = doc["uri"].toString();
      if ((id == "c" || id == "cpp") && !uri.endsWith("." + QString::fromLatin1(id))) return 6;
      buffers[uri] = doc["text"].toString();
      continue;
    } else if (method == "textDocument/didClose") {
      buffers.remove(msg["params"].toObject()["textDocument"].toObject()["uri"].toString());
      continue;
    } else if (method == "textDocument/didChange") {
      auto change = msg["params"]
                        .toObject()["contentChanges"]
                        .toArray()
                        .first()
                        .toObject();
      const auto uri = msg["params"].toObject()["textDocument"].toObject()["uri"].toString();
      if (!buffers.contains(uri) || change["range"].toObject()["end"].toObject()["character"].toInt() !=
          buffers[uri].section('\n', -1).size())
        return 3;
      buffers[uri] = change["text"].toString();
      continue;
    } else if (method == "textDocument/definition" || method == "textDocument/references") {
      auto params = msg["params"].toObject();
      auto uri = params["textDocument"].toObject()["uri"].toString();
      if (!buffers.contains(uri)) return 7;
      auto text = buffers.value(uri);
      if (params["position"].toObject()["character"].toInt() != text.size())
        return 4;
      QJsonObject start{{"line", 0}, {"character", text == "#😀" ? 1 : 3}};
      result = QJsonObject{{"uri", params["textDocument"].toObject()["uri"]},
                           {"range", QJsonObject{{"start", start}}}};
      if (method == "textDocument/references") {
        if (!params["context"].toObject()["includeDeclaration"].toBool()) return 8;
        for (auto it = buffers.cbegin(); it != buffers.cend(); ++it) {
          if (it.key() == uri) continue;
          result = QJsonArray{QJsonObject{{"uri", it.key()},
              {"range", QJsonObject{{"start", QJsonObject{{"line", 0}, {"character", 1}}}}}}};
          break;
        }
      }
    } else
      continue;
    auto body = QJsonDocument(QJsonObject{{"jsonrpc", "2.0"},
                                          {"id", msg["id"]},
                                          {"result", result}})
                    .toJson(QJsonDocument::Compact);
    std::cout << "Content-Length: " << body.size() << "\r\n\r\n";
    std::cout.write(body.data(), body.size());
    std::cout.flush();
  }
}
struct Check {
  bool ok = true;
  void expect(bool pass, QString message) {
    if (!pass) {
      qCritical().noquote() << message;
      ok = false;
    }
  }
};
static bool until(std::function<bool()> check, int ms = 4500) {
  QElapsedTimer clock;
  clock.start();
  while (!check() && clock.elapsed() < ms) {
    QCoreApplication::processEvents();
    QThread::msleep(3);
  }
  return check();
}
static QAction *action(QWidget &w, QString text) {
  for (auto a : w.findChildren<QAction *>())
    if (a->text() == text)
      return a;
  return nullptr;
}
int main(int argc, char **argv) {
  if (argc > 1 && QByteArray(argv[1]) == "--lsp")
    return server(argc > 2 ? QByteArray(argv[2]) : QByteArray("python"));
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication app(argc, argv);
  QTemporaryDir tmp;
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, tmp.path());
  app.setOrganizationName("Codecheck");
  app.setApplicationName("Codecheck");
  Check check;
  QString helper = tmp.filePath("helper.py"), entry = tmp.filePath("entry.py");
  QString original = "from helper import target as alias\nalias(2)";
  QString helpers =
      "def target(x):\n    return x\n\ndef other(x):\n    return x";
  pyindex::Project index(tmp.path(), {{helper, helpers}, {entry, original}});
  auto alias = index.locations(entry, 1, 2, false);
  check.expect(
      alias.size() == 1 &&
          QUrl(alias.first().toObject()["uri"].toString()).toLocalFile() ==
              helper,
      "native import alias definition");
  auto refs = index.locations(helper, 1, 11, true);
  check.expect(refs.size() == 2,
               "native parameter references respect function scope");

  QMainWindow w;
  auto tabs = new QTabWidget;
  w.setCentralWidget(tabs);
  auto e = new QsciScintilla;
  e->setUtf8(true);
  e->setProperty("filePath", entry);
  e->setLexer(new QsciLexerPython(e));
  tabs->addTab(e, "entry.py");
  w.show();
  auto cmd = QJsonDocument(QJsonArray{app.applicationFilePath(), "--lsp"})
                 .toJson(QJsonDocument::Compact);
  qputenv("INKLINE_PYTHON_LSP", cmd);
  installCodeTools(&w, tabs, [tabs](const QString &path) {
    for (int i = 0; i < tabs->count(); ++i)
      if (QFileInfo(tabs->widget(i)->property("filePath").toString()) == QFileInfo(path))
        tabs->setCurrentIndex(i);
  });
  auto def = action(w, "转到定义");
  e->setText("#😀");
  e->SendScintilla(QsciScintilla::SCI_GOTOPOS, 5);
  def->trigger();
  check.expect(until([&] {
                 return e->SendScintilla(QsciScintilla::SCI_GETCURRENTPOS) == 1;
               }),
               "LSP definition / UTF-16 position");
  e->setText("#😀z");
  e->SendScintilla(QsciScintilla::SCI_GOTOPOS, 6);
  def->trigger();
  check.expect(until([&] {
                 return e->SendScintilla(QsciScintilla::SCI_GETCURRENTPOS) == 5;
               }),
               "LSP increment uses prior UTF-16 range and updates bytes");

  QList<Edit *> semanticTabs;
  for (const auto &[suffix, family] : QList<QPair<QString, QByteArray>>{
         {"go", "go"}, {"java", "java"}, {"rs", "rust"}, {"c", "cpp"}, {"cpp", "cpp"}}) {
    const auto key = "INKLINE_" + family.toUpper() + "_LSP";
    qputenv(key.constData(), QJsonDocument(QJsonArray{app.applicationFilePath(), "--lsp",
                         QString::fromLatin1(family)}).toJson(QJsonDocument::Compact));
    const auto samplePath = tmp.filePath("entry." + suffix);
    QFile source(samplePath);
    const QByteArray contents = QStringLiteral("#😀").toUtf8();
    check.expect(source.open(QIODevice::WriteOnly) && source.write(contents) == contents.size(),
                 "create semantic navigation fixture ." + suffix);
    source.close();
    Document doc;
    doc.path = samplePath;
    doc.text = QString::fromUtf8(contents);
    auto *sample = new Edit(doc);
    semanticTabs << sample;
    tabs->addTab(sample, "entry." + suffix);
    tabs->setCurrentWidget(sample);
    sample->SendScintilla(QsciScintilla::SCI_GOTOPOS, 5);
    def->trigger();
    check.expect(until([&] {
      return sample->SendScintilla(QsciScintilla::SCI_GETCURRENTPOS) == 1;
    }), "LSP definition and languageId for ." + suffix);
    sample->setText("#😀z");
    sample->SendScintilla(QsciScintilla::SCI_GOTOPOS, 6);
    def->trigger();
    check.expect(until([&] {
      return sample->SendScintilla(QsciScintilla::SCI_GETCURRENTPOS) == 5;
    }), "LSP incremental changes for ." + suffix);
  }
  // C and C++ have different document IDs and share the clangd workspace.
  auto *cpp = semanticTabs.last();
  auto *c = semanticTabs.at(semanticTabs.size() - 2);
  cpp->SendScintilla(QsciScintilla::SCI_GOTOPOS, 6);
  QTimer chooseReference;
  QObject::connect(&chooseReference, &QTimer::timeout, &w, [&] {
    if (auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget()))
      dialog->accept();
  });
  chooseReference.start(10);
  action(w, "查找引用…")->trigger();
  check.expect(until([&] {
    return tabs->currentWidget() == c && c->SendScintilla(QsciScintilla::SCI_GETCURRENTPOS) == 1;
  }), "LSP references navigates between CPP and C buffers");
  chooseReference.stop();
  tabs->setCurrentWidget(e);
  for (auto *sample : semanticTabs) delete sample;

  QTcpServer http;
  check.expect(http.listen(QHostAddress::LocalHost), "mock AI service startup");
  QByteArray request;
  bool received = false;
  bool delayResponse = false;
  QPointer<QTcpSocket> delayed;
  QByteArray body =
      R"({"choices":[{"message":{"content":"code_result"}}]})";
  auto respond = [&](QTcpSocket *s) {
    s->write("HTTP/1.1 200 OK\r\nContent-Type: "
             "application/json\r\nContent-Length: " +
             QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" +
             body);
    s->disconnectFromHost();
  };
  QObject::connect(&http, &QTcpServer::newConnection, &http, [&] {
    auto sock = http.nextPendingConnection();
    auto input = std::make_shared<QByteArray>();
    QObject::connect(sock, &QTcpSocket::readyRead, sock, [&, sock, input] {
      *input += sock->readAll();
      auto end = input->indexOf("\r\n\r\n");
      if (end < 0)
        return;
      QRegularExpression header("Content-Length: (\\d+)",
                                QRegularExpression::CaseInsensitiveOption);
      auto length = header.match(QString::fromLatin1(input->left(end)))
                        .captured(1)
                        .toInt();
      if (input->size() < end + 4 + length)
        return;
      request = *input;
      received = true;
      if (delayResponse)
        delayed = sock;
      else
        respond(sock);
    });
  });
  QString url =
      QString("http://127.0.0.1:%1/v1/chat/completions").arg(http.serverPort());
  QSettings().setValue("ai/endpoint", url);
  QSettings().setValue("ai/model", "mock");
  auto ai = action(w, "AI 代码建议…");
  e->setText("old");
  e->selectAll();
  for (bool stale : {false, true}) {
    received = false;
    QTimer::singleShot(20, &w, [&] {
      auto d = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      if (!d) {
        check.expect(false, "AI dialog opened");
        return;
      }
      auto buttons = d->findChildren<QPushButton *>();
      QPushButton *send = nullptr, *apply = nullptr;
      for (auto b : buttons) {
        if (b->text() == "发送并生成")
          send = b;
        if (b->text() == "应用建议")
          apply = b;
      }
      auto fields = d->findChildren<QLineEdit *>();
      QLineEdit *key = nullptr, *address = nullptr;
      for (auto field : fields) {
        if (field->echoMode() == QLineEdit::Password)
          key = field;
        if (field->text() == url)
          address = field;
      }
      if (!key || !address || !send || !apply) {
        check.expect(false, "AI dialog controls");
        d->reject();
        return;
      }
      key->setText("test-session-key");
      if (!stale) {
        auto *models = d->findChild<QComboBox *>("providerModels");
        auto it = std::find_if(buttons.begin(), buttons.end(), [](auto *button) {
          return button->text() == "获取模型";
        });
        if (!models || it == buttons.end()) {
          check.expect(false, "model discovery controls");
          d->reject();
          return;
        }
        auto *fetch = *it;
        const auto savedBody = body;
        body = R"({"data":[{"id":"alt"},{"id":"mock"},{"id":"alt"}]})";
        fetch->click();
        check.expect(until([&] { return fetch->isEnabled(); }) && models->count() == 2 &&
                     models->currentText() == "mock" && request.startsWith("GET /v1/models ") &&
                     request.contains("Bearer test-session-key"), "authenticated provider discovery");
        models->setCurrentText("alt");
        body = savedBody;
        received = false;
      }
      send->click();
      check.expect(until([&] { return received; }), "AI received request");
      check.expect(request.contains("Bearer test-session-key") &&
                       request.contains("\"alt\""),
                   "AI token and selected model sent");
      if (stale) {
        e->append("!");
        if (delayed)
          respond(delayed);
      }
      check.expect(until([&] { return send->isEnabled(); }),
                   "AI response completed");
      if (stale) {
        check.expect(!apply->isEnabled(),
                     "stale AI response cannot overwrite changed document");
        d->reject();
      } else {
        check.expect(apply->isEnabled(), "AI review can be applied");
        address->setText(url + "/other");
        check.expect(key->text().isEmpty(), "key stays scoped to endpoint");
        address->setText(url);
        apply->click();
        if (d->isVisible())
          d->reject();
      }
    });
    delayResponse = stale;
    ai->trigger();
    if (!stale)
      check.expect(e->text() == "code_result", "AI replaced selection");
  }
  QSettings().sync();
  QFile settings(QSettings().fileName());
  check.expect(settings.open(QIODevice::ReadOnly), "settings accessible");
  check.expect(!settings.readAll().contains("test-session-key"),
               "key not persisted");
  auto toggle = action(w, "输入时显示 AI 灰字补全");
  delayResponse = false;
  received = false;
  e->setText("prefix");
  e->SendScintilla(QsciScintilla::SCI_GOTOPOS, 6);
  QTimer::singleShot(20, &w, [] {
    auto box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
    if (box)
      box->button(QMessageBox::Yes)->click();
  });
  toggle->trigger();
  w.activateWindow();
  e->setFocus();
  check.expect(until([&] {
                 for (auto label : e->findChildren<QLabel *>())
                   if (label->isVisible() && label->text() == "code_result")
                     return true;
                 return false;
               }),
               "opted-in inline preview appeared");
  check.expect(e->text() == "prefix", "inline preview leaves document intact");
  QKeyEvent tabKey(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
  QApplication::sendEvent(e, &tabKey);
  check.expect(e->text() == "prefixcode_result",
               "Tab accepts inline suggestion");
  // Inline annotations occupy virtual lines, so a mid-line preview leaves the
  // original suffix and the following line fully visible and selectable.
  const auto preview = [&] {
    const auto length = e->SendScintilla(QsciScintilla::SCI_ANNOTATIONGETTEXT, 0UL);
    QByteArray text(length + 1, '\0');
    e->SendScintilla(QsciScintilla::SCI_ANNOTATIONGETTEXT, 0UL, static_cast<void *>(text.data()));
    return QString::fromUtf8(text.constData(), length);
  };
  const QString left(5000, 'p');
  e->setText(left + ");\nexisting");
  e->setCursorPosition(0, left.size());
  body = QJsonDocument(QJsonObject{{"choices", QJsonArray{
      QJsonObject{{"message", QJsonObject{{"content", "hello world\n  next());"}}}}}}}).toJson();
  check.expect(until([&] { return preview() == "hello world\n  next()" && e->SendScintilla(QsciScintilla::SCI_ANNOTATIONGETLINES, 0UL) == 2; }),
               "multiline preview in virtual rows");
  const auto payload = QJsonDocument::fromJson(request.mid(request.indexOf("\r\n\r\n") + 4)).object();
  const auto message = payload.value("messages").toArray().last().toObject().value("content").toString();
  const auto sent = QJsonDocument::fromJson(message.section("\n\n", -1).toUtf8()).object();
  const auto prefix = sent.value("prefix").toString(), suffix = sent.value("suffix").toString();
  check.expect(!prefix.isEmpty() && suffix == ");\nexisting" && prefix.size() + suffix.size() == 4096,
               "both sides share a single 4096-character budget");
  check.expect(e->text() == left + ");\nexisting", "preview leaves existing suffix and following line intact");
  QKeyEvent wordKey(QEvent::KeyPress, Qt::Key_Right, Qt::AltModifier);
  QApplication::sendEvent(e, &wordKey);
  check.expect(e->text() == left + "hello);\nexisting" && preview().contains(" world"),
               "Alt+Right accepts one word and retains remaining multiline preview");
  QApplication::sendEvent(e, &tabKey);
  check.expect(e->text().replace("\r\n", "\n") == left + "hello world\n  next());\nexisting" &&
               preview().isEmpty(), "Tab inserts remaining lines without duplicating suffix");
  body = R"({"choices":[{"message":{"content":"code_result"}}]})";
  // Previously enabled endpoints only have permission to receive prefixes.
  QSettings().remove("ai/inlineContextEndpoint");
  e->setText("onePRIVATE_SUFFIX");
  e->setCursorPosition(0, 3);
  check.expect(until([&] { return preview().contains("code_result"); }),
               "legacy opt-in still receives a middle-of-line suggestion");
  check.expect(!request.contains("PRIVATE_SUFFIX"), "legacy consent keeps right context private");
  e->setText("changed");
  check.expect(preview().isEmpty(), "editing cancels virtual completion rows");
  QSettings().setValue("ai/inlineContextEndpoint", url);
  auto *second = new QsciScintilla;
  second->setLexer(new QsciLexerPython(second));
  tabs->addTab(second, "another.py");
  tabs->setCurrentWidget(second);
  second->setFocus();
  second->setText("other");
  second->SendScintilla(QsciScintilla::SCI_GOTOPOS, 5);
  check.expect(until([&] {
    for (auto *label : second->findChildren<QLabel *>())
      if (label->isVisible() && label->text() == "code_result") return true;
    return false;
  }), "new code tab inherits inline completion");
  QKeyEvent escapeKey(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
  QApplication::sendEvent(second, &escapeKey);
  check.expect(second->text() == "other", "Escape leaves code untouched");
  // Use the same extension detection and editor widget as newly opened files.
  for (const auto &language : editorLanguages()) {
    if (language.extensions.contains("log")) continue;
    Document doc;
    doc.path = "completion." + language.extensions.first();
    auto *sample = new Edit(doc);
    tabs->addTab(sample, doc.path);
    tabs->setCurrentWidget(sample);
    sample->setFocus();
    sample->setText("prefix");
    sample->SendScintilla(QsciScintilla::SCI_GOTOPOS, 6);
    const bool shown = until([&] {
      for (auto *label : sample->findChildren<QLabel *>())
        if (label->isVisible() && label->text() == "code_result") return true;
      return false;
    });
    const bool correctLanguage = sample->lexer() &&
        request.contains(QString("当前语言：%1。").arg(QString::fromUtf8(sample->lexer()->language())).toUtf8());
    QKeyEvent accept(QEvent::KeyPress, Qt::Key_Tab, Qt::NoModifier);
    const bool untouched = sample->text() == "prefix";
    QApplication::sendEvent(sample, &accept);
    check.expect(shown && correctLanguage && untouched && sample->text() == "prefixcode_result",
                 qPrintable(language.title + " inline preview and Tab"));
    if (!shown) break;
  }
  toggle->trigger();
  if (check.ok)
    qInfo() << "PASS native index, mock stdio LSP and HTTP AI integration";
  return check.ok ? 0 : 1;
}
