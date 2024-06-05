#include "qagent.h"

namespace Mads {

QAgent::QAgent(QString name, QString settings_uri, QObject *parent) : QThread{parent}, Agent(name.toStdString(), settings_uri.toStdString()) {

}


void QAgent::run() {
  _subscriber.set(zmqpp::socket_option::receive_timeout, SOCKET_TIMEOUT);
  message_type mt;
  while (!isInterruptionRequested()) {
    mt = receive();
    if (mt == message_type::none) {
      emit noMessage();
    } else if (mt == message_type::json) {
      emit gotNewMessage();
    }
  }
}

void QAgent::set_sub_topic(QString topic) {
  _sub_topic.clear();
  _sub_topic.push_back(topic.toStdString());
}

void QAgent::set_sub_topic(std::vector<std::string> topics) {
  _sub_topic = topics;
}

} // namespace Mads
