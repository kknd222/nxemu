cmake_minimum_required(VERSION 3.18)
project(nxemu-android LANGUAGES C CXX)

add_library(nxemu-android SHARED
    nxemu_android_jni.cpp
)

target_include_directories(nxemu-android PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated
)

target_link_libraries(nxemu-android PRIVATE
    nxemu_core
    nxemu-loader
    nxemu-os
    yuzu_common
    log
    android
)

set_property(TARGET nxemu-android PROPERTY POSITION_INDEPENDENT_CODE ON)
