set(GravityCenterSoftwareManager_SRCS
    applicationmanagerinterface.cpp
    imagestoreupdatesource.cpp
    incrementalupdateoperation.cpp
    progressinterface.cpp
    remoteupdateinterface.cpp
    recoveryupdateoperation.cpp
    softwaremanagerinterface.cpp
    softwaremanagerplugin.cpp
    updateoperation.cpp
    updatesource.cpp
)

qt5_add_dbus_adaptor(GravityCenterSoftwareManager_SRCS ${HEMERAQTSDK_DBUS_INTERFACES_DIR}/com.ispirata.Hemera.SoftwareManagement.ApplianceManager.xml
                        softwaremanagerinterface.h SoftwareManagerInterface)
qt5_add_dbus_adaptor(GravityCenterSoftwareManager_SRCS ${HEMERAQTSDK_DBUS_INTERFACES_DIR}/com.ispirata.Hemera.SoftwareManagement.ApplicationManager.xml
                        applicationmanagerinterface.h ApplicationManagerInterface)
qt5_add_dbus_adaptor(GravityCenterSoftwareManager_SRCS ${HEMERAQTSDK_DBUS_INTERFACES_DIR}/com.ispirata.Hemera.SoftwareManagement.ProgressReporter.xml
                        progressinterface.h ProgressInterface)

hyperspace_add_qt5_consumer(GravityCenterSoftwareManager_SRCS ${CMAKE_SOURCE_DIR}/share/com.ispirata.Hemera.SoftwareManagement.RemoteUpdate.json
                            remoteupdateinterface.h RemoteUpdateInterface)

# final lib
add_library(gravity-center-plugin-software-manager SHARED ${GravityCenterSoftwareManager_SRCS})

target_link_libraries(gravity-center-plugin-software-manager
                      Gravity::Supermassive
                      Qt5::Core Qt5::Network Qt5::DBus
                      HyperspaceQt5::Core HyperspaceQt5::ProducerConsumer
                      HemeraQt5SDK::Core HemeraQt5SDK::SoftwareManagement)

# Install phase
install(TARGETS gravity-center-plugin-software-manager
        RUNTIME DESTINATION "${GRAVITY_PLUGINS_DIR}" COMPONENT bin
        LIBRARY DESTINATION "${GRAVITY_PLUGINS_DIR}" COMPONENT shlib
        ARCHIVE DESTINATION "${GRAVITY_PLUGINS_DIR}" COMPONENT shlib
        COMPONENT gravity-software-manager)

install(FILES com.ispirata.Hemera.SoftwareManager.conf
        DESTINATION ${HA_DBUS_SYSTEM_POLICY_DIR}
        COMPONENT gravity)
