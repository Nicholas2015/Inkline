#include "searchtools.h"
#include "document.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QToolButton>
#include <Qsci/qsciscintilla.h>
#include <QtWidgets>
#include <memory>

namespace {
using Editor = QsciScintilla;
using Cache = std::shared_ptr<QTemporaryDir>;
QString filePath(QWidget *w) {
  return w ? w->property("filePath").toString() : QString();
}
bool save(const QString &path, const QByteArray &data) {
  QFile f(path);
  return f.open(QIODevice::WriteOnly) && f.write(data) == data.size() &&
         f.flush();
}
class Job : public QObject {
  QProcess *process = nullptr;

public:
  explicit Job(QObject *owner) : QObject(owner) {}
  ~Job() override { cancel(); }
  void cancel() {
    if (!process)
      return;
    auto *old = process;
    process = nullptr;
    old->kill();
    if (old->state() != QProcess::NotRunning)
      old->waitForFinished(1000);
    old->deleteLater();
  }
  void run(Cache cache, const QJsonObject &p,
           std::function<void(QJsonObject)> done) {
    cancel();
    const auto request = cache->filePath("request.json");
    if (!cache->isValid() ||
        !save(request, QJsonDocument(p).toJson(QJsonDocument::Compact))) {
      done({{"error", "无法写入临时文件"}});
      return;
    }
    process = new QProcess(this);
    auto *child = process;
    connect(child, &QProcess::errorOccurred, this,
            [=](QProcess::ProcessError error) {
              if (child != process || error != QProcess::FailedToStart)
                return;
              process = nullptr;
              child->deleteLater();
              done({{"error", child->errorString()}});
            });
    connect(child, &QProcess::finished, this,
            [=](int code, QProcess::ExitStatus status) {
              if (child != process)
                return;
              process = nullptr;
              child->deleteLater();
              QFile f(cache->filePath("reply.json"));
              if (code || status != QProcess::NormalExit ||
                  !f.open(QIODevice::ReadOnly)) {
                done({{"error", "搜索进程退出，请重试。"}});
                return;
              }
              auto answer = QJsonDocument::fromJson(f.readAll()).object();
              if (answer.isEmpty())
                answer.insert("error", "搜索结果不完整");
              done(answer);
            });
    child->start(QCoreApplication::applicationFilePath(),
                 {"--search-worker", request});
  }
};

class Results : public QWidget {
  QListWidget *rows = new QListWidget;
  QPushButton *prev = new QPushButton("上一批结果"),
              *next = new QPushButton("下一批结果");
  QLabel *position = new QLabel;
  Cache cache;
  qint64 count = 0, page = 0;
  void showPage(qint64 number) {
    page = number;
    rows->clear();
    if (cache && count) {
      QFile f(cache->filePath(QString::number(page) + ".jsonl"));
      if (f.open(QIODevice::ReadOnly))
        while (!f.atEnd()) {
          auto row = QJsonDocument::fromJson(f.readLine()).array();
          if (row.isEmpty())
            continue;
          QString title = row[0].toString();
          if (row[1].toInteger())
            title += QString(":%1    %2")
                         .arg(row[1].toInteger())
                         .arg(row[4].toString());
          auto *item = new QListWidgetItem(title, rows);
          item->setData(Qt::UserRole, row);
        }
    }
    prev->setEnabled(page > 0);
    next->setEnabled((page + 1) * 512 < count);
    position->setText(
        count ? QString("第 %1 / %2 页").arg(page + 1).arg((count + 511) / 512)
              : QString());
  }

public:
  std::function<void(const QJsonArray &)> activate;
  Results() {
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->addWidget(rows);
    rows->setAlternatingRowColors(true);
    rows->setAccessibleName("搜索结果");
    rows->setUniformItemSizes(true);
    auto *h = new QHBoxLayout;
    h->addWidget(prev);
    h->addWidget(position);
    h->addWidget(next);
    h->addStretch();
    v->addLayout(h);
    connect(prev, &QPushButton::clicked, this, [this] { showPage(page - 1); });
    connect(next, &QPushButton::clicked, this, [this] { showPage(page + 1); });
    connect(rows, &QListWidget::itemActivated, this,
            [this](QListWidgetItem *i) {
              if (activate)
                activate(i->data(Qt::UserRole).toJsonArray());
            });
  }
  void reset(Cache data = {}, qint64 total = 0) {
    cache = std::move(data);
    count = total;
    showPage(0);
  }
};
int utf16Position(const QByteArray &utf8, long byte) {
  return QString::fromUtf8(utf8.constData(), byte).size();
}
void select(Editor *e, const QString &text, int left, int right) {
  auto a = QStringView(text).left(left).toUtf8().size();
  auto b = a + QStringView(text).mid(left, right - left).toUtf8().size();
  e->SendScintilla(Editor::SCI_SETSEL, (unsigned long)a, (long)b);
  e->SendScintilla(Editor::SCI_SCROLLCARET);
}
class Find : public QDialog {
  QTabWidget *tabs;
  Job job{this};
  QComboBox *field = new QComboBox;
  QLineEdit *replacement = new QLineEdit;
  QCheckBox *mode = new QCheckBox("替换"), *regex = new QCheckBox("正则"),
            *whole = new QCheckBox("整词"),
            *sensitive = new QCheckBox("区分大小写"),
            *scope = new QCheckBox("仅选区");
  QLabel *status = new QLabel;
  QPushButton *cancel = new QPushButton("取消搜索");
  Results *results = new Results;
  QPointer<Editor> origin;
  QString scopeText, emptyQuery;
  int lo = 0, hi = 0, lastEmpty = -1;
  struct Snapshot {
    QPointer<Editor> tab;
    QString text;
  };
  QVector<Snapshot> shown;
  QString query() const { return field->currentText(); }
  void history() {
    const auto term = query();
    auto items = QSettings().value("find/history").toStringList();
    items.removeAll(term);
    items.prepend(term);
    items = items.mid(0, 32);
    QSettings().setValue("find/history", items);
    field->clear();
    field->addItems(items);
    field->setEditText(term);
  }

public:
  Find(QMainWindow *window, QTabWidget *t) : QDialog(window), tabs(t) {
    setWindowTitle("查找和替换");
    setMinimumWidth(488);
    auto *v = new QVBoxLayout(this);
    auto *h = new QHBoxLayout;
    v->setContentsMargins(20, 16, 20, 16);
    v->setSpacing(12);
    field->setEditable(true);
    field->setInsertPolicy(QComboBox::NoInsert);
    field->addItems(QSettings().value("find/history").toStringList());
    field->setEditText("");
    field->lineEdit()->setPlaceholderText("查找文本");
    auto *findLabel = new QLabel("查找内容");
    findLabel->setBuddy(field);
    h->addWidget(findLabel);
    h->addWidget(field, 1);
    h->addWidget(mode);
    v->addLayout(h);
    replacement->setPlaceholderText("替换为");
    auto *replaceRow = new QHBoxLayout;
    auto *replaceLabel = new QLabel("替换为");
    replaceLabel->setMinimumWidth(findLabel->sizeHint().width());
    replaceLabel->setBuddy(replacement);
    replaceRow->addWidget(replaceLabel);
    replaceRow->addWidget(replacement,1);
    v->addLayout(replaceRow);
    h = new QHBoxLayout;
    for (auto *c : {sensitive, whole, regex, scope})
      h->addWidget(c);
    sensitive->setChecked(true);
    v->addLayout(h);
    h = new QHBoxLayout;
    auto button = [&](QString label, QString op) {
      auto *b = new QPushButton(label);
      b->setAutoDefault(false);
      h->addWidget(b);
      connect(b, &QPushButton::clicked, this, [this, op] { run(op); });
      return b;
    };
    auto *actions = h;
    h = replaceRow;
    auto *one = button("替换", "replace"), *all = button("全部替换", "all");
    for (QWidget *w : QList<QWidget *>{one, all, replacement, replaceLabel}) {
      w->hide();
      connect(mode, &QCheckBox::toggled, w, &QWidget::setVisible);
    }
    h = actions;
    auto *list = new QToolButton;
    list->setText("查找全部");
    list->setPopupMode(QToolButton::MenuButtonPopup);
    auto *scopes = new QMenu(list);
    list->setMenu(scopes);
    connect(list, &QToolButton::clicked, this, [this] { run("list"); });
    scopes->addAction("当前文档", this, [this] { run("list"); });
    scopes->addAction("所有编辑标签", this, [this] { run("tabs"); });
    h->addWidget(list);
    button("计数", "count")->setProperty("quiet", true);
    h->addStretch();
    button("上一处", "prev");
    button("下一处", "next")->setProperty("primary", true);
    v->addLayout(h);
    h = new QHBoxLayout;
    h->addWidget(status,1);
    cancel->setAutoDefault(false);
    cancel->setEnabled(false);
    h->addWidget(cancel);
    v->addLayout(h);
    status->setProperty("secondary", true);
    v->insertWidget(v->count() - 2, results, 1);
    results->hide();
    connect(cancel, &QPushButton::clicked, this, [this] {
      job.cancel();
      cancel->setEnabled(false);
      status->setText("搜索已取消");
    });
    connect(field->lineEdit(), &QLineEdit::returnPressed, this,
            [this] { run("next"); });
    connect(replacement, &QLineEdit::returnPressed, this,
            [this] { run("replace"); });
    connect(this, &QDialog::finished, this, [this] {
      job.cancel();
      cancel->setEnabled(false);
    });
    results->activate = [this](const QJsonArray &r) {
      int id = r[5].toInt();
      if (id < 0 || id >= shown.size())
        return;
      auto s = shown[id];
      if (!s.tab || tabs->indexOf(s.tab) < 0 || s.tab->text() != s.text) {
        status->setText("文档已变化，请重新查找");
        return;
      }
      tabs->setCurrentWidget(s.tab);
      select(s.tab, s.text, r[2].toInt(), r[3].toInt());
      s.tab->setFocus();
    };
  }
  void openFor(Editor *editor, bool replace) {
    job.cancel();
    cancel->setEnabled(false);
    origin = editor;
    scopeText = editor->text();
    auto bytes = scopeText.toUtf8();
    lo = utf16Position(bytes,
                       editor->SendScintilla(Editor::SCI_GETSELECTIONSTART));
    hi = utf16Position(bytes,
                       editor->SendScintilla(Editor::SCI_GETSELECTIONEND));
    auto selected = scopeText.mid(lo, hi - lo);
    if (lo != hi && !selected.contains('\r') && !selected.contains('\n'))
      field->setEditText(selected);
    scope->setEnabled(lo != hi);
    scope->setChecked(false);
    mode->setChecked(replace);
    results->hide();
    shown.clear();
    results->reset();
    status->clear();
    setWindowTitle("查找和替换 — " + tabs->tabText(tabs->currentIndex()));
    adjustSize();
    show();
    raise();
    activateWindow();
    field->lineEdit()->selectAll();
    field->setFocus();
  }
  void run(QString op) {
    auto *editor = qobject_cast<Editor *>(tabs->currentWidget());
    if (!editor)
      return;
    if (query().isEmpty()) {
      openFor(editor, false);
      status->setText("请输入文本");
      return;
    }
    if (op != "tabs" && scope->isChecked() &&
        (editor != origin || editor->text() != scopeText)) {
      status->setText("选区已变化，请重新选择范围");
      return;
    }
    history();
    auto cache = std::make_shared<QTemporaryDir>();
    QVector<Snapshot> snapshots;
    QJsonArray docs;
    const bool allTabs = op == "tabs";
    for (int i = 0; i < tabs->count(); ++i) {
      auto *e = qobject_cast<Editor *>(tabs->widget(i));
      if (!e || (!allTabs && e != editor))
        continue;
      auto value = e->text();
      auto id = snapshots.size();
      auto filename = QString::number(id) + ".txt";
      if (!save(cache->filePath(filename), value.toUtf8())) {
        status->setText("无法暂存文档");
        return;
      }
      bool scoped = !allTabs && e == origin && scope->isChecked();
      docs.append(QJsonObject{{"label", tabs->tabText(i)},
                              {"snapshot", filename},
                              {"id", id},
                              {"lo", scoped ? lo : 0},
                              {"hi", scoped ? hi : value.size()}});
      snapshots.append({e, value});
    }
    auto bytes = editor->text().toUtf8();
    QJsonObject p{
        {"query", query()},
        {"case", sensitive->isChecked()},
        {"whole", whole->isChecked()},
        {"regex", regex->isChecked()},
        {"replacement", replacement->text()},
        {"docs", docs},
        {"op", allTabs ? "list" : op},
        {"empty", emptyQuery == query() ? lastEmpty : -1},
        {"left", utf16Position(bytes, editor->SendScintilla(
                                          Editor::SCI_GETSELECTIONSTART))},
        {"right", utf16Position(bytes, editor->SendScintilla(
                                           Editor::SCI_GETSELECTIONEND))}};
    cancel->setEnabled(true);
    status->setText("正在查找…");
    QPointer<Editor> active = editor;
    job.run(cache, p, [=](QJsonObject answer) {
      cancel->setEnabled(false);
      if (answer.contains("error")) {
        status->setText(answer["error"].toString());
        return;
      }
      status->setText(answer["status"].toString());
      if (op == "list" || allTabs) {
        shown = snapshots;
        results->reset(cache, answer["count"].toInteger());
        results->show();
        resize(qMax(width(), 600), 430);
        return;
      }
      auto s = snapshots.first();
      if (!active || active != tabs->currentWidget() ||
          active->text() != s.text) {
        status->setText("文档已变化，请重新查找");
        return;
      }
      if (!answer.contains("a"))
        return;
      int a = answer["a"].toInt(), b = answer["b"].toInt();
      select(active, s.text, a, b);
      if (answer["edit"].toBool()) {
        auto value = answer["value"].toString();
        if (s.text.mid(a, b - a) != value) {
          auto utf8 = value.toUtf8();
          active->SendScintilla(Editor::SCI_TARGETFROMSELECTION);
          active->SendScintilla(Editor::SCI_REPLACETARGET,
                                (uintptr_t)utf8.size(), utf8.constData());
          if (scope->isChecked()) {
            scopeText = active->text();
            hi += value.size() - (b - a);
          }
        }
        auto updated = active->text();
        select(active, updated, a + value.size(), a + value.size());
        status->setText(
            QString("已替换 %1 处").arg(answer["count"].toInteger()));
        lastEmpty = (a == b && value.isEmpty()) ? a : -1;
        emptyQuery = query();
        if (op == "replace")
          run("next");
      } else {
        lastEmpty = a == b ? a : -1;
        emptyQuery = query();
      }
    });
  }
};

class Folder : public QDialog {
  QMainWindow *window;
  QTabWidget *tabs;
  bool names;
  std::function<void(QString)> openFile, openLog;
  std::function<void(qint64, QString, int)> jump;
  Job job{this};
  QLineEdit *root = new QLineEdit, *query = new QLineEdit,
            *glob = new QLineEdit("*");
  QCheckBox *regex = new QCheckBox("正则"),
            *sensitive = new QCheckBox("区分大小写");
  QComboBox *codec = new QComboBox;
  QPushButton *files = new QPushButton("选择多个日志…"),
              *cancel = new QPushButton("取消搜索");
  QLabel *status = new QLabel;
  Results *results = new Results;
  QStringList selected;
  QString resultCodec;
  QTimer debounce;
  void start() {
    debounce.stop();
    job.cancel();
    cancel->setEnabled(false);
    results->reset();
    QDir dir(root->text());
    if (root->text().trimmed().isEmpty() || !dir.exists() ||
        (!names && query->text().isEmpty())) {
      status->setText(names ? "请选择文件夹。"
                            : "请选择文件夹并输入搜索内容。");
      return;
    }
    window->setProperty("projectRoot", dir.absolutePath());
    auto cache = std::make_shared<QTemporaryDir>();
    resultCodec = codec->currentText();
    QJsonObject p{{"op", names ? "names" : "folder"},
                  {"root", dir.absolutePath()},
                  {"query", query->text()},
                  {"case", sensitive->isChecked()},
                  {"regex", regex->isChecked()},
                  {"glob", glob->text().isEmpty() ? "*" : glob->text()},
                  {"codec", resultCodec},
                  {"files", QJsonArray::fromStringList(selected)}};
    cancel->setEnabled(true);
    status->setText("正在扫描…");
    job.run(cache, p, [this, cache](QJsonObject r) {
      cancel->setEnabled(false);
      status->setText(r.contains("error") ? r["error"].toString()
                                          : r["status"].toString());
      results->reset(cache, r["count"].toInteger());
    });
  }

public:
  Folder(QMainWindow *w, QTabWidget *t, bool n,
         std::function<void(QString)> opener,
         std::function<void(qint64, QString, int)> nav,
         std::function<void(QString)> openAsLog)
      : QDialog(w), window(w), tabs(t), names(n), openFile(opener),
        openLog(openAsLog), jump(nav) {
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(names ? "按文件名快速打开" : "在文件夹中查找");
    resize(760, 500);
    QString initial = w->property("projectRoot").toString();
    if (initial.isEmpty() && !filePath(t->currentWidget()).isEmpty())
      initial = QFileInfo(filePath(t->currentWidget())).absolutePath();
    root->setText(initial);
    auto *v = new QVBoxLayout(this);
    auto *body = new QHBoxLayout;
    auto *conditions = new QVBoxLayout;
    auto *output = new QVBoxLayout;
    body->setSpacing(18);
    body->addLayout(conditions, 2);
    body->addLayout(output, 3);
    v->addLayout(body, 1);
    auto field = [&](QString title, QWidget *input) {
      auto *label = new QLabel(title);
      label->setBuddy(input);
      conditions->addWidget(label);
      conditions->addWidget(input);
    };
    field("文件夹", root);
    auto *browse = new QPushButton("选择文件夹…");
    browse->setAutoDefault(false);
    auto *sources = new QHBoxLayout;
    sources->addWidget(browse);
    sources->addWidget(files);
    conditions->addLayout(sources);
    files->setVisible(!names);
    files->setAutoDefault(false);
    codec->addItems({"UTF-8", "GBK"});
    if (!names) field("文件编码", codec);
    else { codec->setParent(this); codec->hide(); }
    query->setPlaceholderText(names ? "文件名或相对路径的一部分" : "要在文件内容中查找的文字");
    field(names ? "文件名" : "查找内容", query);
    field("文件匹配", glob);
    conditions->addWidget(regex);
    conditions->addWidget(sensitive);
    conditions->addStretch();
    auto *actions = new QHBoxLayout;
    auto *go = new QPushButton("搜索");
    go->setProperty("primary", true);
    go->setDefault(true);
    cancel->setEnabled(false);
    actions->addWidget(go);
    actions->addWidget(cancel);
    conditions->addLayout(actions);
    auto *heading = new QLabel("匹配结果");
    heading->setProperty("section", true);
    output->addWidget(heading);
    output->addWidget(results, 1);
    results->reset();
    output->addWidget(status);
    query->setFocus();
    connect(root, &QLineEdit::textEdited, this, [this] {
      selected.clear();
      files->setText("选择多个日志…");
    });
    connect(browse, &QPushButton::clicked, this, [this] {
      auto dir =
          QFileDialog::getExistingDirectory(this, "选择文件夹", root->text());
      if (!dir.isEmpty()) {
        root->setText(dir);
        selected.clear();
        files->setText("选择多个日志…");
        if (names)
          start();
      }
    });
    connect(files, &QPushButton::clicked, this, [this] {
      auto paths =
          QFileDialog::getOpenFileNames(this, "选择多个日志", root->text());
      if (!paths.isEmpty()) {
        selected = paths;
        root->setText(QFileInfo(paths.first()).absolutePath());
        files->setText(QString("已选 %1 个日志").arg(paths.size()));
      }
    });
    connect(go, &QPushButton::clicked, this, [this] { start(); });
    connect(cancel, &QPushButton::clicked, this, [this] {
      job.cancel();
      cancel->setEnabled(false);
      status->setText("搜索已取消");
    });
    if (names) {
      debounce.setSingleShot(true);
      debounce.setInterval(250);
      connect(&debounce, &QTimer::timeout, this, [this] { start(); });
      connect(query, &QLineEdit::textChanged, this,
              [this] { debounce.start(); });
      if (!initial.isEmpty())
        debounce.start();
    }
    results->activate = [this](const QJsonArray &r) {
      auto path = r[5].toString();
      if (r[6].toInteger() >= 0 && openLog)
        openLog(path);
      else
        openFile(path);
      auto *current = tabs->currentWidget();
      if (!current || QFileInfo(filePath(current)) != QFileInfo(path)) {
        status->setText("无法定位到文件");
        return;
      }
      if (auto *e = qobject_cast<Editor *>(current); e && r[1].toInt() > 0) {
        int line = r[1].toInt() - 1;
        if (line < e->lines()) {
          auto text = e->text(line);
          auto base = e->SendScintilla(Editor::SCI_POSITIONFROMLINE, line);
          auto start = base + text.left(r[2].toInt()).toUtf8().size(),
               end = base + text.left(r[3].toInt()).toUtf8().size();
          e->SendScintilla(Editor::SCI_SETSEL, (unsigned long)start, (long)end);
          e->ensureLineVisible(line);
        }
      } else if (jump && r[6].toInteger() >= 0)
        jump(r[6].toInteger(), resultCodec, r[3].toInt());
      accept();
    };
    query->setFocus();
  }
};

struct DiffLine {
  int left, right;
  bool changed;
};
QStringList lines(const QString &s) {
  QStringList list;
  int start = 0;
  for (int i = 0; i < s.size(); ++i) {
    if (s[i] != '\r' && s[i] != '\n')
      continue;
    if (s[i] == '\r' && i + 1 < s.size() && s[i + 1] == '\n')
      ++i;
    list.append(s.mid(start, i - start + 1));
    start = i + 1;
  }
  if (start < s.size())
    list.append(s.mid(start));
  return list;
}
// Match common lines near the middle, then align the two remaining ranges.
void align(const QStringList &a, const QStringList &b, int l, int r, int u,
           int v, QVector<DiffLine> &out) {
  while (l < r && u < v && a[l] == b[u])
    out.append({l++, u++, false});
  int suffix = 0;
  while (l < r && u < v && a[r - 1] == b[v - 1]) {
    --r;
    --v;
    ++suffix;
  }
  int x = -1, y = -1;
  if (l < r && u < v) {
    QHash<QString, QVector<int>> index;
    for (int j = u; j < v; ++j)
      index[b[j]].append(j);
    int middle = l + (r - l) / 2;
    for (int step = 0; step < 2 * (r - l) && x < 0; ++step) {
      int i = middle + (step % 2 ? -(step + 1) / 2 : step / 2);
      if (i < l || i >= r)
        continue;
      auto it = index.constFind(a[i]);
      if (it == index.cend())
        continue;
      int target = u + qint64(i - l) * (v - u) / (r - l);
      auto near = std::lower_bound(it->cbegin(), it->cend(), target);
      if (near == it->cend() ||
          (near != it->cbegin() && target - *(near - 1) < *near - target))
        --near;
      x = i;
      y = *near;
    }
  }
  if (x >= 0) {
    align(a, b, l, x, u, y, out);
    out.append({x, y, false});
    align(a, b, x + 1, r, y + 1, v, out);
  } else
    while (l < r || u < v)
      out.append({l < r ? l++ : -1, u < v ? u++ : -1, true});
  for (int i = 0; i < suffix; ++i)
    out.append({r + i, v + i, false});
}
void compare(QMainWindow *window, QTabWidget *tabs, bool disk) {
  auto *e = qobject_cast<Editor *>(tabs->currentWidget());
  if (!e)
    return;
  auto path =
      disk ? filePath(e) : QFileDialog::getOpenFileName(window, "选择右侧文件");
  if (path.isEmpty())
    return;
  try {
    if (QFileInfo(path).size() > 2097152 || e->length() > 2097152)
      throw std::runtime_error("每侧最多支持 2 MiB。");
    auto other = Document::open(path);
    auto a = lines(e->text()), b = lines(other.text);
    if (qMax(a.size(), b.size()) > 15000)
      throw std::runtime_error("每侧最多支持 15,000 行。");
    QVector<DiffLine> diff;
    align(a, b, 0, a.size(), 0, b.size(), diff);
    QDialog dialog(window);
    dialog.setWindowTitle("文件差异");
    dialog.resize(960, 620);
    auto *v = new QVBoxLayout(&dialog);
    auto *source = new QLabel(QDir::toNativeSeparators(path) + "（" + other.codec + "）");
    source->setWordWrap(true);
    source->setTextFormat(Qt::PlainText);
    source->setTextInteractionFlags(Qt::TextSelectableByMouse);
    source->setProperty("secondary", true);
    auto *table = new QTableWidget(0, 2);
    v->addWidget(table, 1);
    v->addWidget(source);
    table->setHorizontalHeaderLabels(
        {tabs->tabText(tabs->currentIndex()), QFileInfo(path).fileName()});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    table->verticalHeader()->hide();
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    table->setWordWrap(false);
    table->setShowGrid(false);
    table->setAlternatingRowColors(true);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    auto cell = [](int idx, const QStringList &text) {
      if (idx < 0)
        return QString();
      auto s = text[idx];
      if (s.endsWith("\r\n")) {
        s.chop(2);
        s += " ␍␊";
      } else if (s.endsWith('\n')) {
        s.chop(1);
        s += " ␊";
      } else if (s.endsWith('\r')) {
        s.chop(1);
        s += " ␍";
      }
      return QString("%1    %2").arg(idx + 1).arg(s);
    };
    QVector<bool> visible(diff.size(), false);
    for (int i = 0; i < diff.size(); ++i)
      if (diff[i].changed)
        for (int j = qMax(0, i - 2); j <= qMin<int>(diff.size() - 1, i + 2);
             ++j)
          visible[j] = true;
    int previous = -1;
    for (int i = 0; i < diff.size(); ++i)
      if (visible[i]) {
        if (previous >= 0 && previous != i - 1) {
          int row = table->rowCount();
          table->insertRow(row);
          for (int c = 0; c < 2; c++)
            table->setItem(row, c, new QTableWidgetItem("…"));
        }
        previous = i;
        auto d = diff[i];
        int row = table->rowCount();
        table->insertRow(row);
        for (int c = 0; c < 2; c++) {
          auto *item =
              new QTableWidgetItem(c ? cell(d.right, b) : cell(d.left, a));
          if (d.changed && !item->text().isEmpty()) {
            item->setBackground(QColor(c ? "#dbeddf" : "#f6e1df"));
            item->setForeground(Qt::black);
          }
          table->setItem(row, c, item);
        }
      }
    if (!table->rowCount())
      v->addWidget(new QLabel("两侧文本相同。"));
    dialog.exec();
  } catch (const std::exception &error) {
    QMessageBox::warning(window, "无法对比", QString::fromUtf8(error.what()));
  }
}
} // namespace
void installSearchTools(QMainWindow *window, QMenu *menu, QTabWidget *tabs,
                        std::function<void(QString)> openFile,
                        std::function<void()> logFind,
                        std::function<void(qint64, QString, int)> jumpToByte,
                        std::function<void(QString)> openLog) {
  auto *find = new Find(window, tabs);
  QList<QAction *> editorActions;
  auto add = [&](QString name, QKeySequence shortcut, std::function<void()> fn,
                 bool editorOnly = false) {
    auto *action = menu->addAction(name);
    if (!shortcut.isEmpty())
      action->setShortcut(shortcut);
    QObject::connect(action, &QAction::triggered, window, std::move(fn));
    if (editorOnly)
      editorActions.append(action);
  };
  add("查找…", QKeySequence::Find, [=] {
    if (auto *e = qobject_cast<Editor *>(tabs->currentWidget()))
      find->openFor(e, false);
    else if (logFind)
      logFind();
  });
  add(
      "查找和替换…", QKeySequence::Replace,
      [=] {
        if (auto *e = qobject_cast<Editor *>(tabs->currentWidget()))
          find->openFor(e, true);
      },
      true);
  add("查找下一处", QKeySequence("F3"), [=] { find->run("next"); }, true);
  add("查找上一处", QKeySequence("Shift+F3"), [=] { find->run("prev"); }, true);
  add("在文件夹中查找…", QKeySequence("Ctrl+Shift+F"), [=] {
    (new Folder(window, tabs, false, openFile, jumpToByte, openLog))->show();
  });
  add("按文件名快速打开…", QKeySequence("Ctrl+P"), [=] {
    (new Folder(window, tabs, true, openFile, jumpToByte, openLog))->show();
  });
  add("与另一文件对比…", {}, [=] { compare(window, tabs, false); }, true);
  add("与磁盘版本对比…", {}, [=] { compare(window, tabs, true); }, true);
  auto update = [=] {
    find->reject();
    for (auto *a : editorActions)
      a->setEnabled(qobject_cast<Editor *>(tabs->currentWidget()) != nullptr);
  };
  QObject::connect(tabs, &QTabWidget::currentChanged, window, update);
  update();
}
