cmake_minimum_required(VERSION 3.12 FATAL_ERROR)

if(APPLE)
     if(NOT CMAKE_OSX_DEPLOYMENT_TARGET)
        set(CMAKE_OSX_DEPLOYMENT_TARGET "10.12" CACHE STRING "" FORCE)
    endif()
endif()

project(openconnect-gui
    VERSION 1.6.2
    HOMEPAGE_URL https://gui.openconnect-vpn.net/
    LANGUAGES CXX
)

message(STATUS "Building ${PROJECT_NAME} v${PROJECT_VERSION} [cmake]")

# The SHORT and COMPANY names affect the installation directory.
set(PRODUCT_NAME_SHORT "OpenConnect-GUI")
set(PRODUCT_NAME_COMPANY "OpenConnect-GUI Team")
set(PRODUCT_NAME_COMPANY_DOMAIN "gui.openconnect-vpn.net")
set(INTERNAL_PROJECT_VERSION "${PROJECT_VERSION}")

# The long name is used as the installer description
# and About screen description.
set(PRODUCT_NAME_LONG "OpenConnect VPN graphical client")

# Printed in the About screen
set(PRODUCT_NAME_COPYRIGHT_SHORT "Copyright © 2016-2024 Red Hat Inc., Nikos Mavrogiannopoulos, Ľubomír Carik and others")
set(PRODUCT_NAME_COPYRIGHT_FULL "Copyright © 2014-2015 Red Hat Inc.<br>Copyright © 2016-2024 Nikos Mavrogiannopoulos<br>Copyright © 2016-2024 Ľubomír Carik<br>Copyright © 2020-2024 David Woodhouse<br>Copyright © 2021-2024 Dimitri Papadopoulos Orfanos<br>Copyright © 2023-2024 Marios Paouris")

# The application name / main window title / installer app name
set(APP_NAME "OpenConnect VPN")
# The URL for determining the latest release
set(GITLAB_LATEST_RELEASE_URL "https://gitlab.com/api/v4/projects/12274423/releases/permalink/latest")
# The URL for the latest release
set(APP_RELEASES_URL "https://gitlab.com/openconnect/openconnect-gui/-/releases")
# The URL for downloading the latest release
set(APP_DOWNLOAD_WIN_URL "https://www.infradead.org/openconnect-gui/download/openconnect-gui-%1-win64.exe")

set(APP_ISSUES_URL "https://gitlab.com/openconnect/openconnect-gui/-/issues")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/CMake/Modules/")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/CMake/Includes/")

set(SIGN_EXE "false" CACHE STRING "When set to true the executable will be signed")

include(ProjectSettings)
include(ProjectDependencies)

if(MINGW)
    # FIXME: upgrading CMake from 3.15 to newer version fails on packaging
    # because objdump.exe is not supported (actually the exe suffix break it)
    # TODO: bundling dependencies use CMake deprecated function; need update..
    find_program(CMAKE_OBJDUMP NAMES objdump.exe)
    string(REGEX REPLACE "\\.exe$" "" CMAKE_OBJDUMP ${CMAKE_OBJDUMP})

endif()

add_subdirectory(src)

install(FILES LICENSE.txt
    DESTINATION .
    COMPONENT App
)

include(clang-format)
include(git_revision_main)
include(build_timestamp_main)

include(ProjectPackage)

