#include "Server.h"

#include <iostream>
#include <set>

#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#define EPOLL_PENDING_QUEUE_LENGTH  10 //size is ignored since Linux 2.6.8
#define EPOLL_ARRAY_SIZE            10
#define MAX_CONNECTION_REQUESTS    100
#define INPUT_BUFFER_SIZE          512

using namespace fwd_proxy::proxy;

/**
 * Constructor
 * @param port Port
 */
Server::Server( int port ) :
    _server_port( std::to_string( port ) ),
    _server_socket_fd( -1 ),
    _server_socket_epoll_fd( -1 ),
    _epoll_pending_fd( -1 ),
    _epoll_paired_fd( -1 ),
    _run_flag( true ),
    _unblock_event_fd( -1 )
{}

/**
 * Destructor
 */
Server::~Server() {
    stop();
}

/**
 * Starts the server
 * @return Success
 */
bool Server::start() {
    std::cout << "[proxy::Server::start()] Staring server on port " << _server_port << "..." << std::endl;

    struct addrinfo   hints {};
    struct addrinfo * server_info;
    struct addrinfo * curr_server_info;

    int yes     { 1 };
    int err_val { 0 };

    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_PASSIVE;

    if( ( err_val = ::getaddrinfo( nullptr, _server_port.c_str(), &hints, &server_info ) ) != 0 ) {
        std::cerr << "[proxy::Server::start()] " << ::gai_strerror( err_val ) << std::endl;
        return false; //EARLY RETURN
    }

    for( curr_server_info = server_info; curr_server_info != nullptr; curr_server_info = curr_server_info->ai_next ) {
        if( ( _server_socket_fd = ::socket( curr_server_info->ai_family, curr_server_info->ai_socktype, curr_server_info->ai_protocol ) ) == -1 ) {
            ::perror("[proxy::Server::start()] 'socket' error");
            continue;
        }

        ::fcntl( _server_socket_fd, F_SETFL, O_NONBLOCK ); //non-blocking so we can 'poll'

        if( ::setsockopt( _server_socket_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int) ) == -1 ) {
            ::perror("[proxy::Server::start()] 'setsockopt' error");
            ::freeaddrinfo( server_info );
            return false; //EARLY RETURN
        }

        if( ::bind( _server_socket_fd, curr_server_info->ai_addr, curr_server_info->ai_addrlen ) == -1 ) {
            ::close( _server_socket_fd );
            ::perror("[proxy::Server::start()] 'bind' error");
            continue;
        }

        break;
    }

    if( curr_server_info == nullptr ) {
        std::cerr << "[proxy::Server::start()] Failed to bind." << std::endl;
        ::freeaddrinfo( server_info );
        return false; //EARLY RETURN
    }

    ::freeaddrinfo( server_info );

    if( ( _epoll_pending_fd = ::epoll_create( EPOLL_PENDING_QUEUE_LENGTH ) ) == -1 ) {
        std::cerr << "[proxy::Server::start()] Failed to create 'pending clients' epoll file descriptor." << std::endl;
        closeFileDescriptors();
        return false; //EARLY RETURN
    }

    if( ( _epoll_paired_fd = ::epoll_create( EPOLL_PENDING_QUEUE_LENGTH ) ) == -1 ) {
        std::cerr << "[proxy::Server::start()] Failed to create 'paired clients' epoll file descriptor." << std::endl;
        closeFileDescriptors();
        return false; //EARLY RETURN
    }

    if( ( _server_socket_epoll_fd = ::epoll_create( 2 ) ) == -1 ) {
        std::cerr << "[proxy::Server::start()] Failed to create 'server socket' epoll file descriptor." << std::endl;
        closeFileDescriptors();
        return false; //EARLY RETURN
    }

    if( ( _unblock_event_fd = ::eventfd( 0, EFD_NONBLOCK ) ) == -1 ) {
        std::cerr << "[proxy::Server::start()] Failed to create 'event unblocking' epoll file descriptor." << std::endl;
        closeFileDescriptors();
        return false; //EARLY RETURN
    }

    if( !Server::modifyEPOLL( _server_socket_epoll_fd, _unblock_event_fd, EPOLL_CTL_ADD, EPOLLIN )           ||
        !Server::modifyEPOLL( _server_socket_epoll_fd, _server_socket_fd, EPOLL_CTL_ADD, EPOLLIN | EPOLLET ) ||
        !Server::modifyEPOLL( _epoll_pending_fd, _unblock_event_fd, EPOLL_CTL_ADD, EPOLLIN )                 ||
        !Server::modifyEPOLL( _epoll_paired_fd, _unblock_event_fd, EPOLL_CTL_ADD, EPOLLIN ) )
    {
        closeFileDescriptors();
        return false; //EARLY RETURN
    }

    if( ::listen( _server_socket_fd, MAX_CONNECTION_REQUESTS ) == -1 ) {
        ::perror( "[proxy::Server::start()] error" );
        closeFileDescriptors();
        return false; //EARLY RETURN
    }

    _connection_worker_th = std::thread( [this]() { this->runConnectionEventLoop(); } );
    _pending_worker_th    = std::thread( [this]() { this->runPendingEventLoop(); } );
    _proxy_worker_th      = std::thread( [this]() { this->runProxyEventLoop(); } );

    return true;
}

/**
 * Stops the server
 * @return Error-less success
 */
bool Server::stop() {
    if( _run_flag ) {
        std::cout << "[proxy::Server::stop()] Shutting down server..." << std::endl;
        _run_flag = false;

        { //unblock any `epoll_wait`
            const uint64_t one = 1;

            if( ::write( _unblock_event_fd, &one, sizeof( uint64_t ) ) != sizeof( uint64_t ) ) {
                ::perror( "[proxy::Server::stop()] error" );
            }
        }

        _connection_worker_th.join();
        _pending_worker_th.join();
        _proxy_worker_th.join();

        std::cout << "[proxy::Server::stop()] paired clients = " << _pairings.size() << std::endl;

        closeFileDescriptors();
    }

    return true;
}

/**
 * [PRIVATE] Closes any opened private file descriptor
 */
void Server::closeFileDescriptors() {
    if( _epoll_pending_fd != -1 ) {
        ::close( _epoll_pending_fd );
    }

    if( _epoll_paired_fd != -1 ) {
        ::close( _epoll_paired_fd );
    }

    if( _server_socket_epoll_fd != -1 ) {
        ::close( _server_socket_epoll_fd );
    }

    if( _unblock_event_fd != -1 ) {
        ::close( _unblock_event_fd );
    }

    if( _server_socket_fd != -1 ) {
        ::close( _server_socket_fd );
    }
}

/**
 * [PRIVATE] Listens for new clients trying to connect
 */
void Server::runConnectionEventLoop() {
    std::cout << "[proxy::Server::runConnectionEventLoop()] Waiting for connections..." << std::endl;

    char                    address[INET6_ADDRSTRLEN];
    struct sockaddr_storage client_socket_addr      = {};
    socklen_t               client_socket_addr_size = sizeof client_socket_addr;

    while( _run_flag ) {
        struct epoll_event event_buff[EPOLL_ARRAY_SIZE];

        int event_count = epoll_wait( _server_socket_epoll_fd, event_buff, EPOLL_ARRAY_SIZE, -1 );

        for( int i = 0; i < event_count && event_buff[i].data.fd != _unblock_event_fd; ++i ) {
            FileDescriptor_t client_fd = ::accept( _server_socket_fd, ( struct sockaddr * ) &client_socket_addr, &client_socket_addr_size );

            if( client_fd == -1 ) {
                ::perror( "[proxy::Server::runConnectionEventLoop()] error" );
                continue;
            }

            const auto & socket_addr = (struct sockaddr *) &client_socket_addr;

            if( socket_addr->sa_family == AF_INET ) { //IPv4
                ::inet_ntop( client_socket_addr.ss_family, &((struct sockaddr_in *) socket_addr )->sin_addr, address, sizeof address );
            } else { //IPv6
                ::inet_ntop( client_socket_addr.ss_family, &((struct sockaddr_in6 *) socket_addr )->sin6_addr, address, sizeof address );
            }

            std::cout << "[proxy::Server::runConnectionEventLoop()] New client " << address << std::endl;

            Server::modifyEPOLL( _epoll_pending_fd, client_fd, EPOLL_CTL_ADD, EPOLLIN );

            ::fcntl( client_fd, F_SETFL, O_NONBLOCK ); //non-blocking so we can 'poll'
        }
    }

    std::cout << "Exiting runConnectionEventLoop()" << std::endl;
}

/**
 * [PRIVATE] Processes new and pending clients to pair them when possible
 */
void Server::runPendingEventLoop() {
    std::unordered_map<FileDescriptor_t, HandshakeState>     negotiations;
    std::unordered_map<FileDescriptor_t, Secret_t>           fd_to_secret;
    std::unordered_map<Secret_t, std::set<FileDescriptor_t>> ready;

    while( _run_flag ) {
        struct epoll_event event_buff[EPOLL_ARRAY_SIZE];

        int event_count = epoll_wait( _epoll_pending_fd, event_buff, EPOLL_ARRAY_SIZE, -1 );

        for( int i = 0; i < event_count && event_buff[i].data.fd != _unblock_event_fd; ++i ) {
            if( event_buff[i].data.fd == _unblock_event_fd ) {
                continue; //skip
            }

            FileDescriptor_t client_fd = event_buff[i].data.fd;

            if( !negotiations.contains( client_fd ) ) {
                negotiations.emplace( client_fd, HandshakeState::INIT );
            }

            auto       secret               = std::string();
            auto       negotiation_entry_it = negotiations.find( client_fd );
            auto &     prev_handshake_state = negotiation_entry_it->second;
            const auto new_handshake_state  = processHandshake( client_fd, prev_handshake_state, secret );

            switch( new_handshake_state ) {
                case HandshakeState::READY: {
                    auto ready_it = ready.find( secret );

                    if( ready_it != ready.end() && !ready_it->second.empty() ) {
                        auto pairing_candidate_it = ready_it->second.begin();

                        Server::modifyEPOLL( _epoll_pending_fd, client_fd, EPOLL_CTL_DEL, EPOLLIN );
                        Server::modifyEPOLL( _epoll_pending_fd, *pairing_candidate_it, EPOLL_CTL_DEL, EPOLLIN );

                        { //move client pairing to main proxy loop
                            std::lock_guard<std::mutex> guard( _pairings_mutex );

                            _pairings.emplace( client_fd, *pairing_candidate_it );
                            _pairings.emplace( *pairing_candidate_it, client_fd );

                            Server::modifyEPOLL( _epoll_paired_fd, client_fd, EPOLL_CTL_ADD, EPOLLIN );
                            Server::modifyEPOLL( _epoll_paired_fd, *pairing_candidate_it, EPOLL_CTL_ADD, EPOLLIN );
                        }

                        Server::send( client_fd, "READY" );
                        Server::send( *pairing_candidate_it, "READY" );

                        std::cout << "[proxy::Server::runPendingEventLoop()] "
                                  << "Client pairing created: " << client_fd << " <-> " << *pairing_candidate_it
                                  << std::endl;

                        //cleanup
                        negotiations.erase( negotiation_entry_it );
                        negotiations.erase( *pairing_candidate_it );
                        fd_to_secret.erase( *pairing_candidate_it );
                        ready_it->second.erase( pairing_candidate_it );

                    } else {
                        auto [it, inserted] = ready.try_emplace( secret, std::set<FileDescriptor_t>() );

                        if( it != ready.end() ) {
                            it->second.emplace( client_fd );
                            fd_to_secret.emplace( client_fd, secret );

                        } else {
                            std::cerr << "[proxy::Server::runPendingEventLoop()] "
                                      << "Failed to add client " << client_fd << " to 'ready' collection."
                                      << std::endl;
                        }
                    }
                } break;

                case HandshakeState::DCN: {
                    if( !Server::modifyEPOLL( _epoll_pending_fd, client_fd, EPOLL_CTL_DEL, EPOLLIN ) ) {
                        std::cerr << "[proxy::Server::runPendingEventLoop()] "
                                  << "Failed to remove client file descriptor from pending epoll: " << client_fd
                                  << std::endl;

                    } else if( prev_handshake_state == HandshakeState::READY ) {
                        std::cout << "[proxy::Server::processHandshake(..)] "
                                  << "Client " << client_fd << " disconnected"
                                  << std::endl;

                        auto it = fd_to_secret.find( client_fd );

                        if( it != fd_to_secret.end() ) {
                            auto ready_it = ready.find( it->second );

                            if( ready_it != ready.end() ) {
                                ready_it->second.erase( client_fd );
                            }

                            if( ready_it->second.empty() ) {
                                ready.erase( ready_it );
                            }
                        }

                        fd_to_secret.erase( it );
                        negotiations.erase( client_fd );
                    }
                } break;

                default: break; //i.e.: pending handshake completion
            }

            prev_handshake_state = new_handshake_state;
        }
    }

    std::cout << "Exiting runPendingEventLoop()" << std::endl;
}

/**
 * [PRIVATE] Runs the proxy event loop (message forwarding)
 */
void Server::runProxyEventLoop() {
    while( _run_flag ) {
        char               in_buffer [INPUT_BUFFER_SIZE];
        struct epoll_event event_buff[EPOLL_ARRAY_SIZE];
        FileDescriptor_t   counterpart_fd = -1;

        int event_count = epoll_wait( _epoll_paired_fd, event_buff, EPOLL_ARRAY_SIZE, -1 );

        for( int i = 0; i < event_count; ++i ) {
            if( event_buff[i].data.fd == _unblock_event_fd ) {
                continue; //skip
            }

            {
                std::lock_guard<std::mutex> guard( _pairings_mutex );
                counterpart_fd = _pairings.at( event_buff[ i ].data.fd ); //unsafe but should be there
            }

            auto in_bytes = ::recv( event_buff[i].data.fd, in_buffer, ( INPUT_BUFFER_SIZE - 1 ), 0 );

            if( in_bytes > 0 ) {
                std::cout << "[proxy::Server::runProxyEventLoop()] "
                          << event_buff[i].data.fd << " -> " << counterpart_fd << ": "
                          << std::string( in_buffer, in_bytes )
                          << std::endl;

                if( ::send( counterpart_fd, in_buffer, in_bytes, 0 ) == -1 ) {
                    ::perror( "[proxy::Server::runProxyEventLoop()] error" );
                }

            } else if( in_bytes == 0 ) {
                std::cout << "[proxy::Server::runProxyEventLoop(..)] "
                          << "Client " << event_buff[i].data.fd << " disconnected"
                          << std::endl;

                send( counterpart_fd, "DISCONNECTED" );
                _pairings.erase( event_buff[i].data.fd );
                _pairings.erase( counterpart_fd );
                ::close( event_buff[i].data.fd );
                ::close( counterpart_fd );

                std::cout << "[proxy::Server::runProxyEventLoop(..)] "
                          << "Disconnected client" << counterpart_fd
                          << std::endl;

            } else {
                ::perror( "[proxy::Server::runProxyEventLoop()] error" );
            }
        }
    }

    std::cout << "Exiting runProxyEventLoop()" << std::endl;
}

/**
 * [PRIVATE] Process connection handshake for a client
 * @param client_fd Client file descriptor
 * @param cxn_state Client connection handshake state
 * @param secret    Reference to string to store secret into
 * @return Handshake state post-processing
 */
fwd_proxy::HandshakeState Server::processHandshake( FileDescriptor_t client_fd, HandshakeState cxn_state, Secret_t & secret  ) {
    auto new_cxn_state = cxn_state;

    switch( cxn_state ) {
        case HandshakeState::INIT: {
            static const size_t AUTH_MSG_LEN = 5;

            char buffer[AUTH_MSG_LEN];
            auto bytes = Server::rcv( client_fd, buffer, AUTH_MSG_LEN );

            if( bytes == 5 ) {
                const auto str = std::string( buffer, bytes );

                if( str == "AUTH0" ) {
                    new_cxn_state = HandshakeState::READY;

                } else if( str == "AUTH1" ) {
                    new_cxn_state = HandshakeState::AUTH1;

                } else {
                    std::cerr << "[proxy::Server::processHandshake(..)] "
                              << "Unexpected AUTH bytes sent from client " << client_fd << ": " << str
                              << std::endl;
                    Server::send( client_fd, "WTF?" );
                    new_cxn_state = HandshakeState::DCN;
                }

            } else if( bytes == 0 ) {
                std::cout << "[proxy::Server::processHandshake(..)] "
                          << "Client " << client_fd << " disconnected"
                          << std::endl;
                new_cxn_state = HandshakeState::DCN;

            } else {
                std::cerr << "[proxy::Server::processHandshake(..)] "
                          << "Unexpected content (" << bytes << " bytes) sent from client " << client_fd << ": "
                          << std::string( buffer, bytes )
                          << std::endl;
                Server::send( client_fd, "WTF?" );
                new_cxn_state = HandshakeState::DCN;
            }
        } break;

        case HandshakeState::AUTH1: { //connection with secret
            static const size_t SECRET_MAX_LEN = 64;

            char buffer[SECRET_MAX_LEN];
            auto bytes = Server::rcvUntil( client_fd, buffer, SECRET_MAX_LEN, isspace );

            if( bytes > 0 ) {
                secret = std::string( buffer, bytes );
                std::cout << "[proxy::Server::processHandshake(..)] "
                          << "Client " << client_fd << " secret: " << secret
                          << std::endl;
                new_cxn_state = HandshakeState::READY;

            } else {
                std::cout << "[proxy::Server::processHandshake(..)] "
                          << "Client " << client_fd << " disconnected"
                          << std::endl;
                new_cxn_state = HandshakeState::DCN;
            }
        } break;

        case HandshakeState::READY: {
            char buffer[INPUT_BUFFER_SIZE];
            auto bytes = Server::rcv( client_fd, buffer, INPUT_BUFFER_SIZE );

            if( bytes == 0 ) {
                new_cxn_state = HandshakeState::DCN;
            } //else: drop
        } break;

        default: {
            new_cxn_state = HandshakeState::READY;
        } break;
    }

    if( new_cxn_state != cxn_state ) {
        std::cout << "[proxy::Server::processHandshake(..)] Client " << client_fd << " handshake state: " << new_cxn_state << std::endl;
    }

    return new_cxn_state;
}


/**
 * [PRIVATE] Sends a message to a client file descriptor
 * @param client_fd Client file descriptor
 * @param msg Message string to send
 * @return Success
 */
bool Server::send( FileDescriptor_t client_fd, const std::string &msg ) {
    if( ::send( client_fd, msg.c_str(), msg.size(), 0 ) == -1 ) {
        ::perror( "[proxy::Server::send(..)] error" );
        return false;
    }

    return true;
}

/**
 * [PRIVATE] Receive characters from stream
 * @param client_fd Client file descriptor
 * @param buffer Buffer
 * @param buffer_size Buffer length
 * @return Number of bytes
 */
ssize_t Server::rcv( FileDescriptor_t client_fd, char * buffer, size_t buffer_size ) {
    auto bytes = ::recv( client_fd, buffer, buffer_size, 0 );

    if( bytes < 0 ) {
        ::perror( "[proxy::Server::rcv(..)] error" );
    }

    return bytes;
}

/**
 * [PRIVATE] Receive characters from stream until predicate is true
 * @param client_fd Client file descriptor
 * @param buffer Buffer
 * @param buffer_size Buffer length
 * @param predicate_fn Predicate function the stops when true
 * @return Number of bytes fetched before reaching end of buffer or byte covered by predicate (byte is dropped in that case)
 */
size_t Server::rcvUntil( Server::FileDescriptor_t client_fd, char * buffer, size_t buffer_size, std::function<int( int )> predicate_fn ) {
    size_t bytes = 0;

    while( bytes < buffer_size && read( client_fd, &buffer[bytes], 1 ) == 1 ) {
        if( predicate_fn( buffer[bytes] ) ) {
            break;
        }

        ++bytes;
    }

    return bytes;
}

/**
 * [PRIVATE] Modifies epoll file descriptor (wrapper for `epoll_ctl`)
 * @param epoll_fd Target epoll file descriptor
 * @param fd File descriptor to modify inside the epoll
 * @param operation Operation
 * @param event_flags Flags to set in the event
 * @return Success
 */
bool Server::modifyEPOLL( Server::FileDescriptor_t epoll_fd, Server::FileDescriptor_t fd, int operation, uint32_t event_flags ) {
    struct epoll_event event   = {};

    event.events  = event_flags;
    event.data.fd = fd;

    if( ::epoll_ctl( epoll_fd, operation, fd, &event ) < 0 ) {
        std::cerr << "[proxy::Server::modifyEPOLL( " << epoll_fd << ", " << fd << ", " << operation << ", " << event_flags << " )] "
                  << "Failed to modify epoll."
                  << std::endl;

        return false;
    }

    return true;
}
