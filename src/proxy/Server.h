#ifndef FWD_PROXY_PROXY_SERVER_H
#define FWD_PROXY_PROXY_SERVER_H

#include <string>
#include <vector>
#include <deque>
#include <unordered_map>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>

#include "../enum/HandshakeState.h"

namespace fwd_proxy::proxy {
    class Server {
      public:
        explicit Server( int port );
        ~Server();

        bool start();
        bool stop();

      private:
        typedef std::string Secret_t;
        typedef int         FileDescriptor_t;

        const std::string _server_port;
        FileDescriptor_t  _server_socket_fd;
        FileDescriptor_t  _server_socket_epoll_fd;
        FileDescriptor_t  _unblock_event_fd;
        std::atomic_bool  _run_flag;
        std::thread       _connection_worker_th;
        std::thread       _pending_worker_th;
        std::thread       _proxy_worker_th;

        FileDescriptor_t                                       _epoll_pending_fd;
        FileDescriptor_t                                       _epoll_paired_fd;
        std::mutex                                             _pairings_mutex; //use for both `_epoll_paired_fd` and `_pairings`
        std::unordered_map<FileDescriptor_t, FileDescriptor_t> _pairings;

        void closeFileDescriptors();

        void runConnectionEventLoop();
        void runPendingEventLoop();
        void runProxyEventLoop();

        static HandshakeState processHandshake( FileDescriptor_t client_fd, HandshakeState cxn_state, Secret_t & secret );
        static bool send( FileDescriptor_t client_fd, const std::string & msg );
        static ssize_t rcv( FileDescriptor_t client_fd, char * buffer, size_t buffer_size );
        static size_t rcvUntil( FileDescriptor_t client_fd, char * buffer, size_t buffer_size, std::function<int( int )> predicate_fn ) ;
        static bool modifyEPOLL( FileDescriptor_t epoll_fd, FileDescriptor_t fd, int operation, uint32_t  event_flags );
    };
}

#endif //FWD_PROXY_PROXY_SERVER_H