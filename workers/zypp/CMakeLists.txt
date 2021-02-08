include_directories(${ZYPP_INCLUDE_DIR})

set(gravity-software-manager-zypp-worker_SRCS
    main.cpp
    zyppbackend.cpp
    zyppworkercallbacks.cpp
)

qt5_add_dbus_adaptor(gravity-software-manager-zypp-worker_SRCS ${CMAKE_SOURCE_DIR}/src/com.ispirata.Hemera.SoftwareManager.Backend.xml
                     zyppbackend.h ZyppBackend)

# final lib
add_executable(gravity-software-manager-zypp-worker ${gravity-software-manager-zypp-worker_SRCS})

target_link_libraries(gravity-software-manager-zypp-worker Qt5::Core Qt5::DBus HemeraQt5SDK::Core HemeraQt5SDK::SoftwareManagement ${ZYPP_LIBRARY})

configure_file(gravity-software-manager-zypp-worker.service.in "${CMAKE_CURRENT_BINARY_DIR}/gravity-software-manager-zypp-worker.service" @ONLY)

# Install phase
install(TARGETS gravity-software-manager-zypp-worker
        RUNTIME DESTINATION "${HA_TOOLS_DIR}" COMPONENT bin
        LIBRARY DESTINATION "${HA_TOOLS_DIR}" COMPONENT shlib
        COMPONENT gravity-software-manager)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/gravity-software-manager-zypp-worker.service
        DESTINATION ${HA_SYSTEMD_SYSTEM_DIR}
        COMPONENT gravity-software-manager)
