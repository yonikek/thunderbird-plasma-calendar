cmake_minimum_required(VERSION 3.21)
project(thunderbird-plasma-calendar VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(ECM 6.0 REQUIRED NO_MODULE)
set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH})
include(KDEInstallDirs)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(ECMQmlModule)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core DBus Qml)
find_package(KF6CoreAddons 6.0 CONFIG REQUIRED)
find_package(KF6Declarative 6.0 CONFIG REQUIRED)
find_package(KF6Config 6.0 CONFIG REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_executable(thunderbird-calendar-native-host src/nativehost.cpp)
target_link_libraries(thunderbird-calendar-native-host PRIVATE Qt6::Core Qt6::DBus)
install(TARGETS thunderbird-calendar-native-host RUNTIME DESTINATION ${KDE_INSTALL_BINDIR})

# Mozilla native-messaging host manifest. The absolute path is embedded at build
# time so the manifest remains valid in an immutable Nix store.
set(THUNDERBIRD_CALENDAR_HOST_PATH "${CMAKE_INSTALL_FULL_BINDIR}/thunderbird-calendar-native-host")
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/native-messaging-host.json.in
    ${CMAKE_CURRENT_BINARY_DIR}/org.kde.plasma.thunderbird_calendar.json
    @ONLY
)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/org.kde.plasma.thunderbird_calendar.json
    DESTINATION ${KDE_INSTALL_DATADIR}/mozilla/native-messaging-hosts)

# Plasma 6 CalendarEvents provider. This is discovered by EventPluginsManager
# from the plasmacalendarplugins KPlugin namespace.
kcoreaddons_add_plugin(thunderbirdcalendar
    SOURCES src/calendareventplugin.cpp src/calendareventplugin.h
    INSTALL_NAMESPACE "plasmacalendarplugins"
)
target_link_libraries(thunderbirdcalendar PRIVATE
    Qt6::Core
    Qt6::DBus
    KF6::CalendarEvents
    KF6::ConfigCore
)

# QML settings module for choosing which Thunderbird calendars appear in the
# Digital Clock calendar popup.
ecm_add_qml_module(thunderbirdcalendarconfig
    URI org.kde.ThunderbirdCalendarPlugin
    VERSION 1.0
    QML_FILES src/ThunderbirdCalendarConfig.qml
    SOURCES src/calendarconfigmodel.cpp src/calendarconfigmodel.h
    GENERATE_PLUGIN_SOURCE
)
target_include_directories(thunderbirdcalendarconfig PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(thunderbirdcalendarconfig PRIVATE
    Qt6::Core
    Qt6::DBus
    Qt6::Qml
    KF6::ConfigCore
)
ecm_finalize_qml_module(thunderbirdcalendarconfig)

# EventPluginsManager resolves the config UI relative to the CalendarEvents
# plugin shared object, so install a file copy alongside that plugin as well.
install(FILES src/ThunderbirdCalendarConfig.qml
    DESTINATION ${KDE_INSTALL_PLUGINDIR}/plasmacalendarplugins)

# Build an installable Thunderbird XPI using only Python's standard library.
set(THUNDERBIRD_XPI ${CMAKE_CURRENT_BINARY_DIR}/thunderbird-plasma-calendar.xpi)
add_custom_command(
    OUTPUT ${THUNDERBIRD_XPI}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/build-xpi.py
            --source ${CMAKE_CURRENT_SOURCE_DIR}/extension
            --output ${THUNDERBIRD_XPI}
    DEPENDS
        tools/build-xpi.py
        extension/manifest.json
        extension/background.js
        extension/experiments/schema.json
        extension/experiments/implementation.js
        extension/icons/calendar.png
    VERBATIM
)
add_custom_target(thunderbird-extension ALL DEPENDS ${THUNDERBIRD_XPI})
install(FILES ${THUNDERBIRD_XPI}
    DESTINATION ${KDE_INSTALL_DATADIR}/thunderbird-plasma-calendar)

include(FeatureSummary)
feature_summary(WHAT REQUIRED_PACKAGES_NOT_FOUND FATAL_ON_MISSING_REQUIRED_PACKAGES)
