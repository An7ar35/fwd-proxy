#include "HandshakeState.h"

/**
 * Output stream operator
 * @param os Output stream
 * @param type HandshakeStage enum
 * @return Output stream
 */
std::ostream & fwd_proxy::operator <<( std::ostream &os, fwd_proxy::HandshakeState stage ) {
    switch( stage ) {
        case HandshakeState::INIT  : { os << "INIT";    } break;
        case HandshakeState::AUTH0 : { os << "AUTH0";   } break;
        case HandshakeState::AUTH1 : { os << "AUTH1";   } break;
        case HandshakeState::READY : { os << "READY";   } break;
        case HandshakeState::DCN   : { os << "DCN";     } break;
    }

    return os;
}