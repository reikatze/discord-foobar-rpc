#if defined(__APPLE__) || defined(MUSIC_TRACKER_TEST_POSIX)
#include "../modules/discord.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <stdexcept>
#include <vector>
namespace music_tracker {
namespace {
class SocketTransport final : public DiscordTransport {
public:
    explicit SocketTransport(int socket):socket_(socket){}
    ~SocketTransport() override {::close(socket_);}
    void send(const std::string& bytes) override {
        auto deadline=MonoClock::now()+std::chrono::milliseconds(500);std::size_t offset=0;
        while(offset<bytes.size()) {
            auto n=::send(socket_,bytes.data()+offset,bytes.size()-offset,
#ifdef MSG_NOSIGNAL
                MSG_NOSIGNAL
#else
                0
#endif
            );
            if(n>0){offset+=static_cast<std::size_t>(n);continue;}
            if(n<0&&errno==EINTR)continue;
            if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK)) {
                if(MonoClock::now()>=deadline)throw std::runtime_error("Discord socket timeout");
                pollfd descriptor{socket_,POLLOUT,0};::poll(&descriptor,1,20);continue;
            }
            throw std::runtime_error("Discord socket disconnected");
        }
    }
    std::string receive_available() override {
        char buffer[65536];auto n=::recv(socket_,buffer,sizeof(buffer),0);
        if(n>0)return std::string(buffer,static_cast<std::size_t>(n));
        if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR))return {};
        throw std::runtime_error("Discord socket disconnected");
    }
private:int socket_;
};
}
std::unique_ptr<DiscordTransport> connect_discord_at(const std::string& path) {
    sockaddr_un address{};address.sun_family=AF_UNIX;
    if(path.size()>=sizeof(address.sun_path))throw std::runtime_error("Discord socket path too long");
    struct stat info{};
    if(lstat(path.c_str(),&info)!=0||!S_ISSOCK(info.st_mode)||info.st_uid!=getuid())throw std::runtime_error("Discord socket unavailable");
    int socket=::socket(AF_UNIX,SOCK_STREAM,0);if(socket<0)throw std::runtime_error("Discord socket creation failed");
    if(fcntl(socket,F_SETFL,O_NONBLOCK)<0){::close(socket);throw std::runtime_error("Discord socket setup failed");}
#ifdef SO_NOSIGPIPE
    int one=1;setsockopt(socket,SOL_SOCKET,SO_NOSIGPIPE,&one,sizeof(one));
#endif
    std::memcpy(address.sun_path,path.c_str(),path.size()+1);
    int result=::connect(socket,reinterpret_cast<sockaddr*>(&address),sizeof(address));
    if(result<0&&errno==EINPROGRESS) {
        pollfd descriptor{socket,POLLOUT,0};
        if(::poll(&descriptor,1,50)>0){int error=0;socklen_t length=sizeof(error);if(getsockopt(socket,SOL_SOCKET,SO_ERROR,&error,&length)==0&&error==0)result=0;}
    }
    if(result==0)return std::make_unique<SocketTransport>(socket);
    ::close(socket);throw std::runtime_error("Discord socket connection failed");
}
std::unique_ptr<DiscordTransport> connect_discord() {
    std::vector<std::string> directories;
    for(const char* variable:{"XDG_RUNTIME_DIR","TMPDIR","TMP","TEMP"})if(const char* value=std::getenv(variable))if(*value)directories.emplace_back(value);
#ifdef __APPLE__
    char user_temp[1024]{};if(confstr(_CS_DARWIN_USER_TEMP_DIR,user_temp,sizeof(user_temp))>0&&*user_temp)directories.emplace_back(user_temp);
#endif
    directories.emplace_back("/tmp");
    auto deadline=MonoClock::now()+std::chrono::seconds(1);
    for(const auto& directory:directories)for(unsigned i=0;i<10;++i) {
        if(MonoClock::now()>deadline)throw std::runtime_error("Waiting for Discord desktop app");
        auto path=directory+(directory.ends_with('/')?"":"/")+"discord-ipc-"+std::to_string(i);
        try{return connect_discord_at(path);}catch(const std::exception&){}
    }
    throw std::runtime_error("Waiting for Discord desktop app");
}
std::uint64_t process_id(){return static_cast<std::uint64_t>(getpid());}
}
#endif
