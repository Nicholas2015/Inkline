#include "logscan.h"
#include "logfields.h"
#include "document.h"
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQueue>
#include <QRegularExpression>
#include <cstdio>
#include <stdexcept>
#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

namespace {
constexpr qint64 chunkSize = 128 * 1024;
void fail(const QString &s) { throw std::runtime_error(s.toStdString()); }
void openLog(QFile &f) {
#ifdef Q_OS_WIN
  auto handle =
      CreateFileW(reinterpret_cast<LPCWSTR>(f.fileName().utf16()), GENERIC_READ,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE)
    fail("无法打开日志：" + f.fileName());
  int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle),
                           _O_RDONLY | _O_BINARY);
  if (fd < 0) {
    CloseHandle(handle);
    fail("无法读取日志");
  }
  if (!f.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
    _close(fd);
    fail(f.errorString());
  }
#else
  if (!f.open(QIODevice::ReadOnly))
    fail(f.errorString());
#endif
}
QByteArray readAt(QFile &f, qint64 pos, qint64 len) {
  if (!f.seek(pos))
    fail(f.errorString());
  auto bytes = f.read(len);
  if (bytes.size() != len)
    fail("日志在扫描期间发生变化");
  return bytes;
}
// Return zero for an incomplete trailing character, one for malformed input.
int width(QByteArrayView bytes, qsizetype i, bool gbk, bool final) {
  auto c = uchar(bytes[i]);
  int n = gbk ? ((c >= 0x81 && c <= 0xfe) ? 2 : 1)
              : (c < 0xc2 || c > 0xf4 ? 1
                 : c < 0xe0           ? 2
                 : c < 0xf0           ? 3
                                      : 4);
  if (bytes.size() - i < n)
    return final ? 1 : 0;
  for (int j = 1; j < n; ++j) {
    auto t = uchar(bytes[i + j]);
    if (gbk ? (t < 0x40 || t == 0x7f || t > 0xfe) : ((t & 0xc0) != 0x80))
      return 1;
  }
  return n;
}
qsizetype byteOffset(const QString &text, qsizetype chars,
                     const QByteArray &raw, const QByteArray &codec) {
  if (!text.left(chars).contains(QChar::ReplacementCharacter))
    return encode(text.left(chars), codec).size();
  qsizetype i = 0, count = 0;
  while (count < chars && i < raw.size()) {
    int w = width(raw, i, codec == "GBK", true);
    count += decode(raw.mid(i, w), codec, false).size();
    i += w;
  }
  return i;
}
void copyRange(QFile &input, QFile &output, qint64 begin, qint64 end) {
  while (begin < end) {
    auto data = readAt(input, begin, qMin(chunkSize, end - begin));
    if (output.write(data) != data.size())
      fail(output.errorString());
    begin += data.size();
  }
}
qint64 lineBeginning(QFile &f, qint64 offset) {
  while (offset) {
    auto begin = qMax<qint64>(0, offset - chunkSize);
    auto data = readAt(f, begin, offset - begin);
    auto nl = data.lastIndexOf('\n');
    if (nl >= 0)
      return begin + nl + 1;
    offset = begin;
  }
  return 0;
}
qint64 lineEnd(QFile &f, qint64 pos, qint64 size) {
  while (pos < size) {
    auto data = readAt(f, pos, qMin(chunkSize, size - pos));
    auto nl = data.indexOf('\n');
    if (nl >= 0)
      return pos + nl + 1;
    pos += data.size();
  }
  return size;
}
} // namespace

qint64 logAlignedStart(const QString &path, qint64 offset,
                       const QByteArray &codec) {
  if (!offset)
    return 0;
  QFile f(path);
  openLog(f);
  offset = qBound<qint64>(0, offset, f.size());
  auto begin = qMax<qint64>(0, offset - 4096);
  auto prefix = readAt(f, begin, offset - begin);
  // Do not treat the position between CR and LF as the following record.
  if (prefix.endsWith('\r') && offset < f.size() && f.peek(1) == "\n")
    prefix.chop(1);
  auto nl = qMax(prefix.lastIndexOf('\n'), prefix.lastIndexOf('\r'));
  if (nl >= 0)
    return begin + nl + 1;
  if (!begin)
    return 0;
  if (codec == "GBK") {
    for (qsizetype i = prefix.size(); i > 0; --i) {
      auto c = uchar(prefix[i - 1]);
      if (c < 0x40 || c == 0x7f || c == 0xff)
        return begin + i;
    }
  } else {
    auto suffix = readAt(f, offset, qMin<qint64>(3, f.size() - offset));
    for (char ch : suffix) {
      if ((uchar(ch) & 0xc0) != 0x80)
        break;
      ++offset;
    }
  }
  return offset;
}

void visitLogRecords(const QString &path, qint64 size, qint64 start,
                     const QByteArray &codec, const QString &query, bool regex,
                     bool caseSensitive, const RecordVisitor &visit) {
  if (query.isEmpty())
    fail("请输入搜索条件");
  QRegularExpression pattern(regex ? query : QRegularExpression::escape(query),
                             caseSensitive
                                 ? QRegularExpression::NoPatternOption
                                 : QRegularExpression::CaseInsensitiveOption);
  if (!pattern.isValid())
    fail(pattern.errorString());
  QFile f(path);
  openLog(f);
  if (!f.seek(start))
    fail(f.errorString());
  while (f.pos() < size) {
    auto begin = f.pos(), base = begin;
    qint64 hit = -1;
    qsizetype length = 0;
    QString preview;
    QByteArray buffer;
    do {
      auto part = f.readLine(qMin(chunkSize, size - f.pos()) + 1);
      if (part.isEmpty())
        fail("日志在扫描期间被截断");
      bool final = part.endsWith('\n') || f.pos() == size;
      if (hit < 0) {
        buffer += part;
        if (regex && !final)
          continue;
        if (final && buffer.endsWith('\n'))
          buffer.chop(1);
        if (final && buffer.endsWith('\r'))
          buffer.chop(1);
        qsizetype usable = 0;
        while (usable < buffer.size()) {
          auto w = width(buffer, usable, codec == "GBK", final);
          if (!w)
            break;
          usable += w;
        }
        const auto text = decode(buffer.left(usable), codec, false);
        auto m = pattern.match(text);
        if (m.hasMatch()) {
          hit = base + byteOffset(text, m.capturedStart(), buffer, codec);
          length = m.capturedLength();
          preview = text.mid(qMax<qsizetype>(0, m.capturedStart() - 60), 220);
          buffer.clear();
        } else if (!final) {
          qsizetype keep = qMax<qsizetype>(0, text.size() - query.size() - 1);
          if (keep && text[keep].isLowSurrogate())
            --keep;
          auto cut = byteOffset(text, keep, buffer, codec);
          buffer.remove(0, cut);
          base += cut;
        }
      }
      if (final)
        break;
    } while (f.pos() < size);
    if (!visit(begin, f.pos(), hit, length, preview))
      return;
  }
}

namespace {
// Visit byte offsets immediately after complete file line endings.
template <class Visitor>
void visitLineStarts(QFile &f, qint64 pos, qint64 size, Visitor visit) {
  while (pos < size) {
    auto data = readAt(f, pos, qMin(chunkSize, size - pos));
    if (data.endsWith('\r') && pos + data.size() < size && f.peek(1) == "\n")
      data += f.read(1);
    for (qsizetype i = 0; i < data.size(); ++i) {
      if (data[i] == '\r') {
        if (i + 1 < data.size() && data[i + 1] == '\n')
          ++i;
      } else if (data[i] != '\n')
        continue;
      if (!visit(pos + i + 1))
        return;
    }
    pos += data.size();
  }
}
QJsonObject execute(const QJsonObject &j) {
  const auto path = j["path"].toString(), kind = j["kind"].toString(),
             query = j["query"].toString();
  const bool session = j["session"].toBool();
  const auto pattern = session ? sessionPattern(QRegularExpression::escape(query)) : query;
  const auto codec = j["codec"].toString().toLatin1();
  auto start = j["start"].toInteger(), size = j["size"].toInteger();
  QFile f(path);
  openLog(f);
  if (f.size() < size)
    fail("日志已被截断");
  QJsonObject result;
  if (kind == "line") {
    const qint64 line = j["line"].toInteger();
    if (line < 1)
      fail("行号应大于零");
    qint64 pos = 0, remaining = 0;
    if (j.contains("anchor")) {
      pos = j["anchor"].toInteger();
      remaining = (line - 1) % 1024;
    } else {
      qint64 lines = 1;
      QJsonArray anchors{0};
      visitLineStarts(f, 0, size, [&](qint64 offset) {
        ++lines;
        if ((lines - 1) % 1024 == 0)
          anchors.append(offset);
        return true;
      });
      if (line > lines)
        fail(QString("请输入 1 至 %1 的行号").arg(lines));
      result["anchors"] = anchors;
      result["lines"] = lines;
      pos = anchors[(line - 1) / 1024].toInteger();
      remaining = (line - 1) % 1024;
    }
    if (remaining)
      visitLineStarts(f, pos, size, [&](qint64 offset) {
        pos = offset;
        return --remaining > 0;
      });
    if (remaining)
      fail("日志已变化，请重新定位");
    result["offset"] = pos;
    return result;
  }
  const int first = j["fromSecond"].toInt(-1), last = j["toSecond"].toInt(-1);
  auto inTimeRange = [&](qint64 begin) {
    if (first < 0)
      return true;
    static const QRegularExpression clock(
        QStringLiteral(R"(^\s*\[?(?:\d{4}[-/]\d{2}[-/]\d{2}[ T])?(\d{2}:\d{2}:\d{2})(?!\d))"));
    auto prefix = readAt(f, begin, qMin<qint64>(64, size - begin));
    auto match = clock.match(QString::fromUtf8(prefix));
    auto time = QTime::fromString(match.captured(1), "HH:mm:ss");
    int sec = time.msecsSinceStartOfDay() / 1000;
    return time.isValid() && (first <= last ? sec >= first && sec <= last
                                            : sec >= first || sec <= last);
  };
  QJsonArray hits;
  qint64 more = -1, resume = size;
  if (kind == "marks") {
    QJsonArray marks;
    for (auto value : j["hits"].toArray()) {
      auto begin = lineBeginning(f, value.toInteger());
      auto prefix = readAt(f, begin, qMin<qint64>(40, size - begin));
      marks.append(QJsonArray{begin, QString::fromLatin1(prefix.toBase64())});
    }
    result["marks"] = marks;
    return result;
  }
  if (kind == "batch") {
    QFile out(j["output"].toString());
    if (!out.open(QIODevice::WriteOnly))
      fail(out.errorString());
    qint64 previous = -1;
    for (auto h : j["hits"].toArray()) {
      auto offset = h.toInteger();
      if (offset < previous)
        continue;
      auto begin = lineBeginning(f, offset);
      previous = lineEnd(f, offset, size);
      copyRange(f, out, begin, previous);
    }
    if (!out.flush())
      fail(out.errorString());
    return result;
  }
  if (kind == "all") {
    QFile out(j["output"].toString());
    if (!out.open(QIODevice::WriteOnly))
      fail(out.errorString());
    int context = j["context"].toInt(), after = 0;
    QQueue<qint64> previous;
    qint64 written = 0, count = 0;
    visitLogRecords(
        path, size, 0, codec, pattern, session || j["regex"].toBool(), j["case"].toBool(),
        [&](qint64 begin, qint64 end, qint64 hit, qsizetype, const QString &) {
          if (hit >= 0 && !inTimeRange(begin))
            hit = -1;
          if (hit >= 0 || after > 0) {
            auto first =
                (hit >= 0 && !previous.isEmpty()) ? previous.head() : begin;
            copyRange(f, out, qMax(first, written), end);
            written = end;
            if (hit >= 0) {
              ++count;
              after = context;
            } else
              --after;
          }
          previous.enqueue(begin);
          if (previous.size() > context)
            previous.dequeue();
          return true;
        });
    if (!out.flush())
      fail(out.errorString());
    result["count"] = count;
    return result;
  }
  int limit = qBound(1, j["limit"].toInt(200), 200);
  if (j["filter"].toBool()) {
    visitLogRecords(path, size, start, codec, pattern, session || j["regex"].toBool(),
                    j["case"].toBool(),
                    [&](qint64 begin, qint64 end, qint64 hit, qsizetype len,
                        const QString &preview) {
                      if (end == size && readAt(f, size - 1, 1) != "\n")
                        resume = begin;
                      if (hit >= 0 && inTimeRange(begin))
                        hits.append(QJsonArray{hit, qint64(len), preview});
                      if (hits.size() < limit)
                        return true;
                      if (end < size)
                        more = end;
                      return false;
                    });
  } else {
    auto needle = encode(query, codec);
    if (needle.isEmpty())
      fail("请输入搜索条件");
    qint64 pos = start, base = start;
    QByteArray buffer;
    while (pos < size && hits.size() < limit) {
      auto part = readAt(f, pos, qMin(chunkSize, size - pos));
      pos += part.size();
      buffer += part;
      qsizetype i = 0, last = buffer.size() - needle.size();
      while (i <= last) {
        if (codec != "GBK") {
          i = buffer.indexOf(needle, i);
          if (i < 0) {
            i = last + 1;
            break;
          }
        } else if (QByteArrayView(buffer).sliced(i, needle.size()) != needle) {
          auto w = width(buffer, i, true, pos == size);
          if (!w)
            break;
          i += w;
          continue;
        }
        hits.append(QJsonArray{base + i, qint64(query.size()), query});
        i += needle.size();
        if (hits.size() == limit) {
          if (base + i < size)
            more = base + i;
          break;
        }
      }
      buffer.remove(0, i);
      base += i;
    }
  }
  result["hits"] = hits;
  result["more"] = more;
  result["resume"] = resume;
  return result;
}
} // namespace
int runLogJob() {
  QFile input, output;
  if (!input.open(stdin, QIODevice::ReadOnly) ||
      !output.open(stdout, QIODevice::WriteOnly))
    return 2;
  QJsonObject result;
  try {
    result = execute(QJsonDocument::fromJson(input.readAll()).object());
  } catch (const std::exception &e) {
    result["error"] = QString::fromUtf8(e.what());
  }
  output.write(QJsonDocument(result).toJson(QJsonDocument::Compact));
  return output.flush() ? 0 : 2;
}
