#include "https_client.hpp"
#include <stdexcept>
#include <sstream>
#include <algorithm>
#include <cctype>

#if !defined(_WIN32) && !defined(__APPLE__)

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/bio.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>

namespace Mads {

// Returns the value of the first matching header (case-insensitive name search).
static std::string linux_get_header(const std::string &headers,
                                    const std::string &lower_name) {
    size_t pos = 0;
    bool first = true;
    while (pos < headers.size()) {
        size_t eol = headers.find("\r\n", pos);
        if (eol == std::string::npos) eol = headers.size();
        if (!first) {
            std::string line = headers.substr(pos, eol - pos);
            size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string key = line.substr(0, colon);
                std::transform(key.begin(), key.end(), key.begin(),
                               [](unsigned char c) { return std::tolower(c); });
                if (key == lower_name) {
                    std::string value = line.substr(colon + 1);
                    size_t vs = value.find_first_not_of(" \t");
                    return vs != std::string::npos ? value.substr(vs) : "";
                }
            }
        }
        first = false;
        if (eol >= headers.size()) break;
        pos = eol + 2;
    }
    return "";
}

static std::string linux_lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return value;
}

static std::string linux_trim_copy(const std::string &value) {
    size_t start = value.find_first_not_of(" \t");
    if (start == std::string::npos)
        return "";
    size_t end = value.find_last_not_of(" \t");
    return value.substr(start, end - start + 1);
}

static bool linux_header_has_token(const std::string &headers,
                                   const std::string &lower_name,
                                   const std::string &lower_token) {
    std::string value = linux_lower_copy(linux_get_header(headers, lower_name));
    size_t pos = 0;
    while (pos <= value.size()) {
        size_t comma = value.find(',', pos);
        std::string token = linux_trim_copy(value.substr(
            pos, comma == std::string::npos ? std::string::npos : comma - pos));
        if (token == lower_token)
            return true;
        if (comma == std::string::npos)
            break;
        pos = comma + 1;
    }
    return false;
}

static std::string linux_decode_chunked_body(const std::string &body) {
    std::string decoded;
    size_t pos = 0;

    while (true) {
        size_t line_end = body.find("\r\n", pos);
        if (line_end == std::string::npos)
            throw std::runtime_error("Invalid chunked HTTP response");

        std::string size_line = body.substr(pos, line_end - pos);
        size_t extension = size_line.find(';');
        if (extension != std::string::npos)
            size_line = size_line.substr(0, extension);
        size_line = linux_trim_copy(size_line);
        if (size_line.empty())
            throw std::runtime_error("Invalid chunked HTTP response");

        size_t parsed = 0;
        unsigned long long parsed_size = 0;
        try {
            parsed_size = std::stoull(size_line, &parsed, 16);
        } catch (const std::exception &) {
            throw std::runtime_error("Invalid chunked HTTP response");
        }
        if (parsed != size_line.size())
            throw std::runtime_error("Invalid chunked HTTP response");

        pos = line_end + 2;
        if (parsed_size == 0)
            return decoded;
        if (parsed_size > body.size() - pos)
            throw std::runtime_error("Invalid chunked HTTP response");

        size_t chunk_size = static_cast<size_t>(parsed_size);
        decoded.append(body.data() + pos, chunk_size);
        pos += chunk_size;
        if (pos + 2 > body.size() || body.compare(pos, 2, "\r\n") != 0)
            throw std::runtime_error("Invalid chunked HTTP response");
        pos += 2;
    }
}

HttpsClient::Response HttpsClient::_get_linux() {
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();

    const SSL_METHOD* method = TLS_client_method();
    if (!method) throw std::runtime_error("Failed to get SSL method");

    SSL_CTX* ctx = SSL_CTX_new(method);
    if (!ctx) throw std::runtime_error("Failed to create SSL context");
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);

    std::string current_hostname = _hostname;
    std::string current_path = _path + query_string();
    Response response;

    try {
        static const int MAX_REDIRECTS = 10;
        for (int redirect = 0; redirect <= MAX_REDIRECTS; ++redirect) {
            BIO* bio = BIO_new_ssl_connect(ctx);
            if (!bio) throw std::runtime_error("Failed to create BIO");

            SSL* ssl = nullptr;
            BIO_get_ssl(bio, &ssl);
            if (!ssl) { BIO_free_all(bio); throw std::runtime_error("Failed to get SSL"); }

            // SNI: required by most modern HTTPS servers
            SSL_set_tlsext_host_name(ssl, current_hostname.c_str());

            std::string connect_str = current_hostname + ":443";
            BIO_set_conn_hostname(bio, connect_str.c_str());

            if (BIO_do_connect(bio) <= 0) {
                BIO_free_all(bio);
                throw std::runtime_error("Failed to connect to " + current_hostname);
            }

            std::string request = "GET " + current_path + " HTTP/1.1\r\n";
            request += "Host: " + current_hostname + "\r\n";
            if (!_user_agent.empty())
                request += "User-Agent: " + _user_agent + "\r\n";
            request += "Connection: close\r\nAccept: */*\r\n\r\n";

            if (BIO_write(bio, request.c_str(), (int)request.size()) <= 0) {
                BIO_free_all(bio);
                throw std::runtime_error("Failed to send HTTP request");
            }

            std::string raw_response;
            char buffer[8192];
            int bytes_read;
            while ((bytes_read = BIO_read(bio, buffer, sizeof(buffer))) > 0)
                raw_response.append(buffer, bytes_read);
            BIO_free_all(bio);

            size_t header_end = raw_response.find("\r\n\r\n");
            if (header_end == std::string::npos)
                throw std::runtime_error("Invalid HTTP response");

            std::string headers = raw_response.substr(0, header_end);
            response.body = raw_response.substr(header_end + 4);
            if (linux_header_has_token(headers, "transfer-encoding", "chunked"))
                response.body = linux_decode_chunked_body(response.body);

            // Parse status line
            size_t first_line_end = headers.find("\r\n");
            std::string status_line = headers.substr(
                0, first_line_end != std::string::npos ? first_line_end : headers.size());
            {
                std::istringstream iss(status_line);
                std::string http_version, status_message;
                if (iss >> http_version >> response.status_code) {
                    std::getline(iss, status_message);
                    if (!status_message.empty() && status_message[0] == ' ')
                        status_message = status_message.substr(1);
                    response.status_message = status_message;
                }
            }

            // Follow 3xx redirects via the Location header
            if (response.status_code >= 300 && response.status_code < 400) {
                std::string location = linux_get_header(headers, "location");
                if (location.empty())
                    break; // no Location: give up and return what we have
                if (location.rfind("https://", 0) == 0) {
                    size_t host_start = 8;
                    size_t path_start = location.find('/', host_start);
                    if (path_start == std::string::npos) {
                        current_hostname = location.substr(host_start);
                        current_path = "/";
                    } else {
                        current_hostname = location.substr(host_start, path_start - host_start);
                        current_path = location.substr(path_start);
                    }
                } else if (!location.empty() && location[0] == '/') {
                    current_path = location; // same host, new absolute path
                } else {
                    break; // relative or http:// redirect; give up
                }
                continue;
            }
            break; // 2xx or error: done
        }
    } catch (...) {
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

} // namespace Mads

#endif // Linux
