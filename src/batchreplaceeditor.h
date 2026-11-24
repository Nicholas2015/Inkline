#pragma once
#include "batchreplace.h"
#include "editor.h"

inline void applyBatchToTabs(QTabWidget *tabs, const QJsonObject &file,
                             const QJsonArray &matches) {
  const QFileInfo path(file["path"].toString());
  QList<Edit *> editors;
  for (int i = 0; i < tabs->count(); ++i) {
    auto *tab = tabs->widget(i);
    const auto openPath = tab->property("filePath").toString();
    if (openPath.isEmpty() || QFileInfo(openPath) != path)
      continue;
    auto *e = dynamic_cast<Edit *>(tab);
    if (!e || e->isReadOnly() || e->dirty() ||
        e->doc.digest.toHex() != file["digest"].toString().toLatin1() ||
        e->doc.codec != file["codec"].toString().toLatin1() || e->text() != e->doc.text)
      throw std::runtime_error("打开的标签为只读、存在未保存的修改或版本不符");
    editors.append(e);
  }
  auto doc = applyBatchFile(file, matches);
  for (auto *e : editors) {
    int line, column;
    e->getCursorPosition(&line, &column);
    e->doc = doc;
    e->setText(doc.text);
    e->setModified(false);
    e->setCursorPosition(qMin(line, e->lines() - 1), column);
  }
}
