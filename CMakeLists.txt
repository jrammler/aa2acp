cmake_minimum_required(VERSION 3.20)

project(acp_aa_pi_bridge LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenSSL REQUIRED)
find_library(BLUETOOTH_LIBRARY bluetooth REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(DBUS REQUIRED IMPORTED_TARGET dbus-1)

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

add_executable(iap2-tcp src/iap2_tcp_main.cpp)
target_link_libraries(iap2-tcp PRIVATE iap2_link)
target_compile_options(iap2-tcp PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(iap2-bt src/iap2_bt_main.cpp)
target_sources(iap2-bt PRIVATE src/iap2/bluez_pairing.cpp)
target_link_libraries(iap2-bt PRIVATE iap2_link ${BLUETOOTH_LIBRARY} PkgConfig::DBUS)
target_compile_options(iap2-bt PRIVATE -Wall -Wextra -Wpedantic -Werror)

enable_testing()
add_executable(iap2-link-tests tests/link_layer_tests.cpp)
target_link_libraries(iap2-link-tests PRIVATE iap2_link)
target_compile_options(iap2-link-tests PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_test(NAME iap2-link-tests COMMAND iap2-link-tests)
