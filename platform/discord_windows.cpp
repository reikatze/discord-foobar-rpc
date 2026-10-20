#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include "../modules/discord.h"
#include <stdexcept>
#include <algorithm>
namespace music_tracker {
namespace {
class PipeTransport final : public DiscordTransport {
public:
    explicit PipeTransport(HANDLE pipe):pipe_(pipe){}
    ~PipeTransport() override {CloseHandle(pipe_);}
    void send(const std::string& bytes) override {
        std::size_t offset=0;
        while(offset<bytes.size())offset+=transfer(true,const_cast<char*>(bytes.data()+offset),static_cast<DWORD>(bytes.size()-offset));
    }
    std::string receive_available() override {
        DWORD available=0;
        if(!PeekNamedPipe(pipe_,nullptr,0,nullptr,&available,nullptr))throw std::runtime_error("Discord pipe disconnected");
        if(!available)return {};
        std::string out(std::min<DWORD>(available,65536),'\0');
        out.resize(transfer(false,out.data(),static_cast<DWORD>(out.size())));return out;
    }
private:
    DWORD transfer(bool write,char* data,DWORD size) {
        OVERLAPPED operation{};operation.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
        if(!operation.hEvent)throw std::runtime_error("Discord pipe event failed");
        DWORD done=0;BOOL ok=write?WriteFile(pipe_,data,size,&done,&operation):ReadFile(pipe_,data,size,&done,&operation);
        if(!ok&&GetLastError()==ERROR_IO_PENDING) {
            if(WaitForSingleObject(operation.hEvent,500)!=WAIT_OBJECT_0) {
                CancelIoEx(pipe_,&operation);
                GetOverlappedResult(pipe_,&operation,&done,TRUE); // Drain before stack OVERLAPPED/event can disappear.
                CloseHandle(operation.hEvent);throw std::runtime_error("Discord pipe timeout");
            }
            ok=GetOverlappedResult(pipe_,&operation,&done,FALSE);
        }
        CloseHandle(operation.hEvent);
        if(!ok||!done)throw std::runtime_error("Discord pipe disconnected");return done;
    }
    HANDLE pipe_;
};
}
std::unique_ptr<DiscordTransport> connect_discord_at(const std::string& endpoint) {
    if(!endpoint.starts_with("\\\\.\\pipe\\"))throw std::runtime_error("Invalid Discord pipe endpoint");
    const std::wstring path(endpoint.begin(),endpoint.end());
    HANDLE handle=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
    if(handle==INVALID_HANDLE_VALUE)throw std::runtime_error("Waiting for Discord desktop app");
    return std::make_unique<PipeTransport>(handle);
}
std::unique_ptr<DiscordTransport> connect_discord() {
    for(unsigned i=0;i<10;++i) {
        try{return connect_discord_at("\\\\.\\pipe\\discord-ipc-"+std::to_string(i));}catch(const std::exception&){}
    }
    throw std::runtime_error("Waiting for Discord desktop app");
}
std::uint64_t process_id(){return GetCurrentProcessId();}
}
#endif
