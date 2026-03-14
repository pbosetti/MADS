/*
  MADS Smoke Test: C Wrapper Agent
  Validates that an external project can compile C code and link against
  the MADS library via the agent_c.h C interface.
  
  Usage: test_c_agent <settings_uri>
*/
#include <agent_c.h>
#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#define msleep(ms) usleep((ms) * 1000)
#else
#include <Windows.h>
#define msleep(ms) Sleep(ms)
#endif

int main(int argc, const char *argv[]) {
  const char *settings_uri = "tcp://localhost:19092";
  if (argc > 1) {
    settings_uri = argv[1];
  }

  /* Library version */
  printf("MADS version: %s\n", mads_version());
  printf("Default URI:  %s\n", mads_default_settings_uri());

  /* Create agent */
  agent_t agent = agent_create("c_test", settings_uri);
  if (!agent) {
    fprintf(stderr, "FAIL: agent_create returned NULL\n");
    return 1;
  }
  printf("Agent created\n");

  /* Set and get ID */
  agent_set_id(agent, "smoke_c_agent");
  const char *id = agent_id(agent);
  if (!id || strcmp(id, "smoke_c_agent") != 0) {
    fprintf(stderr, "FAIL: agent_set_id/agent_id mismatch\n");
    agent_destroy(agent);
    return 1;
  }
  printf("Agent ID: %s\n", id);

  /* Settings timeout */
  agent_set_settings_timeout(agent, 5000);
  if (agent_settings_timeout(agent) != 5000) {
    fprintf(stderr, "FAIL: settings timeout mismatch\n");
    agent_destroy(agent);
    return 1;
  }

  /* Initialize */
  if (agent_init(agent, false) != 0) {
    fprintf(stderr, "FAIL: agent_init: %s\n", agent_last_error());
    agent_destroy(agent);
    return 1;
  }
  printf("Agent initialized\n");

  /* High watermark (must be set before connect) */
  agent_set_high_watermark(agent, 100);
  if (agent_high_watermark(agent) != 100) {
    fprintf(stderr, "FAIL: high_watermark mismatch\n");
    agent_destroy(agent);
    return 1;
  }

  /* Connect */
  if (agent_connect(agent, 500) != 0) {
    fprintf(stderr, "FAIL: agent_connect: %s\n", agent_last_error());
    agent_destroy(agent);
    return 1;
  }
  printf("Agent connected\n");

  /* Publish */
  int i;
  for (i = 0; i < 5; i++) {
    char msg[128];
    snprintf(msg, sizeof(msg), "{\"source\":\"smoke_c\",\"seq\":%d}", i);
    if (agent_publish(agent, msg, "c_test") != 0) {
      fprintf(stderr, "FAIL: agent_publish: %s\n", agent_last_error());
      agent_disconnect(agent);
      agent_destroy(agent);
      return 1;
    }
    printf("Published message %d\n", i);
    msleep(50);
  }

  /* Non-blocking receive (expect nothing) */
  agent_set_receive_timeout(agent, 500);
  message_type_t mt = agent_receive(agent, true);
  printf("Non-blocking receive returned: %d\n", mt);

  /* Disconnect and destroy */
  agent_disconnect(agent);
  agent_destroy(agent);
  printf("PASS: C wrapper agent test completed successfully\n");
  return 0;
}
