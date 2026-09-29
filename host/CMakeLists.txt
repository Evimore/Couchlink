find_package(Threads REQUIRED)

add_library(couchlink_host STATIC
  src/client_store.cpp
  src/controller_backend.cpp
  src/desktop.cpp
  src/discovery.cpp
  src/link_server.cpp
  src/log.cpp
  src/net.cpp
  src/steam_controller_device.cpp
  src/usbip_attach.cpp
  src/usbip_server.cpp
)
target_include_directories(couchlink_host PUBLIC src)
target_link_libraries(couchlink_host PUBLIC couchlink_core Threads::Threads)
target_compile_options(couchlink_host PRIVATE ${COUCHLINK_WARNINGS})
if(WIN32)
  target_link_libraries(couchlink_host PUBLIC ws2_32 user32 advapi32)
  target_compile_definitions(couchlink_host PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
endif()

add_executable(couchlink-host src/main.cpp)
target_link_libraries(couchlink-host PRIVATE couchlink_host)
target_compile_options(couchlink-host PRIVATE ${COUCHLINK_WARNINGS})
target_compile_definitions(couchlink-host PRIVATE COUCHLINK_VERSION="${COUCHLINK_VERSION}")

add_executable(couchlink-sim tools/couchlink_sim.cpp)
target_link_libraries(couchlink-sim PRIVATE couchlink_host)
target_compile_options(couchlink-sim PRIVATE ${COUCHLINK_WARNINGS})

install(TARGETS couchlink-host couchlink-sim RUNTIME DESTINATION bin)

if(COUCHLINK_BUILD_TESTS)
  add_executable(couchlink_usbip_tests tests/test_usbip_device.cpp)
  target_link_libraries(couchlink_usbip_tests PRIVATE couchlink_host)
  target_include_directories(couchlink_usbip_tests PRIVATE tests)
  add_test(NAME couchlink_usbip_tests COMMAND couchlink_usbip_tests)

  add_executable(couchlink_link_tests tests/test_link_server.cpp)
  target_link_libraries(couchlink_link_tests PRIVATE couchlink_host)
  target_include_directories(couchlink_link_tests PRIVATE tests)
  add_test(NAME couchlink_link_tests COMMAND couchlink_link_tests)
endif()
