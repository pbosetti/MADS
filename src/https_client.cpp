#include "https_client.hpp"
#include <stdexcept>
#include <sstream>

#ifdef _WIN32
    #include <winsock2.h>
    #include <winhttp.h>
    #pragma comment(lib, "winhttp.lib")
    #pragma comment(lib, "ws2_32.lib")
#elif __APPLE__
    #include <CoreFoundation/CoreFoundation.h>
    #include <CFNetwork/CFNetwork.h>
#else
    #include <openssl/ssl.h>
    #include <openssl/err.h>
    #include <openssl/bio.h>
    #include <sys/socket.h>
    #include <netdb.h>
    #include <unistd.h>
#endif

namespace Mads {

HttpsClient::HttpsClient()
    : _hostname(""), _path(""), _user_agent("") {}

HttpsClient::~HttpsClient() = default;

void HttpsClient::set_hostname(const std::string& hostname) {
    _hostname = hostname;
}

void HttpsClient::set_path(const std::string& path) {
    _path = path;
}

void HttpsClient::set_user_agent(const std::string& user_agent) {
    _user_agent = user_agent;
}

HttpsClient::Response HttpsClient::get() {
    if (_hostname.empty()) {
        throw std::runtime_error("HttpsClient: hostname not set");
    }
    if (_path.empty()) {
        throw std::runtime_error("HttpsClient: path not set");
    }

#ifdef _WIN32
    return _get_windows();
#elif __APPLE__
    return _get_macos();
#else
    return _get_linux();
#endif
}

#ifdef _WIN32

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

        // Connect to server
        h_connect = WinHttpConnect(h_session, w_hostname.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!h_connect) {
            throw std::runtime_error("WinHttpConnect failed");
        }

        // Create request
        h_request = WinHttpOpenRequest(
            h_connect,
            L"GET",
            w_path.c_str(),
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
        while (WinHttpQueryDataAvailable(h_request, &bytes_available, 0)) {
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

#elif __APPLE__

HttpsClient::Response HttpsClient::_get_macos() {
    Response response;

    // Create URL
    CFStringRef cf_hostname = CFStringCreateWithCString(kCFAllocatorDefault, _hostname.c_str(), kCFStringEncodingUTF8);
    CFStringRef cf_path = CFStringCreateWithCString(kCFAllocatorDefault, _path.c_str(), kCFStringEncodingUTF8);
    
    if (!cf_hostname || !cf_path) {
        if (cf_hostname) CFRelease(cf_hostname);
        if (cf_path) CFRelease(cf_path);
        throw std::runtime_error("Failed to create CFString objects");
    }

    // Construct URL: https://hostname/path
    CFStringRef cf_url_str = CFStringCreateWithFormat(
        kCFAllocatorDefault,
        nullptr,
        CFSTR("https://%@%@"),
        cf_hostname,
        cf_path);

    if (!cf_url_str) {
        CFRelease(cf_hostname);
        CFRelease(cf_path);
        throw std::runtime_error("Failed to create URL string");
    }

    CFURLRef cf_url = CFURLCreateWithString(kCFAllocatorDefault, cf_url_str, nullptr);
    if (!cf_url) {
        CFRelease(cf_url_str);
        CFRelease(cf_hostname);
        CFRelease(cf_path);
        throw std::runtime_error("Failed to create CFURL");
    }

    // Create request
    CFHTTPMessageRef request = CFHTTPMessageCreateRequest(
        kCFAllocatorDefault,
        CFSTR("GET"),
        cf_url,
        kCFHTTPVersion1_1);

    if (!request) {
        CFRelease(cf_url);
        CFRelease(cf_url_str);
        CFRelease(cf_hostname);
        CFRelease(cf_path);
        throw std::runtime_error("Failed to create HTTP request");
    }

    // Set User-Agent header if provided
    if (!_user_agent.empty()) {
        CFStringRef cf_user_agent = CFStringCreateWithCString(
            kCFAllocatorDefault,
            _user_agent.c_str(),
            kCFStringEncodingUTF8);

        if (cf_user_agent) {
            CFHTTPMessageSetHeaderFieldValue(request, CFSTR("User-Agent"), cf_user_agent);
            CFRelease(cf_user_agent);
        }
    }

    // Create read stream
    CFReadStreamRef read_stream = CFReadStreamCreateForHTTPRequest(kCFAllocatorDefault, request);
    if (!read_stream) {
        CFRelease(request);
        CFRelease(cf_url);
        CFRelease(cf_url_str);
        CFRelease(cf_hostname);
        CFRelease(cf_path);
        throw std::runtime_error("Failed to create read stream");
    }

    // Enable automatic redirect
    CFReadStreamSetProperty(read_stream, kCFStreamPropertyHTTPShouldAutoredirect, kCFBooleanTrue);

    // Disable certificate validation (use for self-signed certs)
    // CFMutableDictionaryRef ssl_settings = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    // CFDictionarySetValue(ssl_settings, kCFStreamSSLValidatesCertificateChain, kCFBooleanFalse);
    // CFReadStreamSetProperty(read_stream, kCFStreamPropertySSLSettings, ssl_settings);
    // CFRelease(ssl_settings);

    // Open stream
    if (!CFReadStreamOpen(read_stream)) {
        CFStringRef error_desc = CFErrorCopyDescription(CFReadStreamCopyError(read_stream));
        std::string error_msg;
        if (error_desc) {
            const char* c_str = CFStringGetCStringPtr(error_desc, kCFStringEncodingUTF8);
            if (c_str) {
                error_msg = c_str;
            }
            CFRelease(error_desc);
        }
        CFRelease(read_stream);
        CFRelease(request);
        CFRelease(cf_url);
        CFRelease(cf_url_str);
        CFRelease(cf_hostname);
        CFRelease(cf_path);
        throw std::runtime_error("Failed to open read stream: " + error_msg);
    }

    try {
        // Get response headers
        CFHTTPMessageRef response_headers = (CFHTTPMessageRef)CFReadStreamCopyProperty(
            read_stream,
            kCFStreamPropertyHTTPResponseHeader);

        if (response_headers) {
            response.status_code = CFHTTPMessageGetResponseStatusCode(response_headers);

            CFStringRef status_line = CFHTTPMessageCopyResponseStatusLine(response_headers);
            if (status_line) {
                const char* c_str = CFStringGetCStringPtr(status_line, kCFStringEncodingUTF8);
                if (c_str) {
                    // Extract just the status message part (e.g., "OK" from "HTTP/1.1 200 OK")
                    std::string full_line(c_str);
                    size_t last_space = full_line.rfind(' ');
                    if (last_space != std::string::npos && last_space > 0) {
                        response.status_message = full_line.substr(last_space + 1);
                    }
                }
                CFRelease(status_line);
            }

            CFRelease(response_headers);
        }

        // Read response body
        const UInt8 buffer_size = 8192;
        UInt8 buffer[buffer_size];

        CFIndex bytes_read = 0;
        while ((bytes_read = CFReadStreamRead(read_stream, buffer, buffer_size)) > 0) {
            response.body.append((const char*)buffer, bytes_read);
        }

        if (bytes_read < 0) {
            CFErrorRef error = CFReadStreamCopyError(read_stream);
            std::string error_msg;
            if (error) {
                CFStringRef error_desc = CFErrorCopyDescription(error);
                if (error_desc) {
                    const char* c_str = CFStringGetCStringPtr(error_desc, kCFStringEncodingUTF8);
                    if (c_str) {
                        error_msg = c_str;
                    }
                    CFRelease(error_desc);
                }
                CFRelease(error);
            }
            throw std::runtime_error("Failed to read response body: " + error_msg);
        }

    } catch (const std::exception&) {
        CFReadStreamClose(read_stream);
        CFRelease(read_stream);
        CFRelease(request);
        CFRelease(cf_url);
        CFRelease(cf_url_str);
        CFRelease(cf_hostname);
        CFRelease(cf_path);
        throw;
    }

    // Clean up
    CFReadStreamClose(read_stream);
    CFRelease(read_stream);
    CFRelease(request);
    CFRelease(cf_url);
    CFRelease(cf_url_str);
    CFRelease(cf_hostname);
    CFRelease(cf_path);

    return response;
}

#else // Linux with OpenSSL

HttpsClient::Response HttpsClient::_get_linux() {
    Response response;

    // Initialize SSL/TLS
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();

    const SSL_METHOD* method = TLS_client_method();
    if (!method) {
        throw std::runtime_error("Failed to get SSL method");
    }

    SSL_CTX* ctx = SSL_CTX_new(method);
    if (!ctx) {
        throw std::runtime_error("Failed to create SSL context");
    }

    // Don't verify certificates (use with caution in production)
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);

    try {
        // Create BIO chain
        BIO* bio = BIO_new_ssl_connect(ctx);
        if (!bio) {
            throw std::runtime_error("Failed to create BIO");
        }

        SSL* ssl = nullptr;
        BIO_get_ssl(bio, &ssl);
        if (!ssl) {
            BIO_free_all(bio);
            throw std::runtime_error("Failed to get SSL from BIO");
        }

        // Set connection string (hostname:port)
        std::string connect_str = _hostname + ":443";
        BIO_set_conn_hostname(bio, connect_str.c_str());

        // Connect
        if (BIO_do_connect(bio) <= 0) {
            BIO_free_all(bio);
            throw std::runtime_error("Failed to establish SSL connection");
        }

        // Build HTTP request
        std::string request = "GET " + _path + " HTTP/1.1\r\n";
        request += "Host: " + _hostname + "\r\n";
        if (!_user_agent.empty()) {
            request += "User-Agent: " + _user_agent + "\r\n";
        }
        request += "Connection: close\r\n";
        request += "Accept: */*\r\n";
        request += "\r\n";

        // Send request
        if (BIO_write(bio, request.c_str(), request.length()) <= 0) {
            BIO_free_all(bio);
            throw std::runtime_error("Failed to send HTTP request");
        }

        // Read response
        std::string raw_response;
        char buffer[8192];
        int bytes_read = 0;

        while ((bytes_read = BIO_read(bio, buffer, sizeof(buffer))) > 0) {
            raw_response.append(buffer, bytes_read);
        }

        // Parse HTTP response
        size_t header_end = raw_response.find("\r\n\r\n");
        if (header_end == std::string::npos) {
            BIO_free_all(bio);
            throw std::runtime_error("Invalid HTTP response format");
        }

        std::string headers = raw_response.substr(0, header_end);
        response.body = raw_response.substr(header_end + 4);

        // Parse status line
        size_t first_line_end = headers.find("\r\n");
        if (first_line_end != std::string::npos) {
            std::string status_line = headers.substr(0, first_line_end);
            
            // Extract status code and message from status line
            // Format: "HTTP/1.1 200 OK"
            std::istringstream iss(status_line);
            std::string http_version;
            int status_code;
            std::string status_message;

            if (iss >> http_version >> status_code) {
                std::getline(iss, status_message);
                // Remove leading space
                if (!status_message.empty() && status_message[0] == ' ') {
                    status_message = status_message.substr(1);
                }
                response.status_code = status_code;
                response.status_message = status_message;
            }
        }

        BIO_free_all(bio);

    } catch (const std::exception&) {
        SSL_CTX_free(ctx);
        EVP_cleanup();
        ERR_free_strings();
        throw;
    }

    SSL_CTX_free(ctx);
    EVP_cleanup();
    ERR_free_strings();

    return response;
}

#endif

} // namespace Mads
