#include "https_client.hpp"
#include <stdexcept>

#ifdef _WIN32

#include <winsock2.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")

namespace Mads {

HttpsClient::Response HttpsClient::_get_windows() {
    Response response;
    HINTERNET h_session = nullptr;
    HINTERNET h_connect = nullptr;
    HINTERNET h_request = nullptr;

    try {
        // Initialize WinHTTP
        h_session = WinHttpOpen(
            _user_agent.empty() ? L"WinHTTP/1.0" : std::wstring(_user_agent.begin(), _user_agent.end()).c_str(),
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0);

        if (!h_session) {
            throw std::runtime_error("WinHttpOpen failed");
        }

        // Convert hostname to wide string
        std::wstring w_hostname(_hostname.begin(), _hostname.end());
        std::wstring w_path(_path.begin(), _path.end());
        std::string query = query_string();
        std::wstring w_query(query.begin(), query.end());

        // Connect to server
        h_connect = WinHttpConnect(h_session, w_hostname.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!h_connect) {
            throw std::runtime_error("WinHttpConnect failed");
        }

        // Create request
        h_request = WinHttpOpenRequest(
            h_connect,
            L"GET",
            (w_path + w_query).c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            WINHTTP_FLAG_SECURE);

        if (!h_request) {
            throw std::runtime_error("WinHttpOpenRequest failed");
        }

        // Send request
        if (!WinHttpSendRequest(h_request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
            throw std::runtime_error("WinHttpSendRequest failed");
        }

        // Receive response
        if (!WinHttpReceiveResponse(h_request, nullptr)) {
            throw std::runtime_error("WinHttpReceiveResponse failed");
        }

        // Get status code
        DWORD status_code = 0;
        DWORD buffer_size = sizeof(status_code);
        WinHttpQueryHeaders(
            h_request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &status_code,
            &buffer_size,
            WINHTTP_NO_HEADER_INDEX);

        response.status_code = static_cast<int>(status_code);

        // Get status text
        WCHAR status_text[256] = {};
        buffer_size = sizeof(status_text) - 1;
        WinHttpQueryHeaders(
            h_request,
            WINHTTP_QUERY_STATUS_TEXT,
            WINHTTP_HEADER_NAME_BY_INDEX,
            status_text,
            &buffer_size,
            WINHTTP_NO_HEADER_INDEX);

        response.status_message = std::string(status_text, status_text + wcslen(status_text));

        // Read response body
        DWORD bytes_available = 0;
        while (WinHttpQueryDataAvailable(h_request, &bytes_available)) {
            if (bytes_available == 0) break;

            char buffer[8192];
            DWORD bytes_read = 0;

            if (!WinHttpReadData(h_request, buffer, sizeof(buffer), &bytes_read)) {
                throw std::runtime_error("WinHttpReadData failed");
            }

            if (bytes_read > 0) {
                response.body.append(buffer, bytes_read);
            }
        }

    } catch (const std::exception&) {
        // Clean up handles
        if (h_request) WinHttpCloseHandle(h_request);
        if (h_connect) WinHttpCloseHandle(h_connect);
        if (h_session) WinHttpCloseHandle(h_session);
        throw;
    }

    // Clean up
    if (h_request) WinHttpCloseHandle(h_request);
    if (h_connect) WinHttpCloseHandle(h_connect);
    if (h_session) WinHttpCloseHandle(h_session);

    return response;
}

} // namespace Mads

#endif // _WIN32
