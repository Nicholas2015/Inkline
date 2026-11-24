#pragma once
#include "providerservice.h"
#include <QtWidgets>

// Each saved profile has a chat endpoint and selected model. Credentials are session-only.
class ModelProviders : public QWidget {
  QComboBox providers, models;
  QLineEdit address, key;
  QPushButton fetch{"获取模型"};
  QLabel status;
  Ai::ProviderProfiles profiles{QSettings()};
  Ai::SessionCredentials &credentials;
  Ai::ModelDiscovery discovery;

  QString chatUrl() const {
    return profiles.profile(providers.currentText()).chatUrl(address.text());
  }
  void invalidate() {
    discovery.cancel();
    fetch.setEnabled(true);
    status.clear();
  }
  void selectProfile() {
    invalidate();
    const auto profile = profiles.profile(providers.currentText());
    address.setText(profile.address);
    key.setText(credentials.token(providers.currentText(), chatUrl()));
    models.clear();
    models.setEditText(profile.model);
  }
  void discover() {
    fetch.setEnabled(false);
    status.setText("正在获取模型…");
    discovery.fetch(chatUrl(), key.text(), [this](Ai::ModelList result) {
      fetch.setEnabled(true);
      if (!result.error.isEmpty()) {
        status.setText(result.error);
        return;
      }
      const auto previous = models.currentText().trimmed();
      models.clear();
      models.addItems(result.ids);
      if (result.ids.contains(previous)) models.setCurrentText(previous);
      status.setText(QString("已获取 %1 个模型").arg(result.ids.size()));
    });
  }

public:
  explicit ModelProviders(Ai::SessionCredentials &keys) : credentials(keys) {
    providers.setObjectName("modelProvider");
    providers.addItems(profiles.names());
    providers.setCurrentText(profiles.activeName());
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
      if (profiles.add(name)) providers.addItem(name);
      providers.setCurrentText(name);
    });
    connect(&providers, &QComboBox::currentIndexChanged, this, [this] { selectProfile(); });
    connect(&address, &QLineEdit::textChanged, this, [this] {
      invalidate();
      models.clear();
      key.setText(credentials.token(providers.currentText(), chatUrl()));
    });
    connect(&key, &QLineEdit::textChanged, this, [this] { invalidate(); });
    connect(&fetch, &QPushButton::clicked, this, [this] { discover(); });
    selectProfile();
  }
  QString save() {
    QSettings settings;
    return profiles.save(settings, credentials, providers.currentText(), address.text(),
                         models.currentText(), key.text());
  }
};
