#include "SecurityType.h"

/**
 * Output stream operator
 * @param os Output stream
 * @param type SecurityType enum
 * @return Output stream
 */
std::ostream & fwd_proxy::operator <<( std::ostream &os, fwd_proxy::SecurityType type ) {
    switch( type ) {
        case SecurityType::UNSECURED: { os << "none"; } break;
        case SecurityType::SECURED  : { os << "secured";   } break;
    }

    return os;
}