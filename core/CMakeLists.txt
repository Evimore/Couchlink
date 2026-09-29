add_library(inputline_core STATIC
  src/cpace.cpp
  src/crypto.cpp
  src/feature_responder.cpp
  src/link_client.cpp
  src/link_protocol.cpp
  src/report_converter.cpp
  src/sha256.cpp
  src/sha512.cpp
  src/test_pattern.cpp
  src/timing_stats.cpp
  src/version.cpp
)
target_include_directories(inputline_core PUBLIC include)
target_compile_options(inputline_core PRIVATE ${INPUTLINE_WARNINGS})

if(INPUTLINE_BUILD_TESTS)
  add_executable(inputline_core_tests tests/test_core.cpp)
  target_link_libraries(inputline_core_tests PRIVATE inputline_core)
  add_test(NAME inputline_core_tests COMMAND inputline_core_tests)

  add_executable(inputline_crypto_tests tests/test_crypto.cpp)
  target_link_libraries(inputline_crypto_tests PRIVATE inputline_core)
  add_test(NAME inputline_crypto_tests COMMAND inputline_crypto_tests)

  add_executable(inputline_compat_tests tests/test_compat.cpp)
  target_link_libraries(inputline_compat_tests PRIVATE inputline_core)
  add_test(NAME inputline_compat_tests COMMAND inputline_compat_tests)
endif()
