#include "http.h"
#include <curl/curl.h>
#include <algorithm>
#include <cctype>
#include <memory>
#include <mutex>
namespace music_tracker {
namespace {
constexpr std::size_t response_limit=2*1024*1024;
struct Transfer { HttpResponse response; const Cancelled* cancelled; bool too_large=false; };
std::size_t write_body(char* p, std::size_t size, std::size_t count, void* opaque) {
    auto& t=*static_cast<Transfer*>(opaque);auto bytes=size*count;
    if (bytes > response_limit-t.response.body.size()) {t.too_large=true;return 0;}
    try { t.response.body.append(p,bytes);return bytes; } catch (...) { return 0; }
}
std::size_t read_header(char* p, std::size_t size, std::size_t count, void* opaque) {
    auto bytes=size*count;
    try {
        std::string line(p,bytes), lower=line;
        std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
        if (lower.starts_with("retry-after:")) static_cast<Transfer*>(opaque)->response.retry_after=line.substr(12);
    } catch (...) { return 0; }
    return bytes;
}
int progress(void* opaque, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    try {return (*static_cast<Transfer*>(opaque)->cancelled)() ? 1 : 0;} catch (...) {return 1;}
}
}
void HttpClient::global_init() {
    static std::once_flag once;
    std::call_once(once,[]{if(curl_global_init(CURL_GLOBAL_DEFAULT)!=CURLE_OK)throw std::runtime_error("HTTP initialization failed");});
    // No curl_global_cleanup: another component may be using the process-global library.
}
HttpResponse HttpClient::get(const std::string& url,const Cancelled& cancelled) {
    if (cancelled()) throw LookupCancelled();
    if (!public_https_url(url)) throw std::runtime_error("Invalid lookup URL");
    std::unique_ptr<CURL,decltype(&curl_easy_cleanup)> c(curl_easy_init(),curl_easy_cleanup);
    if (!c) throw std::runtime_error("HTTP initialization failed");
    Transfer t{{},&cancelled};
    curl_easy_setopt(c.get(),CURLOPT_URL,url.c_str());
    curl_easy_setopt(c.get(),CURLOPT_USERAGENT,"discord-foobar-rpc/0.1.0 (foobar2000 component; MusicBrainz metadata)");
    curl_easy_setopt(c.get(),CURLOPT_ACCEPT_ENCODING,"");
    curl_easy_setopt(c.get(),CURLOPT_FOLLOWLOCATION,1L);
    curl_easy_setopt(c.get(),CURLOPT_MAXREDIRS,5L);
    #if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(c.get(),CURLOPT_PROTOCOLS_STR,"https");
    curl_easy_setopt(c.get(),CURLOPT_REDIR_PROTOCOLS_STR,"https");
#else
    curl_easy_setopt(c.get(),CURLOPT_PROTOCOLS,static_cast<long>(CURLPROTO_HTTPS));
    curl_easy_setopt(c.get(),CURLOPT_REDIR_PROTOCOLS,static_cast<long>(CURLPROTO_HTTPS));
#endif
    curl_easy_setopt(c.get(),CURLOPT_CONNECTTIMEOUT_MS,3000L);
    curl_easy_setopt(c.get(),CURLOPT_TIMEOUT_MS,8000L);
    curl_easy_setopt(c.get(),CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(c.get(),CURLOPT_WRITEFUNCTION,write_body);
    curl_easy_setopt(c.get(),CURLOPT_WRITEDATA,&t);
    curl_easy_setopt(c.get(),CURLOPT_HEADERFUNCTION,read_header);
    curl_easy_setopt(c.get(),CURLOPT_HEADERDATA,&t);
    curl_easy_setopt(c.get(),CURLOPT_NOPROGRESS,0L);
    curl_easy_setopt(c.get(),CURLOPT_XFERINFOFUNCTION,progress);
    curl_easy_setopt(c.get(),CURLOPT_XFERINFODATA,&t);
    const auto result=curl_easy_perform(c.get());
    if (cancelled() || result==CURLE_ABORTED_BY_CALLBACK) throw LookupCancelled();
    if (t.too_large) throw std::runtime_error("Metadata response exceeds size limit");
    if (result!=CURLE_OK) throw std::runtime_error("Metadata request failed: "+std::string(curl_easy_strerror(result)));
    curl_easy_getinfo(c.get(),CURLINFO_RESPONSE_CODE,&t.response.status);
    return std::move(t.response);
}
std::string url_encode(const std::string& text) {
    static constexpr char hex[]="0123456789ABCDEF";std::string out;
    for (unsigned char c:text) {
        if ((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~')out+=static_cast<char>(c);
        else {out+='%';out+=hex[c>>4];out+=hex[c&15];}
    }
    return out;
}
bool public_https_url(const std::string& url) {
    if (!url.starts_with("https://") || url.size()>4096 || url.find_first_of("\r\n\t ")!=std::string::npos)return false;
    const auto end=url.find_first_of("/?#",8);const auto host=url.substr(8,end==std::string::npos?end:end-8);
    return !host.empty() && host.find('@')==std::string::npos && host.find(':')==std::string::npos && host!="localhost";
}
}
