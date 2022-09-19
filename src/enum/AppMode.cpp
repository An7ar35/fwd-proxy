#include "AppMode.h"

/**
 * Output stream operator
 * @param os Output stream
 * @param mode AppMode enum
 * @return Output stream
 */
std::ostream & fwd_proxy::operator <<( std::ostream &os, fwd_proxy::AppMode mode )  {
    switch( mode ) {
        case AppMode::UNDEFINED: { os << "undefined"; } break;
        case AppMode::CLIENT   : { os << "client";    } break;
        case AppMode::PROXY    : { os << "proxy";     } break;
    }

    return os;
}