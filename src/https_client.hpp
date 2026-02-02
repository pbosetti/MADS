#pragma once

#include <string>

namespace Mads {

/**
 * @class HttpsClient
 * @brief Portable HTTPS GET client with zero external dependencies
 *
 * Uses platform-native APIs:
 * - macOS: Foundation framework (CFNetwork)
 * - Linux: OpenSSL (system library)
 * - Windows: WinHTTP
 */
class HttpsClient {
public:
    /**
     * @struct Response
     * @brief Result of an HTTPS GET request
     */
    struct Response {
        int status_code = 0;           ///< HTTP status code (e.g., 200, 404)
        std::string status_message;    ///< HTTP status message (e.g., "OK")
        std::string body;              ///< Response body as string
    };

    HttpsClient();
    ~HttpsClient();

    // Non-copyable
    HttpsClient(const HttpsClient&) = delete;
    HttpsClient& operator=(const HttpsClient&) = delete;

    /**
     * @brief Set the remote hostname (without https:// prefix)
     * @param hostname e.g., "api.example.com"
     */
    void set_hostname(const std::string& hostname);

    /**
     * @brief Set the API path on the remote server
     * @param path e.g., "/v1/status" or "/api/data"
     */
    void set_path(const std::string& path);

    /**
     * @brief Set the User-Agent header
     * @param user_agent e.g., "MyApp/1.0"
     */
    void set_user_agent(const std::string& user_agent);

    /**
     * @brief Perform HTTPS GET request to configured endpoint
     * @return Response containing status code, message, and body
     * @throws std::runtime_error on network or system errors
     */
    Response get();

private:
    std::string _hostname;
    std::string _path;
    std::string _user_agent;

#ifdef _WIN32
    Response _get_windows();
#elif __APPLE__
    Response _get_macos();
#else
    Response _get_linux();
#endif
};

} // namespace Mads
