cmake_minimum_required(VERSION 3.23)
project(fwd_proxy)

include(ExternalProject)

set(CMAKE_CXX_STANDARD 20)

add_executable(fwd_proxy
        src/main.cpp
        src/client/Client.cpp
        src/client/Client.h
        src/proxy/Server.cpp
        src/proxy/Server.h
        src/enum/AppMode.cpp
        src/enum/AppMode.h
        src/enum/SecurityType.cpp
        src/enum/SecurityType.h
        src/enum/HandshakeState.cpp
        src/enum/HandshakeState.h)