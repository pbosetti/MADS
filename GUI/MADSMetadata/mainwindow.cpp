#include "mainwindow.h"
#include "./ui_mainwindow.h"
#include <QDebug>
#include <QFutureWatcher>
#include <QRegularExpression>
#include <QtConcurrent/QtConcurrent>
#include <nlohmann/json.hpp>

using namespace Mads;
using json = nlohmann::json;

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), ui(new Ui::MainWindow), _agent(QAgent("", "")) {
  ui->setupUi(this);
  readSettings();
  ui->commentText->setAcceptRichText(false);
  ui->lastJSON->setReadOnly(true);
  ui->enableLogging->setEnabled(false);
  _agent.set_settings_timeout(5000);
  ui->tabWidget->setCurrentIndex(0);
  ui->markOutButton->setEnabled(false);
  ui->markInButton->setEnabled(false);
  ui->immediateMarkButton->setEnabled(false);
  ui->stopButton->setEnabled(false);
  ui->restartButton->setEnabled(false);

  // Broker line, dynamically enables connect button
  connect(ui->brokerURILine, &QLineEdit::textChanged, this, [&]() {
    static QRegularExpression re_tcp(R"(^tcp://([a-z0-9.]+):([0-9]{3,5})$)");
    QRegularExpressionMatch match = re_tcp.match(ui->brokerURILine->text());
    ui->connectButton->setEnabled(match.hasMatch());
  });

  // What to do when connected
  connect(&_watcher, &QFutureWatcher<bool>::finished, this, [&]() {
    if (_watcher.result()) {
      statusBar()->showMessage("Connected");
      ui->sendMessageButton->setEnabled(true);
      ui->sendMessageButton->setText("Mark with info (published to the " + ui->agentNameLine->text() + " topic)");
      ui->connectButton->setText("Click to disconnect");
      ui->enableLogging->setEnabled(true);
      ui->markOutButton->setEnabled(false);
      ui->markInButton->setEnabled(true);
      ui->immediateMarkButton->setEnabled(true);
      ui->stopButton->setEnabled(true);
      ui->restartButton->setEnabled(true);
      _agent.start();
    } else {
      statusBar()->showMessage("Timed out in getting settings from broker");
      ui->brokerURILine->setEnabled(true);
      ui->agentNameLine->setEnabled(true);
      ui->connectButton->setText("Click to connect");
    }
  });

  // Connect button triggers action
  connect(ui->connectButton, &QPushButton::clicked, this, [&](bool c) {
    if (c) {
      ui->connectButton->setText("Connecting...");
      statusBar()->showMessage("Connecting to broker...");
      // the future returns false if there's a timeout in getting settings
      QFuture<bool> future = QtConcurrent::run([&]() -> bool {
        try {
          _agent.init(ui->agentNameLine->text().toStdString(),
                      ui->brokerURILine->text().toStdString());
          _agent.set_sub_topic("");
          _agent.connect(std::chrono::milliseconds(100));
          _agent.register_event(event_type::startup);
        } catch (...) {
          ui->connectButton->setChecked(false);
          return false;
        }
        return true;
      });
      _watcher.setFuture(future);
    } else {
      _agent.register_event(event_type::shutdown);
      _agent.requestInterruption();
      _agent.disconnect();
      statusBar()->showMessage("Disconnected", 5000);
      ui->connectButton->setText("Click to connect");
      ui->sendMessageButton->setEnabled(false);
      ui->sendMessageButton->setText("Mark with info");
      ui->enableLogging->setEnabled(false);
      ui->markOutButton->setEnabled(false);
      ui->markInButton->setEnabled(false);
      ui->immediateMarkButton->setEnabled(false);
      ui->stopButton->setEnabled(false);
      ui->restartButton->setEnabled(false);
    }
    ui->brokerURILine->setEnabled(!c);
    ui->agentNameLine->setEnabled(!c);
  });

  // Agent name line updates the name of the agent (defailt topic)
  connect(ui->agentNameLine, &QLineEdit::textEdited, this,
          [=, this](QString s) { ui->agentNameLine->setText(s.toLower()); });

  // enable/disable logging checkbox acts immediately
  connect(ui->enableLogging, &ToggleSwitch::toggled, this, [=, this](bool checked) {
    json j;
    j["pause"] = !(checked);
    _agent.publish(j);
  });

  connect(ui->markInButton, &QPushButton::clicked, this, [=, this]() {
    const json j = collectData();
    _agent.register_event(event_type::marker_in, j);
    ui->markInButton->setEnabled(false);
    ui->markOutButton->setEnabled(true);
  });

  connect(ui->markOutButton, &QPushButton::clicked, this, [=, this]() {
    _agent.register_event(event_type::marker_out);
    ui->trialNumber->stepUp();
    ui->markInButton->setEnabled(true);
    ui->markOutButton->setEnabled(false);
  });

  connect(ui->immediateMarkButton, &QPushButton::clicked, this, [=, this]() {
    _agent.register_event(event_type::marker);
  });

  // send message button has its function
  connect(ui->sendMessageButton, &QPushButton::clicked, this,
          &MainWindow::sendMessage);

  connect(ui->stopButton, &QPushButton::clicked, this, [&]() {
    json j{{"cmd", "shutdown"}};
    _agent.publish(j, "control");
  });

  connect(ui->restartButton, &QPushButton::clicked, this, [&]() {
    json j{{"cmd", "restart"}};
    _agent.publish(j, "control");
  });

  // Subscriber thread: on new message, updates the JSON tree view
  connect(&_agent, &QAgent::gotNewMessage, this, [=, this]() {
    json j;
    for (auto &[k, v] : _agent.status()) {
      j[k] = json::parse(v);
    }
    if (j.contains(LOGGER_STATUS_TOPIC)) {
      bool paused = j[LOGGER_STATUS_TOPIC]["logger_paused"];
      if (ui->enableLogging->isChecked() == paused) {
        qDebug() << "paused: " << paused << " switch: " << ui->enableLogging->isChecked();
        ui->enableLogging->blockSignals(true);
        ui->enableLogging->setChecked(!paused);
        ui->enableLogging->blockSignals(false);
      }
    }
    if (ui->tabWidget->currentIndex() != 1) return;
    QByteArray ba;
    ba = QByteArray(j.dump().c_str());
    _model->loadJson(ba);
    if (ui->keepExpandedCheckBox->isChecked())
      ui->treeView->expandAll();
  });

  _model = new QJsonModel;
  ui->treeView->setModel(_model);
}

void MainWindow::closeEvent(QCloseEvent *event) {
  writeSettings();
  if (_agent.is_connected()) {
    _agent.requestInterruption();
    while(_agent.isRunning()) {
      QThread::msleep(100);
    }
    _agent.register_event(event_type::shutdown);
    _agent.disconnect();
  } else {
    exit(EXIT_SUCCESS);
  }
}

MainWindow::~MainWindow() { delete ui; }

json MainWindow::collectData() {
  json j;
  j["user"] = ui->userLine->text().toStdString();
  j["operation"] = ui->operationLine->text().toStdString();
  j["operator"] = ui->operatorLine->text().toStdString();
  j["test#"] = ui->trialNumber->value();
  if (ui->commentIsJSON->isChecked()) {
    j["comment"] = json::parse(ui->commentText->toPlainText().toStdString());
  } else {
    j["comment"] = ui->commentText->toPlainText().toStdString();
  }
  j["isATest"] = ui->thisIsATest->isChecked();
  return j;
}

void MainWindow::sendMessage(bool clicked) {
  json j = collectData();
  _agent.publish(j);
  ui->lastJSON->clear();
  ui->lastJSON->insertPlainText(QString::fromUtf8(j.dump(4)));
}

void MainWindow::writeSettings() {
  _settings.beginGroup("MainWindow");
  _settings.setValue("brokerURI", ui->brokerURILine->text());
  _settings.setValue("agentName", ui->agentNameLine->text());
  _settings.setValue("user", ui->userLine->text());
  _settings.setValue("operation", ui->operationLine->text());
  _settings.setValue("operator", ui->operatorLine->text());
  _settings.setValue("isATest", ui->thisIsATest->isChecked());
  _settings.setValue("windowGeometry", saveGeometry());
  _settings.setValue("trialNumber", ui->trialNumber->value());
  _settings.endGroup();
}

void MainWindow::readSettings() {
  _settings.beginGroup("MainWindow");

  auto val = _settings.value("brokerURI");
  if (val.isNull())
    ui->brokerURILine->setText("tcp://localhost:9092");
  else
    ui->brokerURILine->setText(val.toString());

  val = _settings.value("agentName");
  if (val.isNull())
    ui->agentNameLine->setText(QString(APP_NAME).toLower());
  else
    ui->agentNameLine->setText(val.toString());

  val = _settings.value("user");
  if (!val.isNull())
    ui->userLine->setText(val.toString());

  val = _settings.value("operation");
  if (!val.isNull())
    ui->operationLine->setText(val.toString());

  val = _settings.value("operator");
  if (!val.isNull())
    ui->operatorLine->setText(val.toString());

  val = _settings.value("isATest");
  if (!val.isNull())
    ui->thisIsATest->setChecked(val.toBool());

  val = _settings.value("trialNumber");
  if (!val.isNull())
    ui->thisIsATest->setChecked(val.toUInt());

  restoreGeometry(_settings.value("windowGeometry").toByteArray());
  _settings.endGroup();
}
