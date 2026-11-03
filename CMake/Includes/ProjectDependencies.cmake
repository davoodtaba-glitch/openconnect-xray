# common dependencies
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.0 REQUIRED COMPONENTS Core Gui Widgets Network StateMachine)
if(MINGW)
    get_target_property(_qwindows_dll Qt6::QWindowsIntegrationPlugin LOCATION)
    if(Qt6Core_VERSION VERSION_GREATER_EQUAL "6.7")
        get_target_property(_qwinstyle_dylib Qt6::QModernWindowsStylePlugin LOCATION)
    elseif(Qt6Core_VERSION VERSION_GREATER_EQUAL "6.0")
        get_target_property(_qwinstyle_dylib Qt6::QWindowsVistaStylePlugin LOCATION)
    endif()

    get_target_property(_schannel_dll Qt6::QSchannelBackendPlugin LOCATION)
endif()
if(APPLE)
    get_target_property(_qcocoa_dylib Qt6::QCocoaIntegrationPlugin LOCATION)
    if(Qt6Core_VERSION VERSION_GREATER_EQUAL "6.0")
        get_target_property(_qmacstyle_dylib Qt6::QMacStylePlugin LOCATION)
    endif()
endif()

# On MacOS, Linux or when cross-building for Windows we have sane
# package management. Use it.
if(CMAKE_CROSSCOMPILING OR NOT WIN32)
    find_package(GnuTLS REQUIRED)
    if(GNUTLS_FOUND)
        message(STATUS "Library 'GnuTLS' found at ${GNUTLS_LIBRARIES}")
    include_directories(SYSTEM ${GNUTLS_INCLUDE_DIR})
    else()
        message(FATAL_ERROR "Library 'GnuTLS' not found! Install it with e.g. 'brew install gnutls' or 'dnf install gnutls-devel'")
    endif()

    find_package(OpenConnect REQUIRED)
    if(OPENCONNECT_FOUND)
        message(STATUS "Library 'OpenConnect' found at ${OPENCONNECT_LIBRARIES}")
        link_directories(${OPENCONNECT_LIBRARY_DIRS})
        include_directories(SYSTEM ${OPENCONNECT_INCLUDE_DIRS})
    else()
        message(FATAL_ERROR "Library 'OpenConnect' not found! Install it with e.g. 'brew install openconnect or 'dnf install openconnect'")
    endif()

    # This is optional as the package isn't ubiquitous. We'll pull it
    # in and build it locally if not found.
    find_package(spdlog CONFIG)
endif()

if(UNIX)
    set(CMAKE_THREAD_PREFER_PTHREAD ON)
    find_package(Threads REQUIRED)
endif()

if(APPLE)
    find_library(SECURITY_LIBRARY Security REQUIRED)
    if(SECURITY_LIBRARY)
        message(STATUS "Framework 'Security' found at ${SECURITY_LIBRARY}")

        link_directories(${SECURITY_LIBRARY_DIRS})
        include_directories(SYSTEM ${SECURITY_LIBRARY_INCLUDE_DIRS})
    else()
        message(FATAL_ERROR "Framework 'Security' not found!")
    endif()
    mark_as_advanced(SECURITY_LIBRARY)
endif()

# mingw32/mingw64 and other external dependencies
include(ProjectExternals)
