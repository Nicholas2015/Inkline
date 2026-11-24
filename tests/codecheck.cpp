#include "../src/codetools.h"
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

static int server() {
#ifdef _WIN32
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif
  QByteArray record;
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
      record = msg["params"]
                   .toObject()["textDocument"]
                   .toObject()["text"]
                   .toString()
                   .toUtf8();
      continue;
    } else if (method == "textDocument/didChange") {
      auto change = msg["params"]
                        .toObject()["contentChanges"]
                        .toArray()
                        .first()
                        .toObject();
      if (change["range"].toObject()["end"].toObject()["character"].toInt() !=
          3)
        return 3;
      record = change["text"].toString().toUtf8();
      continue;
    } else if (method == "textDocument/definition") {
      auto params = msg["params"].toObject();
      auto text = QString::fromUtf8(record);
      if (params["position"].toObject()["character"].toInt() != text.size())
        return 4;
      QJsonObject start{{"line", 0}, {"character", text == "#😀" ? 1 : 3}};
      result = QJsonObject{{"uri", params["textDocument"].toObject()["uri"]},
                           {"range", QJsonObject{{"start", start}}}};
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
    return server();
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
  installCodeTools(&w, tabs, [](QString) {});
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

  QTcpServer http;
  check.expect(http.listen(QHostAddress::LocalHost), "mock AI service startup");
  QByteArray request;
  bool received = false;
  bool delayResponse = false;
  QPointer<QTcpSocket> delayed;
  const QByteArray body =
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
      send->click();
      check.expect(until([&] { return received; }), "AI received request");
      check.expect(request.contains("Bearer test-session-key") &&
                       request.contains("mock"),
                   "AI token and model sent");
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
  auto toggle = action(w, "为当前标签启用行内续写");
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
  toggle->trigger();
  if (check.ok)
    qInfo() << "PASS native index, mock stdio LSP and HTTP AI integration";
  return check.ok ? 0 : 1;
}
