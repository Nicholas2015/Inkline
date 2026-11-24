#include "document.h"
#include <QCryptographicHash>
#include <QFileInfo>
#include <QSaveFile>
#include <QTextCodec>
#include <stdexcept>
static void fail(const QString &s) {
  throw std::runtime_error(s.toStdString());
}
static QTextCodec *converter(const QByteArray &name) {
  auto *c = QTextCodec::codecForName(name);
  if (!c)
    fail("缺少编码：" + QString::fromLatin1(name));
  return c;
}
QString decode(const QByteArray &bytes, const QByteArray &codec, bool strict) {
  QTextCodec::ConverterState s(QTextCodec::IgnoreHeader);
  auto value = converter(codec)->toUnicode(bytes.constData(), bytes.size(), &s);
  if (strict && (s.invalidChars || s.remainingChars))
    fail("文件编码无效");
  return value;
}
QByteArray encode(const QString &text, const QByteArray &codec) {
  QTextCodec::ConverterState s(QTextCodec::IgnoreHeader);
  auto data = converter(codec)->fromUnicode(text.constData(), text.size(), &s);
  if (s.invalidChars || s.remainingChars || decode(data, codec) != text)
    fail("当前编码无法完整表示文本");
  return data;
}
Document Document::open(const QString &name, const QByteArray &encoding) {
  QFile f(name);
  if (!f.open(QIODevice::ReadOnly))
    fail(f.errorString());
  auto raw = f.read(32 * 1024 * 1024 + 1);
  if (f.error() != QFile::NoError)
    fail(f.errorString());
  if (raw.size() >= 32 * 1024 * 1024)
    fail("请使用日志查看器打开大文件");
  Document d;
  d.path = QFileInfo(name).absoluteFilePath();
  d.digest = QCryptographicHash::hash(raw, QCryptographicHash::Sha256);
  if (raw.startsWith(QByteArray::fromHex("fffe0000")) ||
      raw.startsWith(QByteArray::fromHex("0000feff")))
    fail("暂不支持 UTF-32");
  for (auto pair : {std::pair{"efbbbf", "UTF-8"},
                    {"fffe", "UTF-16LE"},
                    {"feff", "UTF-16BE"}}) {
    auto mark = QByteArray::fromHex(pair.first);
    if (raw.startsWith(mark)) {
      d.bom = mark;
      d.codec = pair.second;
      break;
    }
  }
  if (!encoding.isEmpty()) {
    if (!d.bom.isEmpty() && d.codec != encoding)
      fail("所选编码与文件 BOM 不一致");
    d.codec = encoding;
  }
  raw.remove(0, d.bom.size());
  if (d.codec.startsWith("UTF-16") && raw.size() % 2)
    fail("UTF-16 文件不完整");
  try {
    d.text = decode(raw, d.codec);
  } catch (const std::exception &) {
    if (!d.bom.isEmpty() || !encoding.isEmpty())
      throw;
    d.codec = "GBK";
    try {
      d.text = decode(raw, d.codec);
    } catch (const std::exception &e) {
      throw EncodingError(e.what());
    }
  }
  if (d.text.contains(QChar::Null) || encode(d.text, d.codec) != raw)
    throw EncodingError("无法无损读取文件");
  return d;
}
void Document::save(const QString &name, const QString &value,
                    const QByteArray &approvedRevision) {
  const auto dest = QFileInfo(name).absoluteFilePath();
  if (dest == path) {
    QFile old(path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray revision = "deleted";
    if (old.exists()) {
      if (old.size() >= 32 * 1024 * 1024)
        fail("磁盘文件已增长为大文件，请另存当前修改");
      if (!old.open(QIODevice::ReadOnly) || !hash.addData(&old))
        fail(old.errorString());
      revision = hash.result();
    }
    if (revision != (approvedRevision.isEmpty() ? digest : approvedRevision))
      throw DiskConflict(revision);
  }
  auto bytes = bom + encode(value, codec);
  QSaveFile out(dest);
  if (!out.open(QIODevice::WriteOnly) || out.write(bytes) != bytes.size() ||
      !out.commit())
    fail(out.errorString());
  digest = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
  path = dest;
  text = value;
}
