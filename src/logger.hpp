/*
  _                                       _
 | |    ___   __ _  __ _  ___ _ __    ___| | __ _ ___ ___
 | |   / _ \ / _` |/ _` |/ _ \ '__|  / __| |/ _` / __/ __|
 | |__| (_) | (_| | (_| |  __/ |    | (__| | (_| \__ \__ \
 |_____\___/ \__, |\__, |\___|_|     \___|_|\__,_|___/___/
             |___/ |___/

This class subscribes to all messages published by the broker and logs them to a
MongoDB instance.

Author(s): Paolo Bosetti
*/

#ifndef LOGGER_HPP
#define LOGGER_HPP


#include "mads.hpp"
#include "agent.hpp"
#include <memory>

namespace Mads {

/**
 * @brief The Logger class is responsible for logging messages to a MongoDB
 * instance.
 *
 * It inherits from the Agent class and provides methods for logging messages
 * and connecting to the database.
 *
 * @note The MongoDB driver objects backing this class are hidden behind a
 * pointer to implementation, so this header pulls in no bsoncxx/mongocxx
 * headers and `sizeof(Logger)` does not depend on the driver ABI.
 *
 * @note MongoDB support is optional at build time. When MADS is built with
 * `MADS_ENABLE_MONGOCXX=OFF` the class still works as a file logger, and
 * `has_mongo_support()` returns `false`. See @ref has_mongo_support.
 *
 * @see Metadata for example usage. This class also has the log method.
 */
class Logger : public Agent {
public:
  /**
   * @brief Constructs a Logger object with the specified name and settings
   * path.
   *
   * @param name The name of the logger.
   * @param settings_path The path to the settings file.
   */
  Logger(std::string name, std::string settings_path);

  ~Logger();

  Logger(const Logger &) = delete;
  Logger &operator=(const Logger &) = delete;

  /**
   * @brief Reports whether this build can log to MongoDB.
   *
   * @return `true` when MADS was built with `MADS_ENABLE_MONGOCXX=ON`. When it
   * returns `false`, logging to MongoDB is unavailable: `set_mongo(true)` warns
   * and has no effect, and only file logging is performed.
   */
  static bool has_mongo_support() noexcept;

  /**
   * @brief Sets whether logging to MongoDB is enabled or disabled.
   *
   * @param enabled Whether logging to MongoDB is enabled or disabled. Default
   * is true.
   * @param uri The URI of the MongoDB instance. If not empty, overrides the
   * one in the settings.
   */
  void set_mongo(bool enabled = true, string uri = "");

  /**
   * @brief Sets the file name for logging.
   *
   * @param filename The name of the file to log to.
   * @param array (optional) Indicates whether the log should be stored as an
   * array. If not is is stored as a JSON object per line. Default is false.
   */
  void set_file(string filename, bool array = false);

  /**
   * @brief Sets whether logging should be done to a file.
   *
   * @param enabled Whether logging to a file should be enabled or disabled.
   * Default is false.
   */
  void set_file(bool enabled = false);

  /**
   * @brief Starts the logger.
   *
   * This method opens the logfile (if enabled) and connects to the MongoDB
   * instance (if enabled)
   */
  void open_db();

  /**
   * @brief Stops the logger.
   *
   * This method closes the logfile (if enabled) and disconnects from the
   * MongoDB instance (if enabled)
   */
  void close_db();

  /**
   * @brief Overrides the info method from the Agent class.
   *
   * This method provides information about the logger.
   */
  void info(ostream &out = cout) override;

  /**
   * @brief Registers the startup/shutdown of the logger.
   *
   * This method registers the startup of the logger to the MongoDB instance.
   *
   * @param event The event to be registered.
   */
  void register_event(event_type event);

  /**
   * @brief Logs the given message to the MongoDB instance and to the log file.
   *
   * @param message A touple containing the topic and the message to be logged.
   */
  void log(tuple<string, string> *message = nullptr);

  /**
   * @brief Logs the last message to the MongoDB instance and to the log file.
   *
   * @param type the type of message: json or blob.
   */
  void log(message_type type);

  /**
   * @brief Truncates the message to the maximum length.
   *
   * @param message The message to be truncated.
   * @return string The truncated message.
   */
  string truncated_message(const string &message);

public:
  bool paused = false; // Whether the logger is paused or not

private:
  void load_settings() override;

  void connect_to_db();
  void open_log_file(string filename, bool array = false);
  void close_log_file();
  void log_to_mongo(tuple<string, string> *message = nullptr);
  void log_doc_to_mongo(const std::string &topic, const nlohmann::json &message);
  void log_blob_to_mongo();
  void log_to_file(tuple<string, string> *message = nullptr);

  struct Impl;
  std::unique_ptr<Impl> _impl;
};

} // namespace Mads

#endif // LOGGER_HPP
