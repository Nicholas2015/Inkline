#include "codetools.h"
#include "pythonindex.h"
#include <Qsci/qscilexerpython.h>
#include <Qsci/qsciscintilla.h>
#include <QtConcurrentRun>
#include <QtWidgets>

namespace {
using Editor = QsciScintilla;
using Obj = QJsonObject;
using Arr = QJsonArray;
using Reply = std::function<void(QJsonValue)>;
QString pathOf(Editor *e) {
  auto path = e->property("filePath").toString();
  if (path.isEmpty())
    path = QString(".inkline-untitled-%1.py").arg(quintptr(e), 0, 16);
  return QFileInfo(path).absoluteFilePath();
}
QString uriOf(Editor *e) {
  return QUrl::fromLocalFile(pathOf(e)).toString(QUrl::FullyEncoded);
}
Editor *pythonTab(QWidget *widget) {
  auto e = qobject_cast<Editor *>(widget);
  return e && qobject_cast<QsciLexerPython *>(e->lexer()) ? e : nullptr;
}
long bytePos(Editor *e, Obj pos) {
  int row = qBound(0, pos["line"].toInt(), e->lines() - 1);
  return e->SendScintilla(Editor::SCI_POSITIONFROMLINE, row) +
         e->text(row).left(qMax(0, pos["character"].toInt())).toUtf8().size();
}
Obj cursor(Editor *e) {
  long pos = e->SendScintilla(Editor::SCI_GETCURRENTPOS);
  long row = e->SendScintilla(Editor::SCI_LINEFROMPOSITION, pos);
  long start = e->SendScintilla(Editor::SCI_POSITIONFROMLINE, row);
  int column =
      QString::fromUtf8(e->text(row).toUtf8().left(pos - start)).size();
  return {{"line", int(row)}, {"character", column}};
}
class Language : public QObject {
  QMainWindow *win;
  QTabWidget *tabs;
  std::function<void(QString)> open;
  QProcess process;
  QTimer debounce;
  QByteArray input;
  QHash<int, Reply> requests;
  QHash<QString, int> versions;
  QHash<QString, Obj> oldEnds;
  bool incremental = false;
  QHash<QString, Arr> diagnostics;
  QSet<QString> dirty;
  QSet<Editor *> observed;
  int seq = 0;
  quint64 queryGeneration = 0;
  bool ready = false, starting = false, pull = false;
  QList<std::function<void()>> initialized;
  void notice(QString msg) { win->statusBar()->showMessage(msg, 7000); }
  void send(Obj msg) {
    msg["jsonrpc"] = "2.0";
    auto body = QJsonDocument(msg).toJson(QJsonDocument::Compact);
    process.write("Content-Length: " + QByteArray::number(body.size()) +
                  "\r\n\r\n" + body);
  }
  void notify(QString method, Obj params) {
    send({{"method", method}, {"params", params}});
  }
  void request(QString method, Obj params, Reply callback) {
    int id = ++seq;
    requests[id] = std::move(callback);
    send({{"id", id}, {"method", method}, {"params", params}});
    QTimer::singleShot(15000, this, [this, id] {
      if (requests.contains(id))
        requests.take(id)(QJsonValue(QJsonValue::Undefined));
    });
  }
  void stopped() {
    ready = false;
    starting = false;
    input.clear();
    versions.clear();
    oldEnds.clear();
    diagnostics.clear();
    auto handlers = std::exchange(requests, {});
    for (auto &handler : handlers)
      handler(QJsonValue(QJsonValue::Undefined));
    auto pending = std::exchange(initialized, {});
    for (auto &fn : pending)
      fn();
  }
  void receive() {
    input += process.readAllStandardOutput();
    while (true) {
      int end = input.indexOf("\r\n\r\n");
      if (end < 0) {
        if (input.size() > 8192)
          process.kill();
        return;
      }
      auto header = QString::fromLatin1(input.left(end));
      static QRegularExpression length(
          R"((?:^|\r\n)Content-Length:\s*([0-9]+)\s*(?:$|\r\n))",
          QRegularExpression::CaseInsensitiveOption);
      auto match = length.match(header);
      bool ok = false;
      qlonglong size = match.captured(1).toLongLong(&ok);
      if (!ok || size > 32 * 1024 * 1024) {
        process.kill();
        return;
      }
      if (input.size() < end + 4 + size)
        return;
      auto msg = QJsonDocument::fromJson(input.mid(end + 4, size)).object();
      input.remove(0, end + 4 + size);
      auto method = msg["method"].toString();
      auto id = msg["id"];
      if (method.isEmpty() && requests.contains(id.toInt(-1))) {
        auto handler = requests.take(id.toInt());
        handler(msg.contains("error") ? QJsonValue(QJsonValue::Undefined)
                                      : msg.value("result"));
      } else if (method == "textDocument/publishDiagnostics") {
        auto params = msg["params"].toObject();
        auto uri = params["uri"].toString();
        if (!params.contains("version") || params["version"].isNull() ||
            params["version"].toInt() == versions.value(uri))
          diagnostics[uri] = params["diagnostics"].toArray();
      } else if (msg.contains("id") && !method.isEmpty()) {
        if (method == "workspace/configuration") {
          Arr values;
          for (auto item : msg["params"].toObject()["items"].toArray()) {
            Q_UNUSED(item);
            values.append(Obj{});
          }
          send({{"id", id}, {"result", values}});
        } else
          send({{"id", id}, {"result", QJsonValue::Null}});
      }
    }
  }
  void synchronize() {
    if (!ready)
      return;
    QSet<QString> active;
    for (int i = 0; i < tabs->count(); ++i)
      if (auto e = pythonTab(tabs->widget(i))) {
        auto uri = uriOf(e);
        active.insert(uri);
        if (!observed.contains(e)) {
          observed.insert(e);
          connect(e, &Editor::textChanged, this, [this, e] {
            dirty.insert(uriOf(e));
            debounce.start();
          });
          connect(e, &QObject::destroyed, this, [this, e] {
            observed.remove(e);
            debounce.start();
          });
        }
        if (!versions.contains(uri)) {
          versions[uri] = 1;
          notify("textDocument/didOpen",
                 {{"textDocument", Obj{{"uri", uri},
                                       {"languageId", "python"},
                                       {"version", 1},
                                       {"text", e->text()}}}});
        } else if (dirty.contains(uri)) {
          int version = ++versions[uri];
          Obj change{{"text", e->text()}};
          if (incremental)
            change["range"] = Obj{{"start", Obj{{"line", 0}, {"character", 0}}},
                                  {"end", oldEnds.value(uri)}};
          notify("textDocument/didChange",
                 {{"textDocument", Obj{{"uri", uri}, {"version", version}}},
                  {"contentChanges", Arr{change}}});
        }
        oldEnds[uri] = Obj{{"line", e->lines() - 1},
                           {"character", int(e->text(e->lines() - 1).size())}};
      }
    dirty.clear();
    for (auto uri : versions.keys())
      if (!active.contains(uri)) {
        versions.remove(uri);
        oldEnds.remove(uri);
        diagnostics.remove(uri);
        notify("textDocument/didClose", {{"textDocument", Obj{{"uri", uri}}}});
      }
  }
  QStringList command() const {
    auto override = qgetenv("INKLINE_PYTHON_LSP");
    if (override.isEmpty()) override = qgetenv("INKEDIT_PYTHON_LSP");
    QStringList args;
    if (!override.isEmpty()) {
      for (auto part : QJsonDocument::fromJson(override).array()) {
        if (!part.isString())
          return {};
        args << part.toString();
      }
      return args;
    }
    for (QString name :
         {"pylsp", "pyright-langserver", "basedpyright-langserver"}) {
      auto exe = QStandardPaths::findExecutable(name);
      if (!exe.isEmpty()) {
        args << exe;
        if (name != "pylsp")
          args << "--stdio";
        return args;
      }
    }
    return {};
  }
  bool start(Editor *e) {
    auto args = command();
    if (args.isEmpty())
      return false;
    QString program = args.takeFirst();
    starting = true;
    process.start(program, args);
    QString root = win->property("projectRoot").toString();
    if (root.isEmpty())
      root = QFileInfo(pathOf(e)).absolutePath();
    auto uri = QUrl::fromLocalFile(root).toString(QUrl::FullyEncoded);
    request(
        "initialize",
        {{"processId", int(QCoreApplication::applicationPid())},
         {"rootUri", uri},
         {"workspaceFolders",
          Arr{Obj{{"uri", uri}, {"name", QFileInfo(root).fileName()}}}},
         {"capabilities",
          Obj{{"general", Obj{{"positionEncodings", Arr{"utf-16"}}}},
              {"textDocument",
               Obj{{"synchronization", Obj{{"dynamicRegistration", false}}},
                   {"diagnostic", Obj{}},
                   {"completion", Obj{{"completionItem",
                                       Obj{{"snippetSupport", false}}}}}}}}}},
        [this](QJsonValue result) {
          starting = false;
          if (result.isObject()) {
            auto caps = result.toObject()["capabilities"].toObject();
            auto sync = caps["textDocumentSync"];
            incremental = (sync.isObject() ? sync.toObject()["change"].toInt()
                                           : sync.toInt()) == 2;
            ready = true;
            pull = caps.contains("diagnosticProvider");
            notify("initialized", {});
            synchronize();
          } else if (process.state() != QProcess::NotRunning)
            process.kill();
          auto pending = std::exchange(initialized, {});
          for (auto &fn : pending)
            fn();
        });
    return true;
  }
  void present(Editor *e, QString kind, QJsonValue result) {
    if (kind == "signatureHelp") {
      auto data = result.toObject();
      auto signatures = data["signatures"].toArray();
      if (signatures.isEmpty()) {
        notice("当前位置没有函数签名");
        return;
      }
      auto label = signatures
                       .at(qBound(0, data["activeSignature"].toInt(),
                                  int(signatures.size() - 1)))
                       .toObject()["label"]
                       .toString();
      e->SendScintilla(Editor::SCI_CALLTIPSHOW,
                       e->SendScintilla(Editor::SCI_GETCURRENTPOS),
                       label.toUtf8().constData());
      return;
    }
    auto entries = result.isObject() && result.toObject().contains("items")
                       ? result.toObject()["items"].toArray()
                   : result.isObject() ? Arr{result}
                                       : result.toArray();
    QStringList labels;
    QList<Obj> choices;
    for (auto item : entries) {
      auto obj = item.toObject();
      QString label;
      if (kind == "completion")
        label = obj["label"].toString();
      else {
        auto uri = obj.value("targetUri")
                       .toString(obj.value("uri").toString(uriOf(e)));
        auto span = obj.value("targetSelectionRange")
                        .toObject(obj.value("range").toObject());
        if (!span.contains("start"))
          continue;
        auto pos = span["start"].toObject();
        label = QString("%1 · %2:%3  %4")
                    .arg(QUrl(uri).toLocalFile())
                    .arg(pos["line"].toInt() + 1)
                    .arg(pos["character"].toInt() + 1)
                    .arg(obj["message"].toString());
      }
      if (!label.isEmpty()) {
        labels << label;
        choices << obj;
      }
    }
    if (labels.isEmpty()) {
      notice(kind == "diagnostics" ? "未发现问题" : "没有匹配结果");
      return;
    }
    int index = 0;
    if (kind != "definition" || labels.size() > 1) {
      bool ok;
      auto value = QInputDialog::getItem(win, "Python 语言工具", "选择结果",
                                         labels, 0, false, &ok);
      if (!ok)
        return;
      index = labels.indexOf(value);
    }
    auto choice = choices[index];
    if (kind == "completion") {
      if (e->isReadOnly())
        return;
      auto edit = choice["textEdit"].toObject();
      auto span = edit.value("range").toObject(edit["replace"].toObject());
      long end = e->SendScintilla(Editor::SCI_GETCURRENTPOS);
      long start = e->SendScintilla(Editor::SCI_WORDSTARTPOSITION, end, 1L);
      if (!span.isEmpty()) {
        start = bytePos(e, span["start"].toObject());
        end = bytePos(e, span["end"].toObject());
      }
      e->SendScintilla(Editor::SCI_SETSEL, start, end);
      e->replaceSelectedText(edit.value("newText").toString(
          choice.value("insertText").toString(labels[index])));
    } else {
      auto uri = choice.value("targetUri")
                     .toString(choice.value("uri").toString(uriOf(e)));
      if (uri != uriOf(e)) {
        QString file = QUrl(uri).toLocalFile();
        if (file.isEmpty())
          return;
        open(file);
        e = pythonTab(tabs->currentWidget());
        if (!e || QUrl(uri) != QUrl(uriOf(e)))
          return;
      }
      auto span = choice.value("targetSelectionRange")
                      .toObject(choice.value("range").toObject());
      e->SendScintilla(Editor::SCI_GOTOPOS,
                       bytePos(e, span["start"].toObject()));
      e->ensureCursorVisible();
    }
    e->setFocus();
  }
  void structuralDiagnostics(Editor *e) {
    e->SendScintilla(Editor::SCI_COLOURISE, 0UL, -1L);
    struct Bracket {
      char value;
      int row, column;
    };
    QList<Bracket> stack;
    QString issue;
    int issueRow = 0, issueColumn = 0;
    QByteArray opening = "([{", closing = ")]}";
    for (int row = 0; row < e->lines() && issue.isEmpty(); ++row) {
      auto bytes = e->text(row).toUtf8();
      int start = e->SendScintilla(Editor::SCI_POSITIONFROMLINE, row);
      for (int i = 0; i < bytes.size(); ++i) {
        char c = bytes[i];
        if (!opening.contains(c) && !closing.contains(c))
          continue;
        if (e->SendScintilla(Editor::SCI_GETSTYLEAT, start + i) !=
            QsciLexerPython::Operator)
          continue;
        int column = QString::fromUtf8(bytes.left(i)).size();
        if (opening.contains(c)) {
          stack.append({c, row, column});
          continue;
        }
        if (stack.isEmpty() ||
            opening.indexOf(stack.takeLast().value) != closing.indexOf(c)) {
          issue = "闭括号不匹配";
          issueRow = row;
          issueColumn = column;
          break;
        }
      }
    }
    if (issue.isEmpty() && !stack.isEmpty()) {
      issue = "括号未闭合";
      issueRow = stack.last().row;
      issueColumn = stack.last().column;
    }
    if (issue.isEmpty()) {
      notice("本地括号检查未发现问题");
      return;
    }
    Obj point{{"line", issueRow}, {"character", issueColumn}};
    present(e, "diagnostics",
            Arr{Obj{{"range", Obj{{"start", point}}}, {"message", issue}}});
  }
  void local(Editor *e, QString kind) {
    auto pos = cursor(e);
    int row = pos["line"].toInt(), col = pos["character"].toInt();
    auto line = e->text(row);
    if (kind == "definition" || kind == "references") {
      QMap<QString, QString> buffers;
      for (int i = 0; i < tabs->count(); ++i)
        if (auto doc = pythonTab(tabs->widget(i)))
          buffers[pathOf(doc)] = doc->text();
      auto root = win->property("projectRoot").toString();
      if (root.isEmpty())
        root = QFileInfo(pathOf(e)).absolutePath();
      auto path = pathOf(e);
      QPointer<Editor> target = e;
      auto version = queryGeneration;
      auto watcher = new QFutureWatcher<Arr>(this);
      connect(watcher, &QFutureWatcher<Arr>::finished, this, [=] {
        watcher->deleteLater();
        if (queryGeneration == version && target &&
            tabs->currentWidget() == target && cursor(target) == pos &&
            target->text() == buffers.value(path))
          present(target, kind, watcher->result());
      });
      watcher->setFuture(QtConcurrent::run([=] {
        pyindex::Project project(root, buffers);
        return project.locations(path, row, col, kind == "references");
      }));
      return;
    }
    if (kind == "diagnostics") {
      auto exe = QStandardPaths::findExecutable("python3");
      if (exe.isEmpty())
        exe = QStandardPaths::findExecutable("python");
      if (exe.isEmpty()) {
        structuralDiagnostics(e);
        return;
      }
      auto p = new QProcess(this);
      QPointer<Editor> target = e;
      QString original = e->text();
      connect(p, &QProcess::errorOccurred, p,
              [this, p, target](QProcess::ProcessError err) {
                if (err == QProcess::FailedToStart) {
                  if (target && tabs->currentWidget() == target)
                    structuralDiagnostics(target);
                  p->deleteLater();
                }
              });
      connect(
          p, &QProcess::finished, p,
          [this, p, target, original](int code, QProcess::ExitStatus) {
            p->deleteLater();
            if (!target || target->text() != original ||
                tabs->currentWidget() != target)
              return;
            if (code) {
              structuralDiagnostics(target);
              return;
            }
            auto result =
                QJsonDocument::fromJson(p->readAllStandardOutput()).object();
            if (result.contains("line")) {
              Obj point{{"line", result["line"]},
                        {"character", result["column"]}};
              present(target, "diagnostics",
                      Arr{Obj{{"range", Obj{{"start", point}}},
                              {"message", result["message"]}}});
            } else
              notice("Python 语法检查未发现问题");
          });
      p->start(exe,
               {"-I", "-S", "-c",
                "import ast,json,sys\ntry: "
                "ast.parse(sys.stdin.buffer.read().decode('utf-8'))\nexcept "
                "SyntaxError as e: print(json.dumps(dict(line=(e.lineno or "
                "1)-1,column=(e.offset or 1)-1,message=e.msg)))"});
      p->write(original.toUtf8());
      p->closeWriteChannel();
      return;
    }
    auto file = pyindex::scan(pathOf(e), e->text());
    auto scope = file.scopes.value(row);
    if (kind == "signatureHelp") {
      auto call = QRegularExpression(R"(([\p{L}_][\p{L}\p{N}_]*)\s*\([^()]*$)")
                      .match(line.left(col));
      auto signature = file.signatures.value(
          pyindex::findDecl(file, scope, call.captured(1)));
      if (signature.isEmpty()) {
        notice("当前位置没有本地函数签名");
        return;
      }
      present(e, kind, Obj{{"signatures", Arr{Obj{{"label", signature}}}}});
      return;
    }
    auto prefix = QRegularExpression(R"([\p{L}\p{N}_]*$)")
                      .match(line.left(col))
                      .captured();
    auto lex = qobject_cast<QsciLexerPython *>(e->lexer());
    QStringList words =
        QString::fromLatin1(lex->keywords(1)).split(' ', Qt::SkipEmptyParts);
    words += QString("abs all any bool bytes callable chr dict dir enumerate "
                     "filter float format getattr hasattr int isinstance iter "
                     "len list map max min next object open print range repr "
                     "reversed round set sorted str sum super tuple type zip")
                 .split(' ');
    for (auto key : file.declarations) {
      auto owner = key.section('/', 0, -2);
      if (owner.isEmpty() || scope == owner || scope.startsWith(owner + "/"))
        words << key.section('/', -1);
    }
    words.removeDuplicates();
    words.sort();
    Arr results;
    for (auto symbol : words)
      if (symbol.startsWith(prefix))
        results.append(Obj{{"label", symbol}});
    present(e, kind, results);
  }

public:
  Language(QMainWindow *w, QTabWidget *t, std::function<void(QString)> fn)
      : QObject(w), win(w), tabs(t), open(std::move(fn)) {
    debounce.setSingleShot(true);
    debounce.setInterval(320);
    connect(&debounce, &QTimer::timeout, this, [this] { synchronize(); });
    connect(tabs, &QTabWidget::currentChanged, this,
            [this] { debounce.start(); });
    connect(&process, &QProcess::readyReadStandardOutput, this,
            [this] { receive(); });
    connect(&process, &QProcess::readyReadStandardError, this,
            [this] { process.readAllStandardError(); });
    connect(&process, &QProcess::finished, this, [this] { stopped(); });
    connect(&process, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError err) {
              if (err == QProcess::FailedToStart)
                stopped();
            });
    auto menu = win->menuBar()->addMenu("代码");
    auto sub = menu->addMenu("Python 语言工具");
    auto add = [this, sub](QString kind, QString label, QString shortcut) {
      auto action = sub->addAction(label);
      if (!shortcut.isEmpty())
        action->setShortcut(QKeySequence(shortcut));
      connect(action, &QAction::triggered, this, [this, kind] { run(kind); });
    };
    add("completion", "符号补全…", "Ctrl+Alt+Space");
    add("definition", "转到定义", "F12");
    add("references", "查找引用…", "Shift+F12");
    add("signatureHelp", "函数签名", "Ctrl+Shift+Space");
    add("diagnostics", "查看诊断…", {});
    connect(menu, &QMenu::aboutToShow, this,
            [this, sub] { sub->setEnabled(pythonTab(tabs->currentWidget())); });
  }
  ~Language() override {
    process.disconnect(this);
    if (process.state() != QProcess::NotRunning) {
      process.kill();
      process.waitForFinished(1000);
    }
  }
  void run(QString kind) {
    ++queryGeneration;
    auto e = pythonTab(tabs->currentWidget());
    if (!e)
      return;
    QPointer<Editor> target = e;
    auto uri = uriOf(e);
    auto where = cursor(e);
    auto original = e->text();
    auto active = [=] {
      return target && tabs->currentWidget() == target &&
             uriOf(target) == uri && cursor(target) == where &&
             target->text() == original;
    };
    auto execute = [=] {
      if (!active())
        return;
      if (!ready) {
        local(target, kind);
        return;
      }
      synchronize();
      Obj params{{"textDocument", Obj{{"uri", uri}}}, {"position", where}};
      if (kind == "references")
        params["context"] = Obj{{"includeDeclaration", true}};
      if (kind == "diagnostics" && !pull) {
        if (diagnostics.contains(uri))
          present(target, kind, diagnostics[uri]);
        else
          local(target, kind);
        return;
      }
      if (kind == "diagnostics")
        params.remove("position");
      request("textDocument/" + (kind == "diagnostics" ? "diagnostic" : kind),
              params, [=](QJsonValue data) {
                if (!active())
                  return;
                if (data.isUndefined())
                  local(target, kind);
                else
                  present(target, kind, data);
              });
    };
    if (ready) {
      execute();
      return;
    }
    if (starting || start(e))
      initialized << execute;
    else
      execute();
  }
};
} // namespace
void installLanguageTools(QMainWindow *window, QTabWidget *tabs,
                          std::function<void(QString)> openFile) {
  new Language(window, tabs, std::move(openFile));
}
