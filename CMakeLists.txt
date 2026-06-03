cmake_minimum_required(VERSION 3.20)
project(SilencedTracerBlocker LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED MMSOURCE)
    message(FATAL_ERROR "MMSOURCE is required")
endif()

if(NOT DEFINED HL2SDKCS2)
    message(FATAL_ERROR "HL2SDKCS2 is required")
endif()

add_library(silencedtracerblocker_mm SHARED
    src/silenced_tracer_blocker.cpp
)

target_compile_definitions(silencedtracerblocker_mm PRIVATE
    LINUX
    _LINUX
    POSIX
    META_IS_SOURCE2=1
    COMPILER_GCC
    GNUC
)

target_include_directories(silencedtracerblocker_mm PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shims

    ${MMSOURCE}/core
    ${MMSOURCE}/core/sourcehook
    ${MMSOURCE}/public

    ${HL2SDKCS2}/public
    ${HL2SDKCS2}/public/tier0
    ${HL2SDKCS2}/public/tier1
    ${HL2SDKCS2}/public/mathlib
    ${HL2SDKCS2}/public/entity2
    ${HL2SDKCS2}/public/game/server
    ${HL2SDKCS2}/common
    ${HL2SDKCS2}/game/shared
)

set_target_properties(silencedtracerblocker_mm PROPERTIES
    PREFIX ""
    OUTPUT_NAME "silencedtracerblocker_mm"
)
