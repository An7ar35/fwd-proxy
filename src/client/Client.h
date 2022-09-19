#ifndef FWD_PROXY_CLIENT_CLIENT_H
#define FWD_PROXY_CLIENT_CLIENT_H

#include <string>
#include <vector>
#include <thread>
#include <mutex>

#include "../enum/SecurityType.h"
#include "../enum/HandshakeState.h"

namespace fwd_proxy::client {
    class Client {
      public:
        Client( std::string address, int port, int timeout_s = 30 );
        Client( std::string address, int port, std::string secret, int timeout_s = 30 );
        ~Client();

        bool connect();
        void send( const std::string & str );
        bool disconnect();

      private:
        typedef std::string Secret_t;
        typedef int         FileDescriptor_t;

        const int          _timeout;
        const std::string  _address;
        const std::string  _port;
        const Secret_t     _secret;
        const SecurityType _security;

        std::atomic_bool   _run_flag;
        FileDescriptor_t   _unblock_event_fd;
        std::thread        _io_worker_th;
        std::mutex         _out_buffer_mutex;
        std::vector<char>  _out_buffer;
        HandshakeState     _connection_state;

        FileDescriptor_t   _socket_fd;
        FileDescriptor_t   _epoll_fd;

        void runEventLoop();

        bool waitForReadyState( int timeout_s );
        void closeFileDescriptors();
        size_t rcv( int epoll_fd, char * buffer, int buffer_len, int timeout_s ) const;


        static ssize_t send( FileDescriptor_t socket_fd, const std::string & msg );
        static void signalEvent( FileDescriptor_t event_fd );
        static bool modifyEPOLL( FileDescriptor_t epoll_fd, FileDescriptor_t fd, int operation, uint32_t  event_flags );
    };
}

#endif //FWD_PROXY_CLIENT_CLIENT_H
