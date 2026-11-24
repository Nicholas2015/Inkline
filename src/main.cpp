#include "app/mainwindow.h"
#include "app/verifyinstall.h"
#include "appearance.h"
#include "logscan.h"
#include "searchtools.h"
#include <QFileOpenEvent>
#include <QSettings>
#include <functional>

class DesktopApp final : public QApplication {
public:
  using QApplication::QApplication;
  QStringList queued;
  std::function<void(QString)> openFile;
  bool event(QEvent *ev) override {
    if (ev->type() == QEvent::FileOpen) {
      auto path = static_cast<QFileOpenEvent *>(ev)->file();
      if (!path.isEmpty()) {
        if (openFile)
          openFile(path);
        else
          queued.append(path);
        return true;
      }
    }
    return QApplication::event(ev);
  }
};
int main(int argc, char **argv) {
  if (argc == 2 && QByteArray(argv[1]) == "--log-job")
    return runLogJob();
  if (argc == 3 && QByteArray(argv[1]) == "--search-worker") {
    QCoreApplication core(argc, argv);
    return runSearchWorker(core.arguments().at(2));
  }
  DesktopApp app(argc, argv);
  app.setOrganizationName("InkEdit");
  // Keep the original storage identity for settings, session locks and draft bodies.
  app.setApplicationName("InkEditNative");
  app.setApplicationDisplayName("墨行 Inkline");
  Appearance::apply(app);
  app.setWindowIcon(QIcon(":/inkline.png"));
  if (argc == 3 && app.arguments()[1] == "--verify-install")
    return verifyInstall(app.arguments()[2]);
  auto profile = qEnvironmentVariable("INKLINE_PROFILE", qEnvironmentVariable("INKEDIT_NATIVE_PROFILE"));
  if (!profile.isEmpty()) {
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, profile);
  }
  Window w;
  w.startSession(profile.isEmpty() ? QString()
                                   : QDir(profile).filePath("sessions"));
  app.openFile = [&w](QString path) {
    w.open(path);
    w.show();
    w.raise();
  };
  for (const auto &file : app.queued + app.arguments().sliced(1))
    w.open(file);
  w.ensureTab();
  w.show();
  int code = app.exec();
  app.openFile = {};
  return code;
}
