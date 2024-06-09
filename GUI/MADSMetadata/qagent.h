#ifndef MADS_QAGENT_H
#define MADS_QAGENT_H

#include "agent.hpp"
#include <QObject>
#include <QThread>

#ifndef SOCKET_TIMEOUT
#define SOCKET_TIMEOUT 250
#endif

namespace Mads {

class QAgent : public QThread, public Agent {
  Q_OBJECT
public:
  explicit QAgent (QString name, QString settings_uri, QObject *parent = nullptr);

  using Agent::connect;
  using Agent::disconnect;

  void run() override;

  void set_sub_topic(QString topic);
  void set_sub_topic(std::vector<std::string> topics);

signals:
  void gotNewMessage();
  void noMessage();
};

} // namespace Mads

#endif // MADS_QAGENT_H
