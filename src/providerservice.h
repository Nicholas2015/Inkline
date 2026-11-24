#pragma once

#include <QHash>
#include <QMap>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QSettings>
#include <functional>

namespace Ai {

QString endpoint();
QString model();
bool validEndpoint(const QString &url);
bool validToken(const QString &token);

class SessionCredentials {
  QHash<QPair<QString, QString>, QString> keys;
public:
  QString token(const QString &provider, const QString &endpoint) const;
  void setToken(const QString &provider, const QString &endpoint, const QString &token);
};

struct ProviderProfile {
  QString address;
  QString model;
  bool fullRoute = false;
  QString chatUrl(const QString &editedAddress) const;
};

class ProviderProfiles {
  QMap<QString, ProviderProfile> profiles;
  QString active;
public:
  explicit ProviderProfiles(const QSettings &settings);
  QStringList names() const { return profiles.keys(); }
  QString activeName() const { return active; }
  ProviderProfile profile(const QString &name) const { return profiles.value(name); }
  bool add(const QString &name);
  QString save(QSettings &settings, SessionCredentials &credentials, const QString &name,
               const QString &address, const QString &model, const QString &token);
};

struct ModelList {
  QStringList ids;
  QString error;
};

ModelList parseModelList(const QByteArray &body);

class ModelDiscovery : public QObject {
  QNetworkAccessManager net;
  QPointer<QNetworkReply> pending;
public:
  ~ModelDiscovery() override;
  void cancel();
  void fetch(const QString &chatUrl, const QString &key, std::function<void(ModelList)> done);
};

class ModelClient : public QObject {
  QNetworkAccessManager net;
public:
  using QObject::QObject;
  SessionCredentials credentials;
  QNetworkReply *ask(QObject *owner, const QString &instruction, const QString &context,
                     std::function<void(QString, QString)> done);
};

} // namespace Ai
