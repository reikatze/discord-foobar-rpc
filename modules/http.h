#pragma once
#include "model.h"
#include <functional>
#include <stdexcept>
namespace music_tracker {
using Cancelled = std::function<bool()>;
struct LookupCancelled : std::runtime_error { LookupCancelled():std::runtime_error("Lookup cancelled"){} };
struct HttpResponse { long status=0; std::string body, retry_after; };
class HttpClient {
public:
    static void global_init();
    HttpResponse get(const std::string& url, const Cancelled& cancelled);
};
std::string url_encode(const std::string& text);
bool public_https_url(const std::string& url);
}
