#include "https_client.hpp"
#include <stdexcept>

#ifdef __APPLE__

#import <Foundation/Foundation.h>

namespace Mads {

HttpsClient::Response HttpsClient::_get_macos() {
    Response response;

    @autoreleasepool {
        // Construct URL string
        NSString* url_string = [NSString stringWithFormat:@"https://%s%s%s",
                                _hostname.c_str(), _path.c_str(), query_string().c_str()];
        NSURL* url = [NSURL URLWithString:url_string];

        if (!url) {
            throw std::runtime_error("Failed to create URL");
        }

        // Create request
        NSMutableURLRequest* request = [NSMutableURLRequest requestWithURL:url];
        [request setHTTPMethod:@"GET"];

        // Set User-Agent header if provided
        if (!_user_agent.empty()) {
            NSString* user_agent = [NSString stringWithUTF8String:_user_agent.c_str()];
            [request setValue:user_agent forHTTPHeaderField:@"User-Agent"];
        }

        // Create session with timeout
        NSURLSessionConfiguration* config = [NSURLSessionConfiguration defaultSessionConfiguration];
        config.timeoutIntervalForRequest = 30.0;
        config.timeoutIntervalForResource = 60.0;

        NSURLSession* session = [NSURLSession sessionWithConfiguration:config];

        // Synchronous request using semaphore
        __block NSData* response_data = nil;
        __block NSURLResponse* url_response = nil;
        __block NSError* request_error = nil;
        __block bool request_complete = false;

        dispatch_semaphore_t semaphore = dispatch_semaphore_create(0);

        NSURLSessionDataTask* task = [session dataTaskWithRequest:request
            completionHandler:^(NSData* data, NSURLResponse* response, NSError* error) {
                response_data = [data copy];  // Make a copy to ensure data persists
                url_response = [response retain];
                request_error = [error retain];
                request_complete = true;
                dispatch_semaphore_signal(semaphore);
            }];

        [task resume];

        // Wait for completion (with timeout)
        long result = dispatch_semaphore_wait(semaphore, dispatch_time(DISPATCH_TIME_NOW, 60LL * NSEC_PER_SEC));

        if (result != 0) {
            [session invalidateAndCancel];
            throw std::runtime_error("HTTP request timed out");
        }

        if (!request_complete) {
            [session invalidateAndCancel];
            throw std::runtime_error("HTTP request did not complete");
        }

        if (request_error) {
            NSString* error_desc = [request_error localizedDescription];
            std::string error_msg([error_desc UTF8String]);
            [request_error release];
            [session invalidateAndCancel];
            throw std::runtime_error("HTTP request failed: " + error_msg);
        }

        // Process response
        if ([url_response isKindOfClass:[NSHTTPURLResponse class]]) {
            NSHTTPURLResponse* http_response = (NSHTTPURLResponse*)url_response;
            response.status_code = (int)[http_response statusCode];

            // Get status message from status code
            NSString* status_text = [NSHTTPURLResponse localizedStringForStatusCode:[http_response statusCode]];
            if (status_text) {
                response.status_message = std::string([status_text UTF8String]);
            }
        }

        // Extract response body
        if (response_data) {
            response.body = std::string((const char*)[response_data bytes], [response_data length]);
        }

        // Clean up retained objects
        if (url_response) [url_response release];
        if (response_data) [response_data release];
        [session invalidateAndCancel];
    }

    return response;
}

} // namespace Mads

#endif // __APPLE__