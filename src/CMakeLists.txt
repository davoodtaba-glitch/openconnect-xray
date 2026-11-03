set(CMAKE_VERBOSE_MAKEFILE ON)

set_property(SOURCE ${PROJECT_NAME}.cpp PROPERTY SKIP_AUTOGEN ON)
set_property(SOURCE timestamp.cpp PROPERTY SKIP_AUTOGEN ON)

set(${PROJECT_NAME}_SRCS
    openconnect-gui.qrc
    cert.cpp
    common.h
    config.h.in
    cryptdata.cpp
    dialog/MyCertMsgBox.cpp
    dialog/MyInputDialog.cpp
    dialog/MyMsgBox.cpp
    dialog/editdialog.cpp
    dialog/editdialog.ui
    dialog/NewProfileDialog.cpp
    dialog/NewProfileDialog.ui
    dialog/XrayProfileDialog.cpp
    dialog/XrayProfileDialog.ui
    dialog/logdialog.cpp
    dialog/logdialog.ui
    dialog/VpnProtocolModel.cpp
    dialog/mainwindow.cpp
    dialog/mainwindow.ui
    gtdb.cpp
    key.cpp
    keypair.cpp
    main.cpp
    server_storage.cpp

    timestamp.cpp

    vpninfo.cpp

    logger.cpp

    FileLogger.cpp
    OcSettings.h
    xray/XrayClient.cpp
    xray/XrayConfig.cpp
    xray/XraySubscriptionUpdater.cpp
)

set_source_files_properties(
    ${PROJECT_NAME}.cpp
    timestamp.cpp
    PROPERTIES
        GENERATED true
)

configure_file(
    config.h.in
    config.h
    @ONLY
)

if(MINGW)
    list(APPEND ${PROJECT_NAME}_RESOURCES
        ${PROJECT_NAME}.rc.in
    )

    # Compile the resource from the binary directory so that windres does not
    # receive -I paths containing spaces, which it would split incorrectly.
    set(RC_OBJECT "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.rc.obj")
    add_custom_command(
        OUTPUT ${RC_OBJECT}
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/${PROJECT_NAME}.ico
            ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.ico
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/${PROJECT_NAME}.exe.manifest
            ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.exe.manifest
        COMMAND ${CMAKE_RC_COMPILER} -O coff ${PROJECT_NAME}.rc ${PROJECT_NAME}.rc.obj
        DEPENDS git_revision
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Compiling Windows resources"
        VERBATIM
    )
    list(APPEND ${PROJECT_NAME}_SRCS ${RC_OBJECT})
endif()
if(APPLE)
    list(APPEND ${PROJECT_NAME}_RESOURCES
        Resources/mono_lock.icns
        ${OPENCONNECT_VPNC_SCRIPT}
    )
    set_source_files_properties(
        Resources/mono_lock.icns
        ${OPENCONNECT_VPNC_SCRIPT}
            PROPERTIES MACOSX_PACKAGE_LOCATION Resources
    )
endif()

add_executable(${PROJECT_NAME}
    ${${PROJECT_NAME}_SRCS}
    ${${PROJECT_NAME}_RESOURCES}
)

if(WIN32)
    target_link_libraries(${PROJECT_NAME} wininet iphlpapi)

    set(XRAY_BINARY_DIR "${CMAKE_SOURCE_DIR}/third_party/xray" CACHE PATH "Directory containing the bundled Xray-core files")
    if(NOT EXISTS "${XRAY_BINARY_DIR}/xray.exe")
        message(STATUS "Downloading Xray-core v26.10.31...")
        set(XRAY_ARCHIVE "${CMAKE_BINARY_DIR}/Xray-windows-64.zip")
        file(DOWNLOAD
            "https://github.com/XTLS/Xray-core/releases/download/v26.10.31/Xray-windows-64.zip"
            "${XRAY_ARCHIVE}"
            EXPECTED_HASH SHA256=2e04a9995a6f2ad1b28c0bb25c708ecb2695e43a1c9c7889c4b6ac2b48cf7a6a
            SHOW_PROGRESS
            TLS_VERIFY ON
        )
        file(ARCHIVE_EXTRACT INPUT "${XRAY_ARCHIVE}" DESTINATION "${XRAY_BINARY_DIR}")
    endif()

    foreach(xray_file xray.exe wintun.dll geoip.dat geosite.dat)
        if(EXISTS "${XRAY_BINARY_DIR}/${xray_file}")
            add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${XRAY_BINARY_DIR}/${xray_file}"
                    "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
                COMMENT "Deploying Xray ${xray_file}..."
            )
            install(FILES "${XRAY_BINARY_DIR}/${xray_file}" DESTINATION . COMPONENT App)
        endif()
    endforeach()

    if(EXISTS "${XRAY_BINARY_DIR}/LICENSE")
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${XRAY_BINARY_DIR}/LICENSE"
                "$<TARGET_FILE_DIR:${PROJECT_NAME}>/LICENSE-Xray.txt"
            COMMENT "Deploying Xray license..."
        )
        install(FILES "${XRAY_BINARY_DIR}/LICENSE" DESTINATION . RENAME LICENSE-Xray.txt COMPONENT App)
    endif()
    if(EXISTS "${XRAY_BINARY_DIR}/LICENSE-Wintun")
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${XRAY_BINARY_DIR}/LICENSE-Wintun"
                "$<TARGET_FILE_DIR:${PROJECT_NAME}>/LICENSE-Wintun.txt"
            COMMENT "Deploying Wintun license..."
        )
        install(FILES "${XRAY_BINARY_DIR}/LICENSE-Wintun" DESTINATION . RENAME LICENSE-Wintun.txt COMPONENT App)
    endif()

    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/LICENSE.txt"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>/LICENSE.txt"
        COMMENT "Deploying application license..."
    )

    if(NOT CMAKE_CROSSCOMPILING)
        get_target_property(_qmake_location Qt6::qmake IMPORTED_LOCATION)
        get_filename_component(_qt_bin_dir "${_qmake_location}" DIRECTORY)
        find_program(WINDEPLOYQT_EXECUTABLE NAMES windeployqt HINTS "${_qt_bin_dir}")
        if(WINDEPLOYQT_EXECUTABLE)
            add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                COMMAND ${WINDEPLOYQT_EXECUTABLE}
                    --release
                    --no-translations
                    --no-opengl-sw
                    --compiler-runtime
                    --dir "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
                    "$<TARGET_FILE:${PROJECT_NAME}>"
                COMMENT "Deploying Qt runtime next to the application..."
                VERBATIM
            )
        else()
            message(STATUS "windeployqt not found; deploy the Qt runtime manually")
        endif()
    endif()
endif()

if(MINGW)
    add_definitions(-DSPDLOG_WCHAR_FILENAMES)

    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${CMAKE_BINARY_DIR}/external/vpnc-script.js $<TARGET_FILE_DIR:${PROJECT_NAME}>
        COMMENT "Deploying vpnc-script..."
        VERBATIM
    )

    # The old MinGW runtime shipped with the OpenConnect binaries lacks symbols
    # required by Qt 6, so deploy the runtime matching the compiler.
    get_filename_component(MINGW_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    foreach(runtime_dll libstdc++-6.dll libgcc_s_seh-1.dll libwinpthread-1.dll)
        if(EXISTS "${MINGW_BIN_DIR}/${runtime_dll}")
            add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${MINGW_BIN_DIR}/${runtime_dll}"
                    "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
                COMMENT "Deploying ${runtime_dll}..."
            )
            install(FILES "${MINGW_BIN_DIR}/${runtime_dll}" DESTINATION . COMPONENT App)
        endif()
    endforeach()

    add_dependencies(${PROJECT_NAME}
        vpnc-scripts-${vpnc-scripts-TAG}
    )

    set_target_properties(${PROJECT_NAME} PROPERTIES
        WIN32_EXECUTABLE true
    )

    target_link_libraries (${PROJECT_NAME}
        # system libraries
        wsock32
    )
endif()

#code signing
if(SIGN_EXE STREQUAL "true")
    add_custom_command(
        TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND sh -c "${CMAKE_SOURCE_DIR}/contrib/sign.sh \"$<TARGET_FILE:${PROJECT_NAME}>\""
        COMMENT "Signing executable ..."
        VERBATIM
    )
endif()

if(WIN32 AND NOT CMAKE_CROSSCOMPILING)
    add_dependencies(${PROJECT_NAME}
        openconnect-devel-${openconnect-TAG}
        openconnect-${openconnect-TAG}
    )

    target_link_libraries (${PROJECT_NAME}
        # 3rd party dependencies
        openconnect::gmp
        openconnect::gnutls
        openconnect::hogweed
        openconnect::nettle
        openconnect::openconnect
        openconnect::p11-kit
        openconnect::stoken
        openconnect::xml2
    )
else()
    target_link_libraries (${PROJECT_NAME}
        ${OPENCONNECT_LIBRARIES}
        ${GNUTLS_LIBRARY}
    )
endif()

if(APPLE)
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/Resources/${PROJECT_NAME}.plist.in
        ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.plist
        @ONLY
    )
    set_target_properties(${PROJECT_NAME} PROPERTIES
        MACOSX_BUNDLE true
        OUTPUT_NAME ${PRODUCT_NAME_SHORT}
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}.plist"
        MACOSX_BUNDLE_BUNDLE_NAME ${PRODUCT_NAME_SHORT}
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH}"
        MACOSX_BUNDLE_COPYRIGHT "${PRODUCT_NAME_COPYRIGHT_SHORT}"
        MACOSX_BUNDLE_GUI_IDENTIFIER net.openconnect-vpn.gui
        MACOSX_BUNDLE_ICON_FILE mono_lock.icns
        MACOSX_BUNDLE_INFO_STRING ${PRODUCT_NAME_LONG}
    )
endif()


# common stuff to link...
target_link_libraries (${PROJECT_NAME}
    spdlog::spdlog
    qt-solutions::qtsingleapplication

    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    Qt6::StateMachine
)

if(UNIX)
    target_link_libraries(${PROJECT_NAME}
        Threads::Threads
    )
    if(APPLE)
        target_link_libraries (${PROJECT_NAME}
            ${SECURITY_LIBRARY}
        )
    endif()
endif()

# install executable
install(TARGETS ${PROJECT_NAME}
    RUNTIME DESTINATION . COMPONENT App
    BUNDLE DESTINATION . COMPONENT App
    LIBRARY DESTINATION . COMPONENT App
)

# fixup the bundle
get_target_property(QT_LIBRARY_DIR Qt6::Core LOCATION)
get_filename_component(QT_LIBRARY_DIR ${QT_LIBRARY_DIR} PATH)
list(APPEND libSearchDirs ${QT_LIBRARY_DIR})

if(MINGW)
    set(APPS "\${CMAKE_INSTALL_PREFIX}/${PROJECT_NAME}.exe")
    list(APPEND libSearchDirs ${CMAKE_RUNTIME_OUTPUT_DIRECTORY})

    # Qt Platform Plugin
    install(FILES
        ${_qwindows_dll}
        DESTINATION plugins/platforms
        COMPONENT App
    )
    # Qt schannel plugin
    install(FILES
        ${_schannel_dll}
        DESTINATION plugins/tls
        COMPONENT App
    )
    install(FILES
        ${CMAKE_SOURCE_DIR}/nsis/qt.conf
        DESTINATION .
        COMPONENT App
    )

    set(additionalLib ${_qwindows_dll})
    if(Qt6Core_VERSION VERSION_GREATER_EQUAL "6.0")
        install(FILES
            ${_qwinstyle_dylib}
            DESTINATION plugins/styles
            COMPONENT App
        )
    endif()
elseif(APPLE)
    set(APPS "\${CMAKE_INSTALL_PREFIX}/${PROJECT_NAME}.app")
    list(APPEND libSearchDirs "")

    # Qt Platform Plugin
    install(FILES
        ${_qcocoa_dylib}
        DESTINATION ${PROJECT_NAME}.app/Contents/PlugIns/platforms
        COMPONENT App
    )
    set(additionalLib \${CMAKE_INSTALL_PREFIX}/${PROJECT_NAME}.app/Contents/PlugIns/platforms/libqcocoa.dylib)
    if(Qt6Core_VERSION VERSION_GREATER_EQUAL "6.0")
        install(FILES
            ${_qmacstyle_dylib}
            DESTINATION ${PROJECT_NAME}.app/Contents/PlugIns/styles
            COMPONENT App
        )
        set(additionalLib
            ${additionalLib}
            \${CMAKE_INSTALL_PREFIX}/${PROJECT_NAME}.app/Contents/PlugIns/styles/libqmacstyle.dylib
        )
    endif()
    install(FILES
        ${CMAKE_SOURCE_DIR}/bundle/qt.conf
        DESTINATION ${PROJECT_NAME}.app/Contents/Resources
        COMPONENT App
    )
else()
    message(STATUS "Nothing to bundle...")
endif()

if(MINGW)
    #wintun
    install(FILES
        ${CMAKE_BINARY_DIR}/bin/wintun.dll
        DESTINATION .
        COMPONENT App
    )
endif()

install(CODE "
    include(BundleUtilities)
    if(APPLE)
        set(BU_CHMOD_BUNDLE_ITEMS on)
    endif()
    fixup_bundle(\"${APPS}\" \"${additionalLib}\" \"${libSearchDirs}\")
    " DESTINATION . COMPONENT App
)
