/*
   ____      _                    _   
  / ___|    / \   __ _  ___ _ __ | |_ 
 | |       / _ \ / _` |/ _ \ '_ \| __|
 | |___   / ___ \ (_| |  __/ | | | |_ 
  \____| /_/   \_\__, |\___|_| |_|\__|
                 |___/                

Test file
Paolo Bosetti 2026
*/            

#include "agent_c.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, const char **argv) {
  agent_t agent = NULL;
  printf("Using MADS library version %s\n", mads_version());
  if (argc == 1)
    agent = agent_create("c_test", "tcp://localhost:9092");
  else
    agent = agent_create("c_test", argv[1]);

  printf("Reading settings from %s\n", agent_settings_uri(agent));
  agent_set_settings_timeout(agent, 2000);
  printf("Settings timeout: %d ms\n", agent_settings_timeout(agent));

  if (agent_init(agent, false)) {
    printf("Fatal : %s\n", agent_last_error(agent));
    return -1;
  }
  agent_set_id(agent, "Test C agent");
  agent_connect(agent, 1000);
  agent_get_settings(agent);
  agent_print_settings(agent);
  agent_connect(agent, 1000);
  agent_register_event(agent, startup,
                       "{\"message\":\"Feedback agent started\"}");

  int i = 0;
  char j[256];
  for (i = 0; i < 10; i++) {
    snprintf(j, 256, "{\"i\": %d}", i);
    agent_publish(agent, "test", j);
    usleep(100);
  }

  sleep(1);
  agent_register_event(agent, shutdown, NULL);
  sleep(1);
  agent_disconnect(agent);
  return 0;
}