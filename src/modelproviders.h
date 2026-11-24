#pragma once
#include <QtNetwork>
#include <QtWidgets>

// Each saved profile has a chat endpoint and selected model. Credentials are session-only.
class ModelProviders : public QWidget {
  QComboBox providers, models;
  QLineEdit address, key;
  QPushButton fetch{"获取模型"};
  QLabel status;
  QNetworkAccessManager net;
  QPointer<QNetworkReply> pending;
  QVariantMap profiles;
  QHash<QPair<QString, QString>, QString> &credentials;
  std::function<bool(const QUrl &)> valid;

  void invalidate() {
    if (pending) {
      auto *reply = pending.data();
      pending.clear();
      reply->abort();
    }
    fetch.setEnabled(true);
    status.clear();
  }
  void selectProfile() {
    invalidate();
    const auto profile = profiles.value(providers.currentText()).toMap();
    address.setText(profile.value("url").toString());
    key.setText(credentials.value({providers.currentText(), chatUrl()}));
    models.clear();
    models.setEditText(profile.value("model").toString());
  }
  void discover() {
    const auto chat = chatUrl();
    if (!valid(QUrl(chat, QUrl::StrictMode)) || !chat.endsWith("/chat/completions")) {
      status.setText("请填写兼容服务的基础地址或 /chat/completions 接口。");
      return;
    }
    if (!validKey()) {
      status.setText("API Key 含无效字符。");
      return;
    }
    QNetworkRequest req{QUrl(chat.chopped(QStringLiteral("/chat/completions").size()) + "/models")};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    req.setTransferTimeout(15000);
    if (!token().isEmpty()) req.setRawHeader("Authorization", "Bearer " + token().toUtf8());
    auto *reply = net.get(req);
    pending = reply;
    fetch.setEnabled(false);
    status.setText("正在获取模型…");
    constexpr qint64 limit = 2 * 1024 * 1024;
    reply->setReadBufferSize(limit + 1);
    connect(reply, &QIODevice::readyRead, reply, [reply] {
      if (reply->bytesAvailable() > limit) reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
      reply->deleteLater();
      if (pending != reply) return;
      pending.clear();
      fetch.setEnabled(true);
      const auto code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
      if (reply->error() != QNetworkReply::NoError || code != 200) {
        status.setText(QString("获取失败（%1）。可检查地址和密钥，或手动填写模型。")
                       .arg(code ? QString::number(code) : reply->errorString()));
        return;
      }
      const auto entries = QJsonDocument::fromJson(reply->readAll()).object().value("data");
      QStringList ids;
      for (const auto &entry : entries.toArray()) {
        const auto id = entry.toObject().value("id").toString().trimmed();
        if (!id.isEmpty()) ids.append(id);
      }
      ids.removeDuplicates();
      ids.sort();
      if (ids.isEmpty()) {
        status.setText(entries.isArray() ? "服务未返回可用模型。" : "模型列表格式无效，预期 data 数组。");
        return;
      }
      const auto previous = selectedModel();
      models.clear();
      models.addItems(ids);
      if (ids.contains(previous)) models.setCurrentText(previous);
      status.setText(QString("已获取 %1 个模型").arg(ids.size()));
    });
  }

public:
  ModelProviders(const QString &legacyUrl, const QString &legacyModel,
                 QHash<QPair<QString, QString>, QString> &keys, std::function<bool(const QUrl &)> check)
      : credentials(keys), valid(std::move(check)) {
    QSettings settings;
    profiles = settings.value("ai/providers").toMap();
    if (!profiles.contains("OpenAI")) profiles.insert("OpenAI", QVariantMap{{"url","https://api.openai.com/v1"}});
    auto active = settings.value("ai/provider").toString();
    if (!profiles.contains(active)) {
      active = legacyUrl.isEmpty() ? "OpenAI" : "已有配置";
      if (!legacyUrl.isEmpty()) profiles.insert(active, QVariantMap{{"url",legacyUrl},{"model",legacyModel},{"fullRoute",true}});
    }
    providers.setObjectName("modelProvider");
    providers.addItems(profiles.keys());
    providers.setCurrentText(active);
    models.setEditable(true);
    models.setObjectName("providerModels");
    key.setEchoMode(QLineEdit::Password);
    address.setPlaceholderText("https://example.com/v1");
    status.setWordWrap(true);
    status.setTextFormat(Qt::PlainText);
    auto *form = new QFormLayout(this);
    auto *row = new QHBoxLayout;
    row->addWidget(&providers, 1);
    auto *add = new QPushButton("添加供应商…");
    row->addWidget(add);
    form->addRow("模型供应商", row);
    form->addRow("API 地址", &address);
    form->addRow("API Key", &key);
    auto *picker = new QHBoxLayout;
    picker->addWidget(&models, 1);
    picker->addWidget(&fetch);
    form->addRow("模型", picker);
    form->addRow(QString(), &status);
    connect(add, &QPushButton::clicked, this, [this] {
      const auto name = QInputDialog::getText(this,"添加供应商","供应商名称").trimmed();
      if (name.isEmpty()) return;
      if (!profiles.contains(name)) { profiles.insert(name,QVariantMap{}); providers.addItem(name); }
      providers.setCurrentText(name);
    });
    connect(&providers, &QComboBox::currentIndexChanged, this, [this] { selectProfile(); });
    connect(&address, &QLineEdit::textChanged, this, [this] {
      invalidate();
      models.clear();
      key.setText(credentials.value({providers.currentText(), chatUrl()}));
    });
    connect(&key, &QLineEdit::textChanged, this, [this] { invalidate(); });
    connect(&fetch, &QPushButton::clicked, this, [this] { discover(); });
    selectProfile();
  }
  ~ModelProviders() override {
    if (pending) { pending->disconnect(this); pending->abort(); }
  }
  QString chatUrl() const {
    auto url = address.text().trimmed();
    while (url.endsWith('/')) url.chop(1);
    if (url.isEmpty() || url.endsWith("/chat/completions")) return url;
    // An existing installation may have a custom, complete chat route.
    const auto profile = profiles.value(providers.currentText()).toMap();
    if (profile.value("fullRoute").toBool() && url == profile.value("url").toString()) return url;
    return url + "/chat/completions";
  }
  QString selectedModel() const { return models.currentText().trimmed(); }
  QString token() const { return key.text().trimmed(); }
  bool validKey() const { return !token().contains(QRegularExpression("[^!-~]")); }
  void save(QSettings &settings) {
    const QString name = providers.currentText();
    profiles[name] = QVariantMap{{"url", address.text().trimmed()}, {"model", selectedModel()},
                                 {"fullRoute", address.text().trimmed() == chatUrl()}};
    settings.setValue("ai/providers", profiles);
    settings.setValue("ai/provider", name);
    credentials[{name, chatUrl()}] = token();
  }
};
