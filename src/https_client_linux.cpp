#include "https_client.hpp"
#include <stdexcept>
#include <sstream>

#if !defined(_WIN32) && !defined(__APPLE__)

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/bio.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <iostream>

namespace Mads {

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
        std::string request = "GET " + (_path + query_string()) + " HTTP/1.1\r\n";
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
        std::string raw_response = "";
        char buffer[8192];
        memset(buffer, 0, 8192);
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

} // namespace Mads

#endif // Linux
