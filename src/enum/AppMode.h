#ifndef FWD_PROXY_ENUM_APPMODE_H
#define FWD_PROXY_ENUM_APPMODE_H

#include <ostream>

namespace fwd_proxy {
    enum class AppMode {
        UNDEFINED = -1,
        CLIENT = 0,
        PROXY = 1,
    };

    std::ostream &operator <<( std::ostream &os, AppMode mode );
}

#endif //FWD_PROXY_ENUM_APPMODE_H