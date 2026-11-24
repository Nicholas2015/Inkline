#include "document.h"
#include "logscan.h"
#include "searchtools.h"
#include "searchtask.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <stdexcept>

namespace {
void fail(const QString &s) { throw std::runtime_error(s.toStdString()); }
QByteArray read(const QString &path) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
    fail(f.errorString());
  auto data = f.readAll();
  if (f.error() != QFileDevice::NoError)
    fail(f.errorString());
  return data;
}
void write(QFile &f, const QByteArray &bytes) {
  if (f.write(bytes) != bytes.size())
    fail(f.errorString());
}
struct Pages {
  QDir dir;
  QFile file;
  qint64 count = 0;
  explicit Pages(const QDir &d) : dir(d) {}
  void add(const QJsonArray &row) {
    if (count % SearchPageSize == 0) {
      file.close();
      file.setFileName(dir.filePath(QString::number(count / SearchPageSize) + ".jsonl"));
      if (!file.open(QIODevice::WriteOnly))
        fail(file.errorString());
    }
    write(file, QJsonDocument(row).toJson(QJsonDocument::Compact) + '\n');
    ++count;
  }
};
QString expand(const QString &replacement,
               const QRegularExpressionMatch &match) {
  QString result;
  for (qsizetype i = 0; i < replacement.size(); ++i) {
    QChar c = replacement[i];
    if (c != '\\') {
      result += c;
      continue;
    }
    if (++i == replacement.size())
      fail("替换文本末尾缺少转义字符");
    c = replacement[i];
    QString group;
    if (c == 'g' && replacement.mid(i + 1, 1) == "<") {
      auto end = replacement.indexOf('>', i + 2);
      if (end < 0)
        fail("捕获组名称缺少 >");
      group = replacement.mid(i + 2, end - i - 2);
      i = end;
      if (group.isEmpty())
        fail("捕获组不能为空");
    } else if (c.isDigit()) {
      group = c;
      while (i + 1 < replacement.size() && replacement[i + 1].isDigit())
        group += replacement[++i];
    } else if (c == 'n')
      result += '\n';
    else if (c == 'r')
      result += '\r';
    else if (c == 't')
      result += '\t';
    else if (c == '\\')
      result += '\\';
    else if (c.isLetter())
      fail("无法识别替换文本中的转义字符");
    else {
      result += '\\';
      result += c;
    }
    if (!group.isEmpty()) {
      bool numeric;
      int index = group.toInt(&numeric);
      if (numeric && index >= 0 &&
          index <= match.regularExpression().captureCount())
        result += match.captured(index);
      else if (!numeric &&
               match.regularExpression().namedCaptureGroups().contains(group))
        result += match.captured(group);
      else
        fail("替换文本引用了不存在的捕获组：" + group);
    }
  }
  return result;
}
QJsonObject documents(const QJsonObject &p, const QRegularExpression &pattern,
                      Pages &pages) {
  const auto op = p["op"].toString();
  const auto replacement = p["replacement"].toString();
  qint64 count = 0;
  for (const auto &item : p["docs"].toArray()) {
    auto d = item.toObject();
    auto text =
        QString::fromUtf8(read(pages.dir.filePath(d["snapshot"].toString())));
    int lo = d["lo"].toInt(), hi = d["hi"].toInt();
    QString subject = text.mid(lo, hi - lo);
    int left = p["left"].toInt(), right = p["right"].toInt(),
        empty = p["empty"].toInt(-1);
    QRegularExpressionMatch chosen, first, last;
    auto matches = pattern.globalMatch(subject);
    QString changed;
    int consumed = 0;
    int cursor = 0, line = 1;
    while (matches.hasNext()) {
      auto m = matches.next();
      int a = int(m.capturedStart()) + lo, b = int(m.capturedEnd()) + lo;
      ++count;
      if (op == "list") {
        while (cursor < a) {
          if (text[cursor] == '\r') {
            ++line;
            if (cursor + 1 < a && text[cursor + 1] == '\n')
              ++cursor;
          } else if (text[cursor] == '\n')
            ++line;
          ++cursor;
        }
        pages.add(
            {d["label"], line, a, b,
             text.mid(a, 100).section(QRegularExpression("[\\r\\n]"), 0, 0),
             d["id"]});
      } else if (op == "all") {
        changed += subject.mid(consumed, m.capturedStart() - consumed);
        changed += p["regex"].toBool() ? expand(replacement, m) : replacement;
        consumed = m.capturedEnd();
      } else if (op == "replace" && a == left && b == right) {
        return {{"edit", true},
                {"a", a},
                {"b", b},
                {"value",
                 p["regex"].toBool() ? expand(replacement, m) : replacement},
                {"count", 1}};
      } else if (op != "count") {
        if (a == b && a == empty && left == right && left == a)
          continue;
        if (!first.hasMatch())
          first = m;
        last = m;
        if (op == "prev") {
          if (b <= left)
            chosen = m;
        } else if (a >= right && !chosen.hasMatch()) {
          chosen = m;
          if (op == "next")
            break;
        }
      }
    }
    if (!matches.isValid())
      fail("正则表达式超出匹配限制，请简化表达式");
    if (op == "all") {
      changed += subject.mid(consumed);
      return {{"edit", true},
              {"a", lo},
              {"b", hi},
              {"value", changed},
              {"count", count}};
    }
    if (op != "count" && op != "list") {
      if (!chosen.hasMatch())
        chosen = op == "prev" ? last : first;
      if (chosen.hasMatch())
        return {{"a", chosen.capturedStart() + lo},
                {"b", chosen.capturedEnd() + lo}};
      return {{"status", "没有匹配"}};
    }
  }
  return {{"count", count}, {"status", QString("共 %1 处匹配").arg(count)}};
}

QJsonObject folder(const QJsonObject &p, const QRegularExpression &pattern,
                   Pages &pages) {
  QDir root(p["root"].toString());
  const bool names = p["op"] == "names";
  const bool replacing = p["op"] == "replace-preview";
  QJsonArray preview;
  qsizetype previewChars = 0;
  int previewCount = 0;
  QStringList dirs{root.absolutePath()};
  auto selected = p["files"].toArray();
  if (!selected.isEmpty())
    dirs.clear();
  const auto glob = QRegularExpression::fromWildcard(p["glob"].toString("*"),
                                                     Qt::CaseInsensitive);
  int skipped = 0;
  auto scan = [&](const QFileInfo &info) {
    const auto path = info.absoluteFilePath();
    const auto rel = root.relativeFilePath(path);
    if (info.isSymLink() || !info.isFile())
      return;
    if (names) {
      if (pattern.match(rel).hasMatch())
        pages.add({rel, 0, 0, 0, "", path, -1});
      return;
    }
    if (replacing) {
      Document doc;
      try {
        doc = Document::open(path);
      } catch (const std::exception &) {
        ++skipped;
        return;
      }
      QJsonArray rows;
      int line = 1;
      qsizetype cursor = 0;
      auto matches = pattern.globalMatch(doc.text);
      while (matches.hasNext()) {
        const auto m = matches.next();
        const auto at = m.capturedStart();
        const auto before = m.captured();
        const auto after = p["regex"].toBool()
            ? expand(p["replacement"].toString(), m) : p["replacement"].toString();
        if (before == after)
          continue;
        previewChars += before.size() + after.size();
        if (++previewCount > 5000 || previewChars > 4 * 1024 * 1024)
          fail("预览过大，请缩小文件夹、文件匹配或查找范围。");
        while (cursor < at) {
          const auto c = doc.text[cursor++];
          if (c == '\r' || (c == '\n' && (cursor < 2 || doc.text[cursor - 2] != '\r')))
            ++line;
        }
        auto left = at, right = m.capturedEnd();
        while (left > 0 && at - left < 35 && doc.text[left - 1] != '\r' && doc.text[left - 1] != '\n')
          --left;
        while (right < doc.text.size() && right - m.capturedEnd() < 55 &&
               doc.text[right] != '\r' && doc.text[right] != '\n')
          ++right;
        rows.append(QJsonObject{{"start", at}, {"line", line},
                                {"prefix", doc.text.mid(left, at - left)},
                                {"suffix", doc.text.mid(m.capturedEnd(), right - m.capturedEnd())},
                                {"before", before}, {"after", after}});
      }
      if (!matches.isValid())
        fail("正则表达式超出匹配限制，请简化表达式");
      if (!rows.isEmpty())
        preview.append(QJsonObject{{"path", path}, {"relative", rel},
            {"canonical", info.canonicalFilePath()}, {"digest", QString::fromLatin1(doc.digest.toHex())},
            {"codec", QString::fromLatin1(doc.codec)}, {"matches", rows}});
      return;
    }
    if (info.size() >= 32 * 1024 * 1024 || !selected.isEmpty()) {
      int line = 0;
      try {
        visitLogRecords(
            path, info.size(), 0, p["codec"].toString("UTF-8").toLatin1(),
            p["query"].toString(), p["regex"].toBool(), p["case"].toBool(),
            [&](qint64, qint64, qint64 offset, qsizetype length,
                const QString &preview) {
              ++line;
              if (offset >= 0)
                pages.add({rel, line, 0, length, preview, path, offset});
              return true;
            });
      } catch (const std::exception &) {
        ++skipped;
      }
      return;
    }
    Document doc;
    try {
      doc = Document::open(path);
    } catch (const std::exception &) {
      ++skipped;
      return;
    }
    qsizetype start = 0;
    int line = 0;
    while (start < doc.text.size()) {
      auto end = start;
      while (end < doc.text.size() && doc.text[end] != '\r' &&
             doc.text[end] != '\n')
        ++end;
      auto value = QStringView(doc.text).mid(start, end - start);
      auto m = pattern.matchView(value);
      ++line;
      if (m.hasMatch())
        pages.add({rel, line, m.capturedStart(), m.capturedEnd(),
                   value.mid(qMax<qsizetype>(0, m.capturedStart() - 42), 150)
                       .toString(),
                   path, -1});
      start = end + 1;
      if (end < doc.text.size() && doc.text[end] == '\r' &&
          start < doc.text.size() && doc.text[start] == '\n')
        ++start;
    }
  };
  for (const auto &f : selected)
    scan(QFileInfo(f.toString()));
  while (!dirs.isEmpty() && (!names || pages.count < 2000)) {
    QDir d(dirs.takeLast());
    const auto entries =
        d.entryInfoList(QDir::NoDotAndDotDot | QDir::Dirs | QDir::Files |
                            QDir::Hidden | QDir::NoSymLinks,
                        QDir::Name);
    for (const auto &info : entries) {
      if (info.isDir()) {
        static const QStringList exclude = {".git", ".venv", "node_modules",
                                            "__pycache__"};
        if (!info.isJunction() && !exclude.contains(info.fileName()) &&
            info.absoluteFilePath() != pages.dir.absolutePath())
          dirs.append(info.absoluteFilePath());
      } else if (glob.match(info.fileName()).hasMatch()) {
        scan(info);
        if (names && pages.count >= 2000)
          break;
      }
    }
  }
  if (replacing)
    return {{"preview", preview}, {"count", previewCount}, {"skipped", skipped}};
  return {{"count", pages.count},
          {"status", names && pages.count >= 2000
                         ? QString("已显示前 2000 项，请缩小范围。")
                         : QString("找到 %1 项，跳过 %2 个文件。")
                               .arg(pages.count)
                               .arg(skipped)}};
}
} // namespace
int runSearchWorker(const QString &requestFile) {
  QJsonObject result;
  QDir dir = QFileInfo(requestFile).absoluteDir();
  try {
    const auto p = QJsonDocument::fromJson(read(requestFile)).object();
    QString expression = p["query"].toString();
    if (!p["regex"].toBool())
      expression = QRegularExpression::escape(expression);
    if (p["whole"].toBool())
      expression = "(?<!\\w)(?:" + expression + ")(?!\\w)";
    auto options = QRegularExpression::PatternOptions(
        QRegularExpression::UseUnicodePropertiesOption |
        QRegularExpression::MultilineOption);
    if (!p["case"].toBool())
      options |= QRegularExpression::CaseInsensitiveOption;
    QRegularExpression pattern(expression, options);
    if (!pattern.isValid())
      fail(pattern.errorString());
    Pages pages(dir);
    result = p.contains("docs") ? documents(p, pattern, pages)
                                : folder(p, pattern, pages);
    if (pages.file.isOpen() && !pages.file.flush())
      fail(pages.file.errorString());
  } catch (const std::exception &e) {
    result = {{"error", QString::fromUtf8(e.what())}};
  }
  QFile out(dir.filePath("reply.json"));
  if (!out.open(QIODevice::WriteOnly))
    return 1;
  auto bytes = QJsonDocument(result).toJson(QJsonDocument::Compact);
  return out.write(bytes) == bytes.size() && out.flush() ? 0 : 1;
}
