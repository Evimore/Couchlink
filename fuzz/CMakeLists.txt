# Fuzz targets (libFuzzer). Build with clang:
#   cmake -S . -B build-fuzz -DCMAKE_CXX_COMPILER=clang++ -DINPUTLINE_FUZZ=ON
#   cmake --build build-fuzz
#   build-fuzz/fuzz/fuzz_link_server -max_total_time=60
set(INPUTLINE_FUZZ_FLAGS -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined)

function(inputline_fuzzer name)
  add_executable(${name} ${name}.cpp)
  target_compile_options(${name} PRIVATE ${INPUTLINE_FUZZ_FLAGS})
  target_link_options(${name} PRIVATE ${INPUTLINE_FUZZ_FLAGS})
  target_link_libraries(${name} PRIVATE ${ARGN})
endfunction()

inputline_fuzzer(fuzz_link_protocol inputline_core)
inputline_fuzzer(fuzz_link_server inputline_host)
if(NOT WIN32)
  inputline_fuzzer(fuzz_usbip_server inputline_host)
endif()
