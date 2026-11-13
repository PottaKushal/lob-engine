# libFuzzer needs LLVM clang (Apple clang does not ship it).
add_executable(lob_fuzz fuzz_parser.cpp)
target_link_libraries(lob_fuzz PRIVATE lob)
target_compile_options(lob_fuzz PRIVATE -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all)
target_link_options(lob_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
