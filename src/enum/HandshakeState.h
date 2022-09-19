#ifndef FWD_PROXY_PROXY_ENUM_HANDSHAKESTATE_H
#define FWD_PROXY_PROXY_ENUM_HANDSHAKESTATE_H

#include <ostream>

namespace fwd_proxy {
    enum class HandshakeState {
        INIT =  0,
        AUTH0,
        AUTH1,
        READY,
        DCN,
    };

    std::ostream & operator <<( std::ostream & os, HandshakeState stage );
}

#endif //FWD_PROXY_PROXY_ENUM_HANDSHAKESTATE_H