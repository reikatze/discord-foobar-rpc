#include "test_support.h"
#include "../modules/presence.h"
#include <future>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>
#include <cstring>
#endif
namespace {
class Server {
public:
    std::string endpoint;
#ifdef _WIN32
    HANDLE handle=INVALID_HANDLE_VALUE;
    Server(){endpoint="\\\\.\\pipe\\discord-foobar-rpc-ci-"+std::to_string(process_id());std::wstring name(endpoint.begin(),endpoint.end());handle=CreateNamedPipeW(name.c_str(),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED,PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT,1,65536,65536,0,nullptr);require(handle!=INVALID_HANDLE_VALUE,"CreateNamedPipe failed");}
    ~Server(){if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);}
    void accept(){OVERLAPPED o{};o.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);require(o.hEvent!=nullptr,"CreateEvent failed");BOOL ok=ConnectNamedPipe(handle,&o);auto error=GetLastError();if(!ok&&error==ERROR_PIPE_CONNECTED)ok=TRUE;else if(!ok&&error==ERROR_IO_PENDING){if(WaitForSingleObject(o.hEvent,5000)==WAIT_OBJECT_0){DWORD n=0;ok=GetOverlappedResult(handle,&o,&n,FALSE);}else{CancelIoEx(handle,&o);DWORD n=0;GetOverlappedResult(handle,&o,&n,TRUE);}}CloseHandle(o.hEvent);require(ok,"Mock pipe connection timed out");}
    std::size_t io(bool write,char* data,std::size_t bytes){OVERLAPPED o{};o.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);require(o.hEvent!=nullptr,"Event creation failed");DWORD done=0;BOOL ok=write?WriteFile(handle,data,static_cast<DWORD>(bytes),&done,&o):ReadFile(handle,data,static_cast<DWORD>(bytes),&done,&o);if(!ok&&GetLastError()==ERROR_IO_PENDING){if(WaitForSingleObject(o.hEvent,5000)==WAIT_OBJECT_0)ok=GetOverlappedResult(handle,&o,&done,FALSE);else{CancelIoEx(handle,&o);GetOverlappedResult(handle,&o,&done,TRUE);}}CloseHandle(o.hEvent);require(ok&&done,"Mock pipe I/O failed or timed out");return done;}
#else
    TemporaryDirectory directory;int listener=-1,peer=-1;
    Server(){endpoint=(directory.path/"ipc.sock").string();sockaddr_un a{};a.sun_family=AF_UNIX;require(endpoint.size()<sizeof(a.sun_path),"Mock socket path too long");std::memcpy(a.sun_path,endpoint.c_str(),endpoint.size()+1);listener=socket(AF_UNIX,SOCK_STREAM,0);require(listener>=0,"Socket creation failed");require(bind(listener,reinterpret_cast<sockaddr*>(&a),sizeof(a))==0&&listen(listener,1)==0,"Socket bind/listen failed");}
    ~Server(){if(peer>=0)::close(peer);if(listener>=0)::close(listener);}
    void accept(){pollfd p{listener,POLLIN,0};require(poll(&p,1,5000)>0,"Mock socket connection timed out");peer=::accept(listener,nullptr,nullptr);require(peer>=0,"Accept failed");
#ifdef SO_NOSIGPIPE
        int one=1;setsockopt(peer,SOL_SOCKET,SO_NOSIGPIPE,&one,sizeof(one));
#endif
    }
    std::size_t io(bool write,char* data,std::size_t bytes){pollfd p{peer,static_cast<short>(write?POLLOUT:POLLIN),0};require(poll(&p,1,5000)>0,"Mock socket I/O timed out");auto n=write?::send(peer,data,bytes,
#ifdef MSG_NOSIGNAL
        MSG_NOSIGNAL
#else
        0
#endif
    ) : ::recv(peer,data,bytes,0);require(n>0,"Mock socket I/O disconnected");return static_cast<std::size_t>(n);}
#endif
    void write(std::string bytes){std::size_t n=0;while(n<bytes.size())n+=io(true,bytes.data()+n,bytes.size()-n);}
    std::string read(std::size_t bytes){std::string result(bytes,'\0');std::size_t n=0;while(n<bytes)n+=io(false,result.data()+n,bytes-n);return result;}
    std::pair<unsigned,json> receive(){auto header=read(8);auto size=u32(header.data()+4);require(size<1024*1024,"Mock received oversized frame");return {u32(header.data()),json::parse(read(size))};}
};
}
int main(){Suite s;
 s.test("real native transport: fragmented handshake, Listening payload, ping and clear",[]{
    Server server;std::promise<void> activity_seen;auto seen=activity_seen.get_future();std::exception_ptr server_error;
    std::thread worker([&]{try{
        server.accept();auto [opcode,handshake]=server.receive();require(opcode==0&&handshake["client_id"]=="123456789012345678","Native handshake invalid");
        auto ready=frame(1,{{"evt","READY"}});server.write(ready.substr(0,3));std::this_thread::sleep_for(std::chrono::milliseconds(10));server.write(ready.substr(3));
        auto [activity_op,value]=server.receive();require(activity_op==1&&value["cmd"]=="SET_ACTIVITY","No native SET_ACTIVITY");auto activity=value["args"]["activity"];
        require(activity["type"]==2&&activity["details"]=="Native song"&&activity["state"]=="Native artist","Native presence layout incorrect");require(activity.contains("timestamps"),"Native progress missing");require(value["args"]["pid"]==process_id(),"Wrong process ID");
        server.write(frame(1,{{"evt",nullptr},{"cmd","SET_ACTIVITY"}})+frame(3,{{"probe","native"}}));auto [pong,payload]=server.receive();require(pong==4&&payload["probe"]=="native","Native PONG invalid");activity_seen.set_value();
        auto [clear_op,clear]=server.receive();require(clear_op==1&&clear["args"]["activity"].is_null(),"Native clear missing");
    }catch(...){server_error=std::current_exception();try{activity_seen.set_exception(server_error);}catch(...){}}});
    std::exception_ptr client_error;
    try {
        DiscordRPC rpc([&]{return connect_discord_at(server.endpoint);});Playback p;p.active=true;p.track.identity="native";p.track.title="Native song";p.track.artist="Native artist";p.track.duration=180;p.position=60;p.epoch=epoch_now();PlaybackClock clock;auto activity=make_activity(p,{},clock);
        auto until=MonoClock::now()+std::chrono::seconds(5);while(seen.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready&&MonoClock::now()<until){rpc.tick("123456789012345678",activity);std::this_thread::sleep_for(std::chrono::milliseconds(10));}
        require(seen.wait_for(std::chrono::milliseconds(0))==std::future_status::ready,"Native presence timed out");seen.get();rpc.tick("123456789012345678",nullptr);rpc.shutdown();
    }catch(...){client_error=std::current_exception();}
    worker.join();if(server_error)std::rethrow_exception(server_error);if(client_error)std::rethrow_exception(client_error);
 });
 s.test("unavailable native endpoint fails without hanging",[]{auto start=MonoClock::now();throws<std::exception>([]{connect_discord_at(
#ifdef _WIN32
    "\\\\.\\pipe\\discord-foobar-rpc-nonexistent-ci"
#else
    "/tmp/discord-foobar-rpc-nonexistent-ci.sock"
#endif
 );},"Missing endpoint unexpectedly connected");require(MonoClock::now()-start<std::chrono::seconds(2),"Missing endpoint detection blocked");});
 return s.finish();}
