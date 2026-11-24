#include "logfile.h"
#include "document.h"
#include <QFile>
#include <stdexcept>
static int unit(const QByteArray &data, qsizetype pos, bool gbk) {
  auto c = static_cast<unsigned char>(data[pos]);
  if (gbk) {
    if (c < 0x81 || c > 0xfe)
      return 1;
    if (pos + 1 == data.size())
      return 2;
    auto t = static_cast<unsigned char>(data[pos + 1]);
    return t >= 0x40 && t <= 0xfe && t != 0x7f ? 2 : 1;
  }
  return c < 0xc2 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : c <= 0xf4 ? 4 : 1;
}
QByteArray readPage(const QString &path, qint64 offset,
                    const QByteArray &codec) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly) || !f.seek(offset))
    throw std::runtime_error(f.errorString().toStdString());
  auto data = f.read(128 * 1024);
  if (f.error() != QFile::NoError)
    throw std::runtime_error(f.errorString().toStdString());
  if (!f.atEnd()) {
    for (qsizetype i = 0; i < data.size();) {
      auto width = unit(data, i, codec == "GBK");
      if (i + width > data.size()) {
        data.truncate(i);
        break;
      }
      i += width;
    }
    if (data.endsWith('\r') && f.peek(1) == "\n")
      data.chop(1);
  }
  return data;
}
Scan scanLog(const QString &path, const QString &query, const QByteArray &codec,
             const std::atomic_bool &stop) {
  Scan out;
  try {
    const auto needle = encode(query, codec);
    if (needle.isEmpty())
      throw std::runtime_error("请输入搜索文字");
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
      throw std::runtime_error(f.errorString().toStdString());
    const auto size = f.size();
    qint64 base = 0;
    QByteArray buf;
    while (base < size && !stop.load()) {
      auto amount = qMin<qint64>(256 * 1024, size - f.pos());
      auto chunk = f.read(amount);
      if (chunk.size() != amount)
        throw std::runtime_error("日志读取失败或已被截断");
      buf += chunk;
      qsizetype i = 0, last = buf.size() - needle.size();
      while (i <= last && !stop.load()) {
        if (codec == "GBK") {
          if (QByteArrayView(buf).sliced(i, needle.size()) != needle) {
            auto width = unit(buf, i, true);
            if (i + width > buf.size())
              break;
            i += width;
            continue;
          }
        } else {
          i = buf.indexOf(needle, i);
          if (i < 0) {
            i = last + 1;
            break;
          }
        }
        if (out.offsets.size() < 200)
          out.offsets.append(base + i);
        ++out.count;
        i += needle.size();
      }
      if (f.pos() == size)
        break;
      base += i;
      buf.remove(0, i);
    }
    out.cancelled = stop.load();
  } catch (const std::exception &e) {
    out.error = QString::fromUtf8(e.what());
  }
  return out;
}
