#pragma once
#include <QJsonObject>
#include <QWidget>
#include <memory>

class Log final : public QWidget {
public:
  explicit Log(const QString &file);
  ~Log() override;
  QString filePath() const;
  QJsonObject saveView() const;
  void restoreView(const QJsonObject &state);
  void showSearch();
  void setHighlightWords(const QStringList &words);
  void jumpDialog();
  void gotoLine(qint64 line);
  void gotoSearchHit(qint64 offset, const QString &codec, int matchLength);
  void gotoByte(qint64 offset, const QString &codec = {});

protected:
  bool eventFilter(QObject *, QEvent *) override;

private:
  struct State;
  std::unique_ptr<State> d;
};
