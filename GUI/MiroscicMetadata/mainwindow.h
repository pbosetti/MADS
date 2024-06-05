#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QSettings>
#include <QThread>
#include <QFutureWatcher>
#include "qjsonmodel.h"
#include "qagent.h"
#include <nlohmann/json.hpp>

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

#include <agent.hpp>

class MainWindow : public QMainWindow {
  Q_OBJECT

public:
  MainWindow (QWidget *parent = nullptr);
  ~MainWindow ();

  void writeSettings();
  void readSettings();

private:
  Ui::MainWindow *ui;
  Mads::QAgent _agent;
  QSettings _settings = QSettings(APP_DOMAIN, APP_NAME);
  QThread *_connThread = nullptr;
  QFutureWatcher<bool> _watcher;
  QJsonModel *_model = nullptr;
  nlohmann::json collectData();


private slots:
  void sendMessage(bool clicked);
  void closeEvent(QCloseEvent *event);
};
#endif // MAINWINDOW_H
