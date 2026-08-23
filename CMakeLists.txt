cmake_minimum_required(VERSION 3.20)

project(aa2acp LANGUAGES CXX)

if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(AA2ACP_BUILD_DIAGNOSTICS "Build standalone diagnostic tools" OFF)

find_package(OpenSSL REQUIRED)
find_package(Boost 1.83 REQUIRED COMPONENTS system log_setup log unit_test_framework)
find_package(absl CONFIG REQUIRED)
find_package(aap_protobuf CONFIG REQUIRED)
find_package(aasdk CONFIG REQUIRED)
find_library(BLUETOOTH_LIBRARY bluetooth REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(DBUS REQUIRED IMPORTED_TARGET dbus-1)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil)
find_path(LIBUSB_INCLUDE_DIR libusb.h PATH_SUFFIXES libusb-1.0 REQUIRED)

add_library(iap2_link
    src/iap2/link_layer.cpp
    src/iap2/csm.cpp
    src/iap2/bootstrap.cpp
    src/iap2/carplay_probe.cpp
    src/iap2/network_manager.cpp
)
target_include_directories(iap2_link PUBLIC include)
target_link_libraries(iap2_link PUBLIC OpenSSL::Crypto)
target_compile_options(iap2_link PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_library(airplay_rtsp
    src/airplay/rtsp.cpp
    src/airplay/srp.cpp
    src/airplay/crypto.cpp
    src/airplay/control_cipher.cpp
    src/airplay/bplist.cpp
    src/airplay/head_unit_capabilities.cpp
    src/airplay/pairing_store.cpp
)
target_include_directories(airplay_rtsp PUBLIC include)
target_link_libraries(airplay_rtsp PUBLIC OpenSSL::Crypto)
target_compile_options(airplay_rtsp PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_library(airplay_session src/airplay/session.cpp)
target_include_directories(airplay_session PUBLIC include)
target_link_libraries(airplay_session PUBLIC airplay_rtsp)
target_compile_options(airplay_session PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_library(bridge_config src/bridge/config.cpp)
target_include_directories(bridge_config PUBLIC include)
target_compile_options(bridge_config PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_library(bridge_daemon_log src/bridge/daemon_log.cpp)
target_include_directories(bridge_daemon_log PUBLIC include)
target_compile_options(bridge_daemon_log PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_library(bridge_media_forwarders src/bridge/media_forwarders.cpp)
target_include_directories(bridge_media_forwarders PUBLIC include)
target_compile_options(bridge_media_forwarders PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_library(bridge_carplay_worker src/bridge/carplay_worker.cpp)
target_include_directories(bridge_carplay_worker PUBLIC include)
target_link_libraries(bridge_carplay_worker PUBLIC iap2_bluetooth_worker)
target_compile_options(bridge_carplay_worker PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_library(bridge_management_ui src/bridge/management_ui.cpp)
target_include_directories(bridge_management_ui PUBLIC include)
target_link_libraries(bridge_management_ui PUBLIC bridge_config bluez_inventory iap2_bluetooth_worker)
target_compile_options(bridge_management_ui PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_library(bluez_inventory src/bridge/bluez_inventory.cpp)
target_include_directories(bluez_inventory PUBLIC include)
target_link_libraries(bluez_inventory PUBLIC PkgConfig::DBUS)
target_compile_options(bluez_inventory PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_library(h264_normalizer src/bridge/h264_normalizer.cpp)
target_include_directories(h264_normalizer PUBLIC include)
target_link_libraries(h264_normalizer PUBLIC PkgConfig::FFMPEG)
target_compile_options(h264_normalizer PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_library(android_auto_usb src/aa/wired_receiver.cpp)
target_include_directories(android_auto_usb PUBLIC include)
target_include_directories(android_auto_usb PRIVATE ${LIBUSB_INCLUDE_DIR})
# Newer Abseil (>= 20240722, e.g. Nix) exports this internal target; older
# distro builds (e.g. Ubuntu 24.04) do not and expose the symbols via
# absl::log instead.
if(TARGET absl::log_internal_check_op)
    set(ABSL_LOG_LINK absl::log_internal_check_op)
elseif(TARGET absl::log)
    set(ABSL_LOG_LINK absl::log)
else()
    # Distro abseil packages (e.g. Ubuntu 24.04) may not export log targets
    # at all; aasdk resolves those symbols against the system libabsl itself.
    set(ABSL_LOG_LINK "")
endif()
# aasdk's public headers leak Abseil CHECK() macros into consumers
# (e.g. wired_receiver.cpp references MakeCheckOpString). Newer Abseil
# (Nix) puts those symbols in an internal library that neither AASDK nor
# absl::log pulls in transitively, so link it explicitly when exported.
# Older distros (Ubuntu 24.04) lack these targets and resolve the symbols
# differently, so fall back gracefully.
if(TARGET absl::log_internal_check_op)
    set(ABSL_LOG_LINK absl::log_internal_check_op)
elseif(TARGET absl::log)
    set(ABSL_LOG_LINK absl::log)
else()
    set(ABSL_LOG_LINK "")
endif()
target_link_libraries(android_auto_usb PUBLIC AASDK::aasdk ${ABSL_LOG_LINK})
target_compile_options(android_auto_usb PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(aa2acp-airplay-pair-setup-probe src/airplay_pair_setup_probe_main.cpp)
target_link_libraries(aa2acp-airplay-pair-setup-probe PRIVATE airplay_session)
target_compile_options(aa2acp-airplay-pair-setup-probe PRIVATE -Wall -Wextra -Wpedantic -Werror)

if(AA2ACP_BUILD_DIAGNOSTICS)
  add_executable(aa2acp-iap2-tcp src/iap2_tcp_main.cpp)
  target_link_libraries(aa2acp-iap2-tcp PRIVATE iap2_link)
  target_compile_options(aa2acp-iap2-tcp PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_library(iap2_bluetooth_worker src/iap2_bt_main.cpp src/iap2/bluez_pairing.cpp)
target_include_directories(iap2_bluetooth_worker PUBLIC include)
target_link_libraries(iap2_bluetooth_worker PUBLIC iap2_link airplay_session bluez_inventory ${BLUETOOTH_LIBRARY} PkgConfig::DBUS)
target_compile_options(iap2_bluetooth_worker PRIVATE -Wall -Wextra -Wpedantic -Werror)

if(AA2ACP_BUILD_DIAGNOSTICS)
  add_executable(aa2acp-iap2-bt src/iap2_bt_cli_main.cpp)
  target_link_libraries(aa2acp-iap2-bt PRIVATE iap2_bluetooth_worker)
  target_compile_options(aa2acp-iap2-bt PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

add_executable(aa2acp src/bridge_daemon_main.cpp)
target_link_libraries(aa2acp PRIVATE bridge_config bridge_daemon_log bridge_media_forwarders bridge_management_ui bridge_carplay_worker bluez_inventory android_auto_usb h264_normalizer airplay_rtsp iap2_link iap2_bluetooth_worker)
target_compile_options(aa2acp PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(aa2acp-android-auto-usb src/android_auto_usb_main.cpp)
target_link_libraries(aa2acp-android-auto-usb PRIVATE android_auto_usb)
target_compile_options(aa2acp-android-auto-usb PRIVATE -Wall -Wextra -Wpedantic -Werror)

include(CTest)
if(BUILD_TESTING)
  add_executable(iap2-link-tests tests/link_layer_tests.cpp)
  target_link_libraries(iap2-link-tests PRIVATE iap2_link)
  target_compile_options(iap2-link-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME iap2-link-tests COMMAND iap2-link-tests)
  add_executable(airplay-rtsp-tests tests/airplay_rtsp_tests.cpp)
  target_link_libraries(airplay-rtsp-tests PRIVATE airplay_rtsp)
  target_compile_options(airplay-rtsp-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME airplay-rtsp-tests COMMAND airplay-rtsp-tests)
  add_executable(bridge-config-tests tests/config_tests.cpp)
  target_link_libraries(bridge-config-tests PRIVATE bridge_config)
  target_compile_options(bridge-config-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME bridge-config-tests COMMAND bridge-config-tests)
  add_executable(h264-normalizer-tests tests/h264_normalizer_tests.cpp)
  target_link_libraries(h264-normalizer-tests PRIVATE h264_normalizer)
  target_compile_options(h264-normalizer-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
  add_test(NAME h264-normalizer-tests COMMAND h264-normalizer-tests)
  add_test(NAME no-direct-stream-logs
           COMMAND ${CMAKE_COMMAND} -DPROJECT_SOURCE_DIR=${PROJECT_SOURCE_DIR}
                   -P ${PROJECT_SOURCE_DIR}/tests/no_direct_stream_logs.cmake)
endif()

option(AA2ACP_INSTALL_SYSTEMD "Install systemd units with the package" ON)

install(TARGETS aa2acp RUNTIME DESTINATION bin)
if(AA2ACP_INSTALL_SYSTEMD)
    install(FILES systemd/aa2acp.service
                  systemd/aa2acp-update.path
                  systemd/aa2acp-update.service
            DESTINATION lib/systemd/system)
endif()
install(FILES udev/70-aa2acp-android-auto.rules DESTINATION lib/udev/rules.d)

set(CPACK_PACKAGE_NAME aa2acp)
set(CPACK_PACKAGE_VERSION 0.1.0)
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Android Auto to CarPlay Raspberry Pi bridge")
set(CPACK_PACKAGE_VENDOR aa2acp)
set(CPACK_PACKAGE_CONTACT "jrammler <jrammler@users.noreply.github.com>")
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE arm64)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS OFF)
set(CPACK_GENERATOR DEB)
include(CPack)
