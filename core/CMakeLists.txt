add_library(couchlink_core STATIC
  src/crypto.cpp
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

  add_executable(couchlink_crypto_tests tests/test_crypto.cpp)
  target_link_libraries(couchlink_crypto_tests PRIVATE couchlink_core)
  add_test(NAME couchlink_crypto_tests COMMAND couchlink_crypto_tests)

  add_executable(couchlink_compat_tests tests/test_compat.cpp)
  target_link_libraries(couchlink_compat_tests PRIVATE couchlink_core)
  add_test(NAME couchlink_compat_tests COMMAND couchlink_compat_tests)
endif()
