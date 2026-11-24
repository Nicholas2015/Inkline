#include "providerservice.h"
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace Ai {
namespace {
const QString chatSuffix = QStringLiteral("/chat/completions");
constexpr qint64 modelListLimit = 2 * 1024 * 1024;
constexpr qint64 chatLimit = 1024 * 1024;

QNetworkRequest requestFor(const QString &url, const QString &token, int timeoutMs) {
  QNetworkRequest request{QUrl(url, QUrl::StrictMode)};
  request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
  request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
  request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
  request.setTransferTimeout(timeoutMs);
  if (!token.isEmpty()) request.setRawHeader("Authorization", "Bearer " + token.toUtf8());
  return request;
}

// Keep at most one byte beyond the limit so readyRead can detect overflow.
void boundReply(QNetworkReply *reply, qint64 limit) {
  reply->setReadBufferSize(limit + 1);
  QObject::connect(reply, &QIODevice::readyRead, reply, [reply, limit] {
    if (reply->bytesAvailable() > limit && reply->isRunning()) reply->abort();
  });
}
} // namespace

QString endpoint() { return QSettings().value("ai/endpoint").toString(); }
QString model() { return QSettings().value("ai/model").toString(); }

bool validEndpoint(const QString &address) {
  const QUrl url(address, QUrl::StrictMode);
  const bool local = url.host() == "localhost" || QHostAddress(url.host()).isLoopback();
  return url.isValid() && !url.host().isEmpty() && url.userInfo().isEmpty() &&
         !url.hasQuery() && !url.hasFragment() && url.path().size() > 1 &&
         (url.scheme() == "https" || (url.scheme() == "http" && local));
}

bool validToken(const QString &token) {
  static const QRegularExpression invalid("[^!-~]");
  return !token.contains(invalid);
}

QString SessionCredentials::token(const QString &provider, const QString &endpoint) const {
  return keys.value({provider, endpoint});
}
void SessionCredentials::setToken(const QString &provider, const QString &endpoint, const QString &token) {
  if (token.isEmpty()) keys.remove({provider, endpoint});
  else keys.insert({provider, endpoint}, token);
}

QString ProviderProfile::chatUrl(const QString &editedAddress) const {
  auto url = editedAddress.trimmed();
  while (url.endsWith('/')) url.chop(1);
  if (url.isEmpty() || url.endsWith(chatSuffix)) return url;
  if (fullRoute && (editedAddress.trimmed() == address || url == address)) return address;
  return url + chatSuffix;
}

ProviderProfiles::ProviderProfiles(const QSettings &settings) {
  const auto entries = settings.value("ai/providers").toMap();
  for (auto it = entries.cbegin(); it != entries.cend(); ++it) {
    const auto value = it.value().toMap();
    profiles.insert(it.key(), {value.value("url").toString(), value.value("model").toString(),
                               value.value("fullRoute").toBool()});
  }
  if (!profiles.contains("OpenAI"))
    profiles.insert("OpenAI", {"https://api.openai.com/v1", {}, false});
  active = settings.value("ai/provider").toString();
  if (!profiles.contains(active)) {
    const auto legacy = settings.value("ai/endpoint").toString();
    active = legacy.isEmpty() ? "OpenAI" : "已有配置";
    if (!legacy.isEmpty())
      profiles.insert(active, {legacy, settings.value("ai/model").toString(), true});
  }
}

bool ProviderProfiles::add(const QString &name) {
  if (name.trimmed().isEmpty() || profiles.contains(name)) return false;
  profiles.insert(name, {});
  return true;
}

QString ProviderProfiles::save(QSettings &settings, SessionCredentials &credentials, const QString &name,
                               const QString &address, const QString &model, const QString &token) {
  const auto url = profile(name).chatUrl(address);
  const auto id = model.trimmed();
  if (!validEndpoint(url) || id.isEmpty())
    return "请填写完整 HTTPS 接口路径（本机可用 HTTP）及模型名称。";
  const auto key = token.trimmed();
  if (!validToken(key)) return "API Key 含无效字符。";
  active = name;
  profiles[name] = {address.trimmed(), id, address.trimmed() == url};
  QVariantMap entries;
  for (auto it = profiles.cbegin(); it != profiles.cend(); ++it) {
    const auto &p = it.value();
    entries.insert(it.key(), QVariantMap{{"url", p.address}, {"model", p.model}, {"fullRoute", p.fullRoute}});
  }
  settings.setValue("ai/providers", entries);
  settings.setValue("ai/provider", active);
  settings.setValue("ai/endpoint", url);
  settings.setValue("ai/model", id);
  credentials.setToken(name, url, key);
  return {};
}

ModelList parseModelList(const QByteArray &body) {
  const auto data = QJsonDocument::fromJson(body).object().value("data");
  if (!data.isArray()) return {{}, "模型列表格式无效，预期 data 数组。"};
  QStringList ids;
  for (const auto &entry : data.toArray()) {
    const auto id = entry.toObject().value("id").toString().trimmed();
    if (!id.isEmpty()) ids.append(id);
  }
  ids.removeDuplicates();
  ids.sort();
  if (ids.isEmpty()) return {{}, "服务未返回可用模型。"};
  return {ids, {}};
}

ModelDiscovery::~ModelDiscovery() { cancel(); }
void ModelDiscovery::cancel() {
  if (!pending) return;
  auto *reply = pending.data();
  pending.clear();
  reply->abort();
}

void ModelDiscovery::fetch(const QString &chatUrl, const QString &key, std::function<void(ModelList)> done) {
  cancel();
  if (!validEndpoint(chatUrl) || !chatUrl.endsWith(chatSuffix))
    return done({{}, "请填写兼容服务的基础地址或 /chat/completions 接口。"});
  const auto token = key.trimmed();
  if (!validToken(token)) return done({{}, "API Key 含无效字符。"});
  const auto url = chatUrl.chopped(chatSuffix.size()) + "/models";
  auto *reply = net.get(requestFor(url, token, 15000));
  pending = reply;
  boundReply(reply, modelListLimit);
  connect(reply, &QNetworkReply::finished, this, [this, reply, done = std::move(done)] {
    reply->deleteLater();
    if (pending != reply) return;
    pending.clear();
    if (reply->bytesAvailable() > modelListLimit) return done({{}, "模型列表超过 2 MiB。"});
    const auto code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError || code != 200)
      return done({{}, QString("获取失败（%1）。可检查地址和密钥，或手动填写模型。")
                            .arg(code ? QString::number(code) : reply->errorString())});
    done(parseModelList(reply->readAll()));
  });
}

QNetworkReply *ModelClient::ask(QObject *owner, const QString &instruction, const QString &context,
                                std::function<void(QString, QString)> done) {
  const QSettings settings;
  const auto url = settings.value("ai/endpoint").toString();
  const auto id = settings.value("ai/model").toString();
  const auto key = credentials.token(settings.value("ai/provider").toString(), url);
  if (!validEndpoint(url) || id.trimmed().isEmpty() || !validToken(key)) {
    done({}, "模型接口、模型名称或 API Key 无效。");
    return nullptr;
  }
  auto request = requestFor(url, key, 60000);
  request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
  QJsonArray messages{
      QJsonObject{{"role", "system"}, {"content", "Return only the requested code or JSON without Markdown fences."}},
      QJsonObject{{"role", "user"}, {"content", instruction + "\n\n" + context}}};
  auto *reply = net.post(request, QJsonDocument(QJsonObject{{"model", id}, {"messages", messages}}).toJson());
  boundReply(reply, chatLimit);
  connect(owner, &QObject::destroyed, reply, [reply, owner] {
    QObject::disconnect(reply, nullptr, owner, nullptr);
    if (reply->isRunning()) reply->abort();
  });
  connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
  connect(reply, &QNetworkReply::finished, owner, [reply, done = std::move(done)] {
    if (reply->bytesAvailable() > chatLimit) return done({}, "模型响应超过 1 MiB");
    const auto code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError || code < 200 || code >= 300)
      return done({}, QString("请求失败（HTTP %1）%2").arg(code).arg(reply->errorString()));
    const auto choices = QJsonDocument::fromJson(reply->readAll()).object()["choices"].toArray();
    auto answer = choices.isEmpty() ? QString() : choices.first().toObject()["message"].toObject()["content"].toString();
    static const QRegularExpression fence(R"(^\s*[\x60]{3}[^\n]*\n([\s\S]*?)\r?\n[\x60]{3}\s*$)");
    const auto match = fence.match(answer);
    if (match.hasMatch()) answer = match.captured(1);
    if (answer.trimmed().isEmpty() || answer.contains(QChar::Null)) return done({}, "服务没有返回有效文本");
    done(answer, {});
  });
  return reply;
}

} // namespace Ai
