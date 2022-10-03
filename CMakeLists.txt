cmake_minimum_required(VERSION 2.8)

project(server C)

set(CMAKE_EXPORT_COMPILE_COMMANDS 1)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -gdwarf-4")

find_package(Threads REQUIRED)

add_executable(server server.c)
target_link_libraries(server Threads::Threads)

add_executable(client client.c)
target_link_libraries(client Threads::Threads)