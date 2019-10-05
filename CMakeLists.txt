cmake_minimum_required(VERSION 3.5)
project(sdluxer C)

add_executable(sdluxer
        sdluxer.c
        lux/lux.c
        lux/lux.h
        lux/font.h)

find_package(SDL REQUIRED)
include_directories(SYSTEM ${SDL_INCLUDE_DIR})
include_directories(lux)
target_link_libraries(sdluxer ${SDL_LIBRARY})
target_link_libraries(sdluxer rt)
