#pragma once
#include <QtCore>

namespace pyindex {
struct Use {
  QString name, owner, target;
  int row, column;
  bool defines = false;
};
struct Scope {
  QString name;
  int indent;
};
struct Binding {
  QString module, symbol;
};
struct File {
  QString path;
  QList<Use> uses;
  QHash<QString, Binding> imports;
  QHash<QString, QString> signatures;
  QSet<QString> declarations;
  QStringList scopes;
};
inline QString key(QString owner, QString name) { return owner + "/" + name; }
inline QString parent(QString owner) { return owner.section('/', 0, -2); }
inline QString findDecl(const File &file, QString scope, QString name) {
  while (!scope.isEmpty() && !file.declarations.contains(key(scope, name)))
    scope = parent(scope);
  return key(scope, name);
}
inline File scan(QString path, const QString &source) {
  static const QRegularExpression word(R"([\p{L}_][\p{L}\p{N}_]*)");
  static const QRegularExpression declaration(
      R"(^\s*(?:async\s+)?(def|class)\s+([\p{L}_][\p{L}\p{N}_]*)\s*(\([^\r\n]*?\))?)");
  static const QRegularExpression param(
      R"((?:^|,)\s*[*]{0,2}([\p{L}_][\p{L}\p{N}_]*))");
  static const QRegularExpression import(
      R"(^\s*(?:from\s+([\w.]+)\s+import\s+|import\s+)([^#;\r\n]+))",
      QRegularExpression::UseUnicodePropertiesOption);
  static const QRegularExpression binding(
      R"(([\w.]+)(?:\s+as\s+(\w+))?)",
      QRegularExpression::UseUnicodePropertiesOption);
  static const QRegularExpression ignored(
      R"py(#.*$|"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*')py");
  File result;
  result.path = path;
  QList<Scope> stack;
  QStringList lines = source.split(QRegularExpression("\r\n|\n|\r"));
  for (int row = 0; row < lines.size(); ++row) {
    QString text = lines[row];
    auto ignore = ignored.globalMatch(text);
    while (ignore.hasNext()) {
      auto h = ignore.next();
      text.replace(h.capturedStart(), h.capturedLength(),
                   QString(h.capturedLength(), ' '));
    }
    int spaces = 0, indent = 0;
    while (spaces < text.size() && text[spaces].isSpace()) {
      indent += text[spaces++] == '\t' ? 8 - indent % 8 : 1;
    }
    if (!text.trimmed().isEmpty())
      while (!stack.isEmpty() && stack.last().indent >= indent)
        stack.removeLast();
    QString owner = stack.isEmpty() ? QString{} : stack.last().name;
    result.scopes << owner;
    auto decl = declaration.match(text);
    auto imp = import.match(text);
    QString nested;
    QSet<int> params;
    QHash<int, QString> importNames;
    if (decl.hasMatch()) {
      auto name = decl.captured(2);
      nested = owner + "/" + QString::number(row);
      result.declarations.insert(key(owner, name));
      if (decl.captured(1) == "def" && !decl.captured(3).isEmpty()) {
        result.signatures[key(owner, name)] = name + decl.captured(3);
        auto p = param.globalMatch(decl.captured(3).mid(1));
        while (p.hasNext()) {
          auto h = p.next();
          params.insert(decl.capturedStart(3) + 1 + h.capturedStart(1));
        }
      }
    }
    if (imp.hasMatch()) {
      auto matches = binding.globalMatch(imp.captured(2));
      while (matches.hasNext()) {
        auto h = matches.next();
        QString sourceName = h.captured(1), alias = h.captured(2);
        QString local = alias.isEmpty() ? sourceName.section('.', 0, 0) : alias;
        QString module = imp.captured(1).isEmpty()
                             ? (alias.isEmpty() ? local : sourceName)
                             : imp.captured(1);
        QString imported = imp.captured(1).isEmpty() ? QString{} : sourceName;
        result.imports[key(owner, local)] = {module, imported};
        result.declarations.insert(key(owner, local));
        importNames[imp.capturedStart(2) + h.capturedStart(1)] = local;
        if (!alias.isEmpty())
          importNames[imp.capturedStart(2) + h.capturedStart(2)] = local;
      }
    }
    auto matches = word.globalMatch(text);
    while (matches.hasNext()) {
      auto h = matches.next();
      int col = h.capturedStart();
      QString name = h.captured();
      if (imp.hasMatch() && !importNames.contains(col))
        continue;
      QString scope = owner;
      bool defined = (decl.hasMatch() && col == decl.capturedStart(2));
      if (params.contains(col)) {
        scope = nested;
        defined = true;
      }
      if (decl.hasMatch() && col > decl.capturedEnd(3) &&
          !decl.captured(3).isEmpty())
        scope = nested;
      auto tail = text.mid(h.capturedEnd()).trimmed();
      if ((tail.startsWith('=') && !tail.startsWith("==")) &&
          (col == 0 || text[col - 1] != '.'))
        defined = true;
      if (defined)
        result.declarations.insert(key(scope, name));
      QString access = importNames.value(col, name);
      int start = col;
      while (start > 0 && text[start - 1] == '.') {
        int end = start - 1, begin = end;
        while (begin > 0 &&
               (text[begin - 1].isLetterOrNumber() || text[begin - 1] == '_'))
          --begin;
        if (begin == end)
          break;
        access = text.mid(begin, end - begin) + "." + access;
        start = begin;
      }
      result.uses << Use{name, scope, access, row, col, defined};
    }
    if (!nested.isEmpty())
      stack << Scope{nested, indent};
  }
  return result;
}
class Project {
  QString root;
  QMap<QString, File> files;
  QMap<QString, QString> buffers;
  QString normalize(QString path) const {
    QFileInfo info(path);
    return QDir::cleanPath(info.exists() ? info.canonicalFilePath()
                                         : info.absoluteFilePath());
  }
  QString resolve(QString path, QString module) const {
    QStringList bases{root, root + "/src", QFileInfo(path).absolutePath()};
    if (module.startsWith('.')) {
      QDir base(QFileInfo(path).absolutePath());
      int count = 0;
      while (count < module.size() && module[count] == '.')
        ++count;
      for (int i = 1; i < count; ++i)
        base.cdUp();
      bases = QStringList{base.path()};
      module = module.mid(count);
    }
    module.replace('.', '/');
    for (auto base : bases)
      for (auto ext : {QString(".py"), QString("/__init__.py")}) {
        auto dest = normalize(base + "/" + module + ext);
        if (dest.startsWith(root + "/") &&
            (files.contains(dest) || buffers.contains(dest) ||
             QFileInfo::exists(dest)))
          return dest;
      }
    return {};
  }
  QString identity(const File &f, const Use &use) const {
    QString head = use.target.section('.', 0, 0);
    auto local = findDecl(f, use.owner, head);
    if (!f.imports.contains(local))
      return f.path + "|" + local +
             (use.target.contains('.') ? "." + use.target.section('.', 1) : "");
    auto binding = f.imports[local];
    QStringList pieces;
    if (!binding.symbol.isEmpty())
      pieces << binding.symbol;
    if (use.target.contains('.'))
      pieces << use.target.section('.', 1).split('.');
    if (pieces.isEmpty())
      return resolve(f.path, binding.module) + "|/";
    QString name = pieces.takeLast(), module = binding.module;
    if (!pieces.isEmpty())
      module += (module.endsWith('.') ? "" : ".") + pieces.join('.');
    return resolve(f.path, module) + "|/" + name;
  }
  void load(QString path) {
    path = normalize(path);
    if (files.contains(path))
      return;
    if (buffers.contains(path)) {
      files.insert(path, scan(path, buffers[path]));
      return;
    }
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || f.size() > 512 * 1024)
      return;
    files.insert(path, scan(path, QString::fromUtf8(f.readAll())));
  }

public:
  Project(QString folder, const QMap<QString, QString> &open)
      : root(normalize(folder)) {
    for (auto i = open.begin(); i != open.end(); ++i)
      buffers[normalize(i.key())] = i.value();
  }
  QJsonArray locations(QString current, int row, int column, bool references) {
    current = normalize(current);
    load(current);
    const auto &source = files[current];
    QString target;
    for (const auto &use : source.uses)
      if (use.row == row && use.column <= column &&
          use.column + use.name.size() >= column) {
        target = identity(source, use);
        break;
      }
    if (target.isEmpty())
      return {};
    auto dest = target.section('|', 0, 0);
    if (!dest.isEmpty())
      load(dest);
    if (references) {
      for (const auto &path : buffers.keys())
        load(path);
      QList<QString> dirs{root};
      int visited = 0;
      while (!dirs.isEmpty() && files.size() < 250 && visited++ < 500) {
        auto dir = dirs.takeFirst();
        QDirIterator it(dir, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot |
                                 QDir::NoSymLinks);
        while (it.hasNext() && files.size() < 250) {
          auto path = it.next();
          auto info = it.fileInfo();
          auto name = info.fileName();
          if (info.isDir()) {
            static const QStringList excluded{
                "__pycache__", "node_modules", "site-packages", "venv",
                "env",         "build",        "dist"};
            if (!name.startsWith('.') && !excluded.contains(name))
              dirs << path;
          } else if (info.suffix() == "py")
            load(path);
        }
      }
    }
    QJsonArray out;
    for (const auto &f : std::as_const(files))
      for (const auto &use : f.uses) {
        if ((!references && !use.defines) || identity(f, use) != target)
          continue;
        QJsonObject start{{"line", use.row}, {"character", use.column}};
        out.append(QJsonObject{
            {"uri", QUrl::fromLocalFile(f.path).toString(QUrl::FullyEncoded)},
            {"range", QJsonObject{{"start", start}}}});
      }
    return out;
  }
};
} // namespace pyindex
