find_package(Threads REQUIRED)

add_library(inputline_host STATIC
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
target_include_directories(inputline_host PUBLIC src)
target_link_libraries(inputline_host PUBLIC inputline_core Threads::Threads)
target_compile_options(inputline_host PRIVATE ${INPUTLINE_WARNINGS})
if(WIN32)
  target_link_libraries(inputline_host PUBLIC ws2_32 user32 advapi32 wtsapi32)
  target_compile_definitions(inputline_host PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
endif()

add_executable(inputline-host src/main.cpp)
if(WIN32)
  enable_language(RC)
  target_sources(inputline-host PRIVATE src/inputline-host.rc)
endif()
target_link_libraries(inputline-host PRIVATE inputline_host)
target_compile_options(inputline-host PRIVATE ${INPUTLINE_WARNINGS})
target_compile_definitions(inputline-host PRIVATE INPUTLINE_VERSION="${INPUTLINE_VERSION}")

add_executable(inputline-sim tools/inputline_sim.cpp)
target_link_libraries(inputline-sim PRIVATE inputline_host)
target_compile_options(inputline-sim PRIVATE ${INPUTLINE_WARNINGS})

install(TARGETS inputline-host inputline-sim RUNTIME DESTINATION bin)

if(INPUTLINE_BUILD_TESTS)
  add_executable(inputline_usbip_tests tests/test_usbip_device.cpp)
  target_link_libraries(inputline_usbip_tests PRIVATE inputline_host)
  target_include_directories(inputline_usbip_tests PRIVATE tests)
  add_test(NAME inputline_usbip_tests COMMAND inputline_usbip_tests)

  add_executable(inputline_link_tests tests/test_link_server.cpp)
  target_link_libraries(inputline_link_tests PRIVATE inputline_host)
  target_include_directories(inputline_link_tests PRIVATE tests)
  add_test(NAME inputline_link_tests COMMAND inputline_link_tests)
endif()
