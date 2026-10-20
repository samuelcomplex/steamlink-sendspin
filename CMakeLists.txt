cmake_minimum_required(VERSION 3.16)
project(sendspin_steamlink LANGUAGES C CXX)

# SENDSPIN_LEGACY_API=ON builds against sendspin-cpp v0.8.x: the unencrypted protocol spoken by
# Music Assistant 2.10.x (aiosendspin 9.1). OFF builds against sendspin-cpp >= 0.9: the encrypted
# Sendspin 1.0 protocol (aiosendspin >= 10).
option(SENDSPIN_LEGACY_API "Build against the pre-1.0 (unencrypted) sendspin-cpp API" OFF)

# Only the player and metadata roles; everything else is compiled out to keep the binary small.
set(SENDSPIN_ENABLE_PLAYER ON CACHE BOOL "" FORCE)
set(SENDSPIN_ENABLE_METADATA ON CACHE BOOL "" FORCE)
set(SENDSPIN_ENABLE_CONTROLLER OFF CACHE BOOL "" FORCE)
set(SENDSPIN_ENABLE_COLOR OFF CACHE BOOL "" FORCE)
set(SENDSPIN_ENABLE_ARTWORK OFF CACHE BOOL "" FORCE)
set(SENDSPIN_ENABLE_VISUALIZER OFF CACHE BOOL "" FORCE)
set(SENDSPIN_ENABLE_SOURCE OFF CACHE BOOL "" FORCE)
set(SENDSPIN_ENABLE_OPUS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SENDSPIN_USE_CCACHE OFF CACHE BOOL "" FORCE)

set(SENDSPIN_CPP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/sendspin-cpp" CACHE PATH "sendspin-cpp checkout")
add_subdirectory(${SENDSPIN_CPP_DIR} sendspin-cpp)

set(APP_SOURCES src/main.cpp src/sdl_sink.cpp src/sdl2_api.cpp)
if(NOT SENDSPIN_LEGACY_API)
    list(APPEND APP_SOURCES ${SENDSPIN_CPP_DIR}/examples/common/file_persistence_provider.cpp)
endif()

add_executable(sendspin-steamlink ${APP_SOURCES})
target_include_directories(sendspin-steamlink PRIVATE ${SENDSPIN_CPP_DIR}/examples/common)
target_compile_features(sendspin-steamlink PRIVATE cxx_std_20)
target_link_libraries(sendspin-steamlink PRIVATE sendspin ${CMAKE_DL_LIBS})
if(SENDSPIN_LEGACY_API)
    target_compile_definitions(sendspin-steamlink PRIVATE SENDSPIN_LEGACY_API=1)
endif()
# The audio callback must stamp playback on the library's own clock: SendspinClient::now_us()
# where it exists (newer releases read CLOCK_MONOTONIC_RAW), steady_clock before that.
file(STRINGS ${SENDSPIN_CPP_DIR}/include/sendspin/client.h _has_now_us REGEX "static int64_t now_us\\(")
if(_has_now_us)
    target_compile_definitions(sendspin-steamlink PRIVATE SENDSPIN_HAS_NOW_US=1)
endif()

string(TIMESTAMP SL_BUILD_STAMP "%Y-%m-%d %H:%M UTC" UTC)
target_compile_definitions(sendspin-steamlink PRIVATE SL_BUILD_STAMP="${SL_BUILD_STAMP}")
