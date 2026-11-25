# A program that uses an installed ffaudio the way an application does: find_package(ffaudio), every public header,
# from C and from C++. CI builds it against `cmake --install` of the library (and the same sources by pkg-config).
#   cmake -S tests/consumer -B build-consumer -DCMAKE_PREFIX_PATH=<install prefix>
cmake_minimum_required(VERSION 3.20)
project(ffaudio_consumer LANGUAGES C CXX)

find_package(ffaudio REQUIRED)

add_executable(consumer_c consumer.c)
set_target_properties(consumer_c PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)
target_link_libraries(consumer_c PRIVATE ffaudio::ffaudio)

add_executable(consumer_cpp consumer.cpp)
set_target_properties(consumer_cpp PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON)
target_link_libraries(consumer_cpp PRIVATE ffaudio::ffaudio)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(consumer_c PRIVATE -Wall -Wextra -pedantic -Werror)
    target_compile_options(consumer_cpp PRIVATE -Wall -Wextra -Werror)
endif()
