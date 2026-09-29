add_library(couchlink_core STATIC
  src/feature_responder.cpp
  src/link_client.cpp
  src/link_protocol.cpp
  src/report_converter.cpp
  src/sha256.cpp
  src/test_pattern.cpp
  src/timing_stats.cpp
)
target_include_directories(couchlink_core PUBLIC include)
target_compile_options(couchlink_core PRIVATE ${COUCHLINK_WARNINGS})

if(COUCHLINK_BUILD_TESTS)
  add_executable(couchlink_core_tests tests/test_core.cpp)
  target_link_libraries(couchlink_core_tests PRIVATE couchlink_core)
  add_test(NAME couchlink_core_tests COMMAND couchlink_core_tests)
endif()
