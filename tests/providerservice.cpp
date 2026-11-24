#include "providerservice.h"
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <optional>

namespace {
int failures = 0;
void check(bool ok, const char *description) {
  if (!ok) {
    qCritical() << description;
    ++failures;
  }
}

bool until(const std::function<bool()> &predicate) {
  QDeadlineTimer deadline(5000);
  while (!predicate() && !deadline.hasExpired())
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
  return predicate();
}

void profilesAndRoutes(const QString &path) {
  QSettings settings(path, QSettings::IniFormat);
  Ai::SessionCredentials session;
  Ai::ProviderProfiles defaults(settings);
  check(defaults.activeName() == "OpenAI", "fresh installation selects OpenAI");
  const QString custom = "https://legacy.example/api/generate";
  settings.setValue("ai/endpoint", custom);
  settings.setValue("ai/model", "old");
  Ai::ProviderProfiles profiles(settings);
  const auto legacy = profiles.activeName();
  check(legacy != "OpenAI" && profiles.profile(legacy).chatUrl(custom) == custom &&
        profiles.profile(legacy).model == "old", "legacy full route and model migrate");
  check(profiles.add("second") && !profiles.add("second") && !profiles.add(" "), "add a named provider once");
  const QString base = "https://mock.example/compat/v1";
  const QString route = base + "/chat/completions";
  check(profiles.profile("second").chatUrl(base + "///") == route &&
        profiles.profile("second").chatUrl(route) == route, "base or complete chat URL");
  const Ai::ProviderProfile complete{route + "/", "old", true};
  check(complete.chatUrl(complete.address) == route, "legacy chat route normalizes trailing slash for discovery");
  check(profiles.save(settings, session, "second", base, " m ", "session-token-2").isEmpty(),
        "save second provider");
  check(session.token("second", route) == "session-token-2" &&
        session.token(legacy, route).isEmpty() && session.token("second", custom).isEmpty(),
        "credentials scoped to provider and complete endpoint");
  check(profiles.save(settings, session, legacy, custom, "old", "session-token-1").isEmpty(),
        "switch to legacy provider");
  check(!profiles.save(settings, session, "second", base, "m", "bad\r\nheader").isEmpty() &&
        !profiles.save(settings, session, "second", base, " ", "ok").isEmpty() &&
        !profiles.save(settings, session, "second", "http://remote.example/v1", "m", "ok").isEmpty() &&
        settings.value("ai/endpoint") == custom && session.token("second", route) == "session-token-2",
        "invalid edits preserve active profile and credential");
  settings.sync();
  QSettings reopened(path, QSettings::IniFormat);
  Ai::ProviderProfiles saved(reopened);
  check(saved.activeName() == legacy && saved.profile(legacy).chatUrl(custom) == custom &&
        saved.profile("second").model == "m" && saved.profile("second").address == base &&
        reopened.value("ai/model") == "old", "both saved profiles reopen and active legacy keys match");
  QFile file(path);
  check(file.open(QIODevice::ReadOnly) && !file.readAll().contains("session-token"),
        "serialized settings contain no session credential");
  Ai::SessionCredentials newSession;
  check(newSession.token("second", route).isEmpty(), "credentials expire between sessions");
  check(Ai::validEndpoint("http://[::1]:8080/api") && Ai::validEndpoint("http://localhost/api") &&
        Ai::validEndpoint("https://remote.example/api"), "TLS and loopback endpoints accepted");
  for (const auto *bad : {"http://remote.example/api", "https://alice@mock.example/api",
                          "https://mock.example/api?secret=1", "https://mock.example/api#anchor",
                          "file:///api", "https://mock.example/", "https://mock.example/a b"})
    check(!Ai::validEndpoint(bad), bad);
}

// This server only binds to loopback. Each prefix exercises a real HTTP response.
class Server : public QTcpServer {
public:
  QList<QByteArray> requests;
  QPointer<QTcpSocket> held;
  Server() {
    connect(this, &QTcpServer::newConnection, this, [this] {
      auto *socket = nextPendingConnection();
      connect(socket, &QTcpSocket::readyRead, socket, [this, socket, input = QByteArray{}]() mutable {
        input += socket->readAll();
        if (!input.contains("\r\n\r\n")) return;
        requests.append(input);
        const auto path = input.split(' ').value(1);
        if (path == "/hold/models") {
          held = socket;
          return;
        }
        QByteArray status = "200 OK", headers, body = R"({"data":[{"id":"b"},{"id":"a"},{"id":"b"}]})";
        if (path == "/http-error/models") status = "401 Unauthorized";
        if (path == "/redirect/models") {
          status = "302 Found";
          headers = "Location: /other/models\r\n";
        }
        if (path == "/excess/models") body += QByteArray(2 * 1024 * 1024 + 1 - body.size(), ' ');
        if (path == "/empty/models") body = R"({"data":[]})";
        if (path == "/invalid/models") body = "{";
        socket->write("HTTP/1.1 " + status + "\r\n" + headers + "Content-Length: " +
                      QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
        socket->disconnectFromHost();
      });
    });
  }
};

void discovery() {
  const auto parsed = Ai::parseModelList(R"({"data":[{"id":" z "},{"id":"a"},{"id":"z"},{"id":4},{}]})");
  check(parsed.error.isEmpty() && parsed.ids == QStringList({"a","z"}), "normalize, sort and deduplicate IDs");
  Server server;
  if (!server.listen(QHostAddress::LocalHost)) {
    check(false, "listen on loopback");
    return;
  }
  const auto origin = QString("http://127.0.0.1:%1").arg(server.serverPort());
  Ai::ModelDiscovery client;
  auto fetch = [&](const QString &route, const QString &key = {}) {
    std::optional<Ai::ModelList> result;
    client.fetch(route, key, [&](Ai::ModelList list) { result = std::move(list); });
    const bool completed = until([&] { return result.has_value(); });
    if (!completed) client.cancel();
    check(completed, "model discovery completed");
    return result.value_or(Ai::ModelList{{}, "timed out"});
  };
  const auto list = fetch(origin + "/api/v2/chat/completions", "test-session");
  check(list.ids == QStringList({"a","b"}) && list.error.isEmpty() &&
        server.requests.value(0).startsWith("GET /api/v2/models HTTP/") &&
        server.requests.value(0).contains("Authorization: Bearer test-session\r\n"),
        "discover through sibling models route with bearer key");
  const auto requestCount = server.requests.size();
  check(!fetch(origin + "/custom").error.isEmpty() &&
        !fetch(origin + "/api/chat/completions", "one\r\ntwo").error.isEmpty() &&
        server.requests.size() == requestCount, "invalid route and header rejected before dispatch");
  const auto unauthorized = fetch(origin + "/http-error/chat/completions");
  check(unauthorized.ids.isEmpty() && unauthorized.error.contains("401"), "authorization failure reported");
  const auto empty = fetch(origin + "/empty/chat/completions");
  const auto invalid = fetch(origin + "/invalid/chat/completions");
  check(empty.ids.isEmpty() && !empty.error.isEmpty() && invalid.ids.isEmpty() &&
        !invalid.error.isEmpty() && invalid.error != empty.error, "empty and malformed results distinguished");
  const auto beforeRedirect = server.requests.size();
  check(fetch(origin + "/redirect/chat/completions", "private-key").error.contains("302") &&
        server.requests.size() == beforeRedirect + 1, "redirect target receives no request or key");
  const auto excess = fetch(origin + "/excess/chat/completions");
  check(excess.ids.isEmpty() && !excess.error.isEmpty(), "oversized response rejected");

  int staleCallbacks = 0;
  client.fetch(origin + "/hold/chat/completions", {}, [&](Ai::ModelList) { ++staleCallbacks; });
  check(until([&] { return !server.held.isNull(); }), "first request in flight");
  const auto current = fetch(origin + "/new/chat/completions");
  check(!current.ids.isEmpty() && staleCallbacks == 0 && !server.requests.last().contains("Authorization:"),
        "replacing a request suppresses its callback and clears its key");
  server.held.clear();
  client.fetch(origin + "/hold/chat/completions", {}, [&](Ai::ModelList) { ++staleCallbacks; });
  check(until([&] { return !server.held.isNull(); }), "second request in flight");
  client.cancel();
  check(until([&] { return !server.held || server.held->state() == QAbstractSocket::UnconnectedState; }) &&
        staleCallbacks == 0, "cancel closes HTTP request without invoking stale UI");
}
} // namespace

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  QTemporaryDir temporary;
  if (!temporary.isValid()) return 1;
  profilesAndRoutes(temporary.filePath("settings.ini"));
  discovery();
  return failures ? 1 : 0;
}
