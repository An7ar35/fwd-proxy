#include "Client.h"

#include <iostream>

#include <unistd.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>

#define EPOLL_ARRAY_SIZE            10
#define EPOLL_PENDING_QUEUE_LENGTH  10 //size is ignored since Linux 2.6.8
#define INPUT_BUFFER_SIZE          512

using namespace fwd_proxy::client;

/**
 * Constructor
 * @param address Server address
 * @param port Port
 * @param timeout_s Connection timeout in seconds (default = 30s)
 */
Client::Client( std::string address, int port, int timeout_s ) :
    _timeout( timeout_s ),
    _address( std::move( address ) ),
    _port( std::to_string( port ) ),
    _security( SecurityType::UNSECURED ),
    _run_flag( false ),
    _connection_state( HandshakeState::INIT ),
    _socket_fd( -1 ),
    _epoll_fd( -1 ),
    _unblock_event_fd( -1 )
{}

/**
 * Constructor
 * @param address Server address
 * @param port Port
 * @param secret Secret
 * @param timeout_s Connection timeout in seconds (default = 30s)
 */
Client::Client( std::string address, int port, std::string secret, int timeout_s ) :
    _timeout( timeout_s ),
    _address( std::move( address ) ),
    _port( std::to_string( port ) ),
    _secret( std::move( secret ) ),
    _security( SecurityType::SECURED ),
    _run_flag( false ),
    _connection_state( HandshakeState::INIT ),
    _socket_fd( -1 ),
    _epoll_fd( -1 ),
    _unblock_event_fd( -1 )
{}

/**
 * Destructor
 */
Client::~Client() {
    disconnect();
}

/**
 * Connect to server
 * @return Success
 */
bool Client::connect() {
    if( _run_flag ) {
        std::cerr << "[client::Client::connect()] already connected - disconnect first." << std::endl;
        return false; //EARLY RETURN
    }

    _connection_state = HandshakeState::INIT;

    int               err_val          = 0;
    struct addrinfo * server_info      = nullptr;
    struct addrinfo * curr_server_info = nullptr;
    struct addrinfo   hints {};

    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if( ( err_val = ::getaddrinfo( _address.c_str(), _port.c_str(), &hints, &server_info ) ) != 0 ) {
        std::cerr << "[client::Client::connect()] getaddrinfo: " << ::gai_strerror( err_val ) << std::endl;
        return false; //EARLY RETURN
    }

    for( curr_server_info = server_info; curr_server_info != nullptr; curr_server_info = curr_server_info->ai_next ) {
        if( ( _socket_fd = ::socket( curr_server_info->ai_family, curr_server_info->ai_socktype, curr_server_info->ai_protocol ) ) == -1 ) {
            ::perror( "[client::Client::connect()] error" );
            continue;
        }

        if( ::connect( _socket_fd, curr_server_info->ai_addr, curr_server_info->ai_addrlen ) == -1 ) {
            ::close( _socket_fd );
            ::perror( "[client::Client::connect()] error" );
            continue;
        }

        break;
    }

    if( curr_server_info == nullptr ) {
        std::cerr << "[client::Client::connect()] failed to connect to " << _address << ":" << _port << std::endl;
        return false; //EARLY RETURN
    }

    const auto & socket_addr = static_cast<struct sockaddr *>( curr_server_info->ai_addr );
    char         address[INET6_ADDRSTRLEN];

    if( socket_addr->sa_family == AF_INET ) { //IPv4
        ::inet_ntop( curr_server_info->ai_family, &((struct sockaddr_in *) socket_addr )->sin_addr, address, sizeof address );
    } else { //IPv6
        ::inet_ntop( curr_server_info->ai_family, &((struct sockaddr_in6 *) socket_addr )->sin6_addr, address, sizeof address );
    }

    ::freeaddrinfo( server_info );
    ::fcntl( _socket_fd, F_SETFL, O_NONBLOCK ); //non-blocking so we can 'poll'

    if( ( _epoll_fd = epoll_create( EPOLL_PENDING_QUEUE_LENGTH ) ) == -1 ) {
        std::cerr << "[client::Client::connect()] Failed to create epoll file descriptor." << std::endl;
        goto failed;
    }

    if( ( _unblock_event_fd = ::eventfd( 0, EFD_NONBLOCK ) ) == -1 ) {
        std::cerr << "[client::Client::connect()] Failed to create 'event unblocking' epoll file descriptor." << std::endl;
        goto failed;
    }

    if( !Client::modifyEPOLL( _epoll_fd, _unblock_event_fd, EPOLL_CTL_ADD, EPOLLIN ) ||
        !Client::modifyEPOLL( _epoll_fd, _socket_fd, EPOLL_CTL_ADD, EPOLLIN ) )
    {
        goto failed;
    }

    if( _security == SecurityType::SECURED ) {
        Client::send( _socket_fd, "AUTH1" + _secret );
        _connection_state = HandshakeState::AUTH1;
    } else {
        Client::send( _socket_fd, "AUTH0" );
        _connection_state = HandshakeState::AUTH0;
    }

    std::cout << "connected to <" << address << ">" << std::endl;

    if( !waitForReadyState( _timeout ) ) {
        std::cout << "Pairing to another client timed out." << std::endl;
        goto failed;
    }

    _connection_state = HandshakeState::READY;
    _run_flag         = true;
    _io_worker_th     = std::thread( [ this ]() { runEventLoop(); } );

    return _run_flag;

    failed: {
        closeFileDescriptors();
        return false;
    };
}


/**
 * Send a string to the server (buffered)
 * @param str String
 */
void Client::send( const std::string &str ) {
    if( _run_flag ) {
        std::lock_guard<std::mutex> guard( _out_buffer_mutex );
        _out_buffer.insert( _out_buffer.end(), str.begin(), str.end() );
        Client::signalEvent( _unblock_event_fd );
    }
}

/**
 * Disconnect connection
 * @return Error-less success
 */
bool Client::disconnect() {
    if( _run_flag ) {
        std::cout << "[client::Client::disconnect()] disconnecting..." << std::endl;

        _run_flag         = false;
        _connection_state = HandshakeState::DCN;

        Client::signalEvent( _unblock_event_fd );

        _io_worker_th.join();
        ::shutdown( _socket_fd, SHUT_WR );
        closeFileDescriptors();

        return true;
    }

    return false;
}

/**
 * [PRIVATE] Run the client event loop
 */
void Client::runEventLoop() {
    std::cout << "Ready for input..." << std::endl;

    while( _run_flag ) {
        struct epoll_event event_buff[EPOLL_ARRAY_SIZE];
        char               in_buffer [INPUT_BUFFER_SIZE];

        int event_count = epoll_wait( _epoll_fd, event_buff, EPOLL_ARRAY_SIZE, -1 );

        for( int i = 0; i < event_count; ++i ) { //IN
            if( event_buff[i].data.fd == _unblock_event_fd ) {
                continue; //skip
            }

            auto in_bytes = ::recv( event_buff[i].data.fd, in_buffer, ( INPUT_BUFFER_SIZE - 1 ), 0 );

            if( in_bytes > 0 ) {
                std::cout << "[client::Client::runEventLoop()] "
                          << "(" << _connection_state << ") received: " << std::string( in_buffer, in_bytes )
                          << std::endl;
            }
        }

        { //OUT //TODO maybe dump that into separate send worker thread?
            std::lock_guard<std::mutex> guard( _out_buffer_mutex );

            if( !_out_buffer.empty() ) {
                auto out_bytes = ::send( _socket_fd, &_out_buffer[0], _out_buffer.size(), 0 );

                if( out_bytes == -1 ) {
                    ::perror( "[client::Client::runEventLoop()] error" );
                } else {
                    _out_buffer.erase( _out_buffer.begin(), _out_buffer.begin() + out_bytes );
                }
            }
        }
    }

    std::cout << "Exiting runEventLoop()..." << std::endl;
}

/**
 * [PRIVATE] Closes any opened private file descriptor
 */
void Client::closeFileDescriptors() {
    if( _unblock_event_fd != -1 ) {
        ::close( _unblock_event_fd );
    }

    if( _socket_fd != -1 ) {
        ::close( _epoll_fd );
    }

    if( _epoll_fd != -1 ) {
        ::close( _epoll_fd );
    }
}

/**
 * [PRIVATE] Receive bytes from the
 * @param epoll_fd   File descriptor of epoll
 * @param buffer     Character buffer
 * @param buffer_len Buffer size
 * @param timeout_s  Timeout in seconds
 * @return Number of bytes received (0 on timeout/error)
 */
size_t Client::rcv( int epoll_fd, char * buffer, int buffer_len, int timeout_s ) const {
    struct epoll_event event_buff[EPOLL_ARRAY_SIZE];

    int event_count = epoll_wait( epoll_fd, event_buff, buffer_len, ( timeout_s * 1000 ) ); //TODO switch to a thrread interupt function?

    for( auto i = 0; i < event_count; ++i ) {
        if( event_buff[i].data.fd == _socket_fd ) {
            if( event_count > 0 ) {
                return ::recv( event_buff[ 0 ].data.fd, buffer, buffer_len, 0 ); //EARLY RETURN
            }
        }
    }

    std::cout << "[client::Client::rcv(..)] Timeout (" << timeout_s << ")" << std::endl;
    return 0;
}

/**
 * [PRIVATE] Sends unbuffered message directly
 * @param socket_fd Socket file descriptor
 * @param msg Message string
 * @return Bytes sent
 */
ssize_t Client::send( Client::FileDescriptor_t socket_fd, const std::string &msg ) {
    auto out_bytes = ::send( socket_fd, &msg[0], msg.size(), 0 );

    if( out_bytes == -1 ) {
        ::perror( "[client::Client::runEventLoop()] error" );
    }

    return out_bytes;
}

/**
 * [PRIVATE] Signal an event to unblock `epoll_wait`
 * @param event_fd Event file descriptor
 */
void Client::signalEvent( Client::FileDescriptor_t event_fd ) {
    const uint64_t one = 1;

    if( ::write( event_fd, &one, sizeof( uint64_t ) ) != sizeof( uint64_t ) ) {
        ::perror( "[client::Client::signalEvent()] error" );
    }
}

/**
 * [PRIVATE] Modifies epoll file descriptor (wrapper for `epoll_ctl`)
 * @param epoll_fd Target epoll file descriptor
 * @param fd File descriptor to modify inside the epoll
 * @param operation Operation
 * @param event_flags Flags to set in the event
 * @return Success
 */
bool Client::modifyEPOLL( Client::FileDescriptor_t epoll_fd, Client::FileDescriptor_t fd, int operation, uint32_t event_flags ) {
    struct epoll_event event   = {};

    event.events  = event_flags;
    event.data.fd = fd;

    if( ::epoll_ctl( epoll_fd, operation, fd, &event ) < 0 ) {
        std::cerr << "[client::Client::modifyEPOLL( " << epoll_fd << ", " << fd << ", " << operation << ", " << event_flags << " )] "
                  << "Failed to modify epoll."
                  << std::endl;

        return false;
    }

    return true;
}

/**
 * [PRIVATE] Waits for the server to return a READY state
 * @param timeout_s Timeout in seconds
 * @return Success
 */
bool Client::waitForReadyState( int timeout_s ) {
    struct epoll_event  event_buff[EPOLL_ARRAY_SIZE];
    static const size_t BUFFER_SIZE = 5;
    char                in_buffer[BUFFER_SIZE];
    bool                ready_flag = false;

    const int event_count = epoll_wait( _epoll_fd, event_buff, BUFFER_SIZE, ( timeout_s * 1000 ) );

    for( int i = 0; i < event_count; ++i ) { //IN
        if( event_buff[i].data.fd == _unblock_event_fd ) {
            continue; //skip
        }

        auto in_bytes = ::recv( event_buff[i].data.fd, in_buffer, BUFFER_SIZE, 0 );

        if( in_bytes > 0 ) {
            if( std::string( in_buffer, 5 ) == "READY" ) {
                ready_flag = true;
            } //else: drop

        } else if( in_bytes < 0 ) {
            ::perror( "[client::Client::waitForReadyState()] error" );
        }
    }

    return ready_flag;
}
