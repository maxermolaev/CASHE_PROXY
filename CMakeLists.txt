cmake_minimum_required(VERSION 4.0)
project(CACHE_PROXY C)

set(CMAKE_C_STANDARD 17)

add_executable(CACHE_PROXY src/main.c
        include/cache.h
        include/env.h
        include/log.h
        include/message.h
        include/proxy.h
        include/thread_pool.h
        src/cache.c
        src/entry.c
        src/env.c
        src/log.c
        src/message.c
        src/proxy.c
        src/thread_pool.c
        picohttpparser/picohttpparser.c
        picohttpparser/picohttpparser.h
)


target_include_directories(CACHE_PROXY PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
