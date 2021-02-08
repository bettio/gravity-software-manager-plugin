include_directories(${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/workers)

add_subdirectory(zypp)

install(FILES com.ispirata.Hemera.SoftwareManager.Backend.conf
        DESTINATION ${HA_DBUS_SYSTEM_POLICY_DIR}
        COMPONENT gravity)

install(FILES com.ispirata.Hemera.SoftwareManager.Backend.service
        DESTINATION ${DBUS_SYSTEM_ACTIVATION_DIR}
        COMPONENT gravity-software-manager)
