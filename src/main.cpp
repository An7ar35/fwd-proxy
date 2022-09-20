#include <iostream>
#include <string>
#include <algorithm>
#include <csignal>
#include <getopt.h>

#include "enum/AppMode.h"
#include "enum/SecurityType.h"
#include "client/Client.h"
#include "proxy/Server.h"

#define DEFAULT_PORT   9595
#define DEFAULT_ADDR   "127.0.0.1"
#define CLIENT_TIMEOUT 10

void printHelp();
void handleClientInput();
void handleServerInput();
void handleSignal( int signo, siginfo_t * info, void * context );

namespace fwd_proxy {
    std::unique_ptr<client::Client> client_instance;
    std::unique_ptr<proxy::Server>  server_instance;
}

int main( int argc, char **argv ) {
    using namespace fwd_proxy;

    //Setup interrupt and terminate signal handling
    static struct sigaction signal_handler {};

    signal_handler.sa_sigaction = handleSignal;
    signal_handler.sa_flags     = SA_SIGINFO;

    sigaction( SIGINT, &signal_handler, NULL );
    sigaction( SIGTERM, &signal_handler, NULL );

    //Process CLI arguments
    const static struct option long_options[] = {
        {"mode",   required_argument, nullptr, 'm'},
        {"secret", required_argument, nullptr, 's'},
    };

    if( argc < 2 ) {
        printHelp();
        exit( EXIT_FAILURE );
    }

    bool    error        = false;
    int     option       = -1;
    int     option_index =  0;
    AppMode app_mode     = AppMode::UNDEFINED;
    auto    security     = SecurityType::UNSECURED;
    auto    secret       = std::string();
    int     port         = DEFAULT_PORT;

    while( ( option = getopt_long( argc, argv, "m:s:", long_options, &option_index) ) != -1 ) {
        switch( option ) {
            case 'm': {
                auto mode = std::string( optarg );
                std::for_each( mode.begin(), mode.end(), tolower );

                if( mode == "proxy" || mode == "server" ) {
                    app_mode = AppMode::PROXY;
                } else if( mode == "client") {
                    app_mode = AppMode::CLIENT;
                }
            } break;

            case 's': {
                secret   = std::string( optarg );
                security = SecurityType::SECURED;
            } break;

            case '?': [[fallthrough]];
            default: {
                error = true;
                printHelp();
            } break;
        }

        option_index = 0;
    }

    if( error ) {
        exit( EXIT_FAILURE );
    }

    std::cout << "Mode  : " << app_mode << "\n"
              << "Secret: " << secret << "\n"
              << "Port  : " << port << std::endl;

    //Get started...
    switch( app_mode ) {
        case AppMode::UNDEFINED: {
            std::cerr << "Error: application mode (server/client) not defined!" << std::endl;
            exit( EXIT_FAILURE );
        } break;

        case AppMode::CLIENT: {
            if( security == SecurityType::SECURED ) {
                client_instance = std::make_unique<client::Client>( DEFAULT_ADDR, port, secret, CLIENT_TIMEOUT );

                if( client_instance->connect() ) {
                    handleClientInput();
                }

            } else {
                client_instance = std::make_unique<client::Client>( DEFAULT_ADDR, port, CLIENT_TIMEOUT );

                if( client_instance->connect() ) {
                    handleClientInput();
                }
            }
        } break;

        case AppMode::PROXY: {
            server_instance = std::make_unique<proxy::Server>( port );

            if( server_instance->start() ) {
                handleServerInput();
            }
        } break;
    }

    return 0;
}

/**
 * Prints CLI help
 */
void printHelp() {
    std::cout << "Usage:\n"
              << "  -m, --mode <mode>       Set the mode (server/client)\n"
              << "  -s, --secret <secret>   Set the secret (optional - client only)\n"
              << std::endl;
}

/**
 * Sends input from the console to the client 'send' buffer
 */
void handleClientInput() {
    std::cout << "Press CTRL+C to exit." << std::endl;

    while( true ) {
        std::string str;
        std::cin >> str;
        fwd_proxy::client_instance->send( str );
    }
}

/**
 * Watch for console key input
 */
void handleServerInput() {
    std::cout << "Press 'q' to exit." << std::endl;

    char key = 0;

    while( key != 'q' ) {
        std::cin >> key;
    }
}

/**
 * Handles system signals
 * @param signo Signal to handle
 * @param info Pointer to signal information struct
 * @param context Pointer to context
 */
void handleSignal( int signo, siginfo_t * info, void * context ) {
    switch( signo ) {
        case SIGINT : [[fallthrough]]; //interrupt (^C)
        case SIGTERM: { //terminate
            exit( EXIT_SUCCESS );
        } break;

        default:
            signal( signo, SIG_DFL );
            break;
    }
}