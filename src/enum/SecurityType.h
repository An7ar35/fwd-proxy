#ifndef FWD_PROXY_ENUM_SECURITYTYPE_H
#define FWD_PROXY_ENUM_SECURITYTYPE_H

#include <ostream>

namespace fwd_proxy {
    enum class SecurityType {
        UNSECURED = 0,
        SECURED,
    };

    std::ostream & operator <<( std::ostream & os, SecurityType type );
}

#endif //FWD_PROXY_ENUM_SECURITYTYPE_H