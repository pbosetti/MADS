#pragma once

#include <memory>
#include <string>

class PlatformProcess {
 public:
  virtual ~PlatformProcess() = default;

  virtual bool start(const std::string& command, const std::string& workdir, bool tty, std::string* out_error) = 0;
  virtual void stop() = 0;
  virtual bool is_running() = 0;
  virtual int exit_code() const = 0;
  virtual int process_id() const = 0;
  virtual bool read_available(std::string* out_chunk) = 0;
  virtual bool write_input(const std::string& data) = 0;
  virtual bool attach_until_detach(std::string* out_error) = 0;
};

std::unique_ptr<PlatformProcess> create_platform_process();
