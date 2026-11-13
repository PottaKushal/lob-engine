add_executable(lob_bench bench_main.cpp)
target_link_libraries(lob_bench PRIVATE lob)
string(TOUPPER "${CMAKE_BUILD_TYPE}" _bt)
target_compile_definitions(lob_bench PRIVATE
  LOB_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
  LOB_BUILD_FLAGS="${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_bt}} -std=c++20")
