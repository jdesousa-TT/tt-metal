cmake_minimum_required(VERSION 3.16)
### THIS CMAKE IS TO BUILD THE UMD_DEVICE SHARED LIBRARY ###
### All variables/compiler flags declared in this file are passed to umd/device .mk file to build device ###
set(WARNINGS "-Wdelete-non-virtual-dtor -Wreturn-type -Wswitch -Wuninitialized -Wno-unused-parameter" CACHE STRING "Warnings to enable")
set(CONFIG_CFLAGS "${CONFIG_CFLAGS} -DFMT_HEADER_ONLY -Werror -Wno-int-to-pointer-cast -I$(TT_METAL_HOME)/tt_metal/third_party/fmt")
if(${CONFIG} STREQUAL "release")
    set(CONFIG_CFLAGS "${CONFIG_CFLAGS} -O3")
elseif(${CONFIG} STREQUAL "ci")
    set(CONFIG_CFLAGS "${CONFIG_CFLAGS} -O3 -DDEBUG=DEBUG")
    set(CONFIG_LDFLAGS "${CONFIG_LDFLAGS} -Wl,--verbose")
elseif(${CONFIG} STREQUAL "assert")
    set(CONFIG_CFLAGS "${CONFIG_CFLAGS} -O3 -g -DDEBUG=DEBUG")
elseif(${CONFIG} STREQUAL "asan")
    set(CONFIG_CFLAGS "${CONFIG_CFLAGS} -O3 -g -DDEBUG=DEBUG -fsanitize=address")
    set(CONFIG_LDFLAGS "${CONFIG_LDFLAGS} -fsanitize=address")
elseif(${CONFIG} STREQUAL "ubsan")
    set(CONFIG_CFLAGS "${CONFIG_CFLAGS} -O3 -g -DDEBUG=DEBUG -fsanitize=undefined")
    set(CONFIG_LDFLAGS "${CONFIG_LDFLAGS} -fsanitize=undefined")
elseif(${CONFIG} STREQUAL "debug")
    set(CONFIG_CFLAGS "${CONFIG_CFLAGS} -O0 -g -DDEBUG=DEBUG")
else()
    message(FATAL_ERROR "Unknown value for CONFIG: \"${CONFIG}\"")
endif()

if(NOT TT_METAL_VERSIM_DISABLED)
    set(UMD_VERSIM_STUB 0)
else()
    set(UMD_VERSIM_STUB 1)
    set(CONFIG_CFLAGS "${CONFIG_CFLAGS} -DTT_METAL_VERSIM_DISABLED")
endif()
if(ENABLE_CODE_TIMERS)
    set(CONFIG_CFLAGS "${CONFIG_CFLAGS} -DTT_ENABLE_CODE_TIMERS")
endif()
if(ENABLE_PROFILER)
    set(CONFIG_CFLAGS "${CONFIG_CFLAGS} -DPROFILER")
endif()
if(ENABLE_TRACY)
    set(CONFIG_CFLAGS "${CONFIG_CFLAGS} -DTRACY_ENABLE -fno-omit-frame-pointer -fPIC")
    set(CONFIG_LDFLAGS "${CONFIG_LDFLAGS} -ltracy -rdynamic")
endif()

set(LDFLAGS_ "${CONFIG_LDFLAGS} -L${CMAKE_BINARY_DIR}/lib -ldl -lz -lboost_thread -lboost_filesystem -lboost_system -lboost_regex -lpthread -latomic -lhwloc -lstdc++")
set(SHARED_LIB_FLAGS_ "-shared -fPIC")
set(STATIC_LIB_FLAGS_ "-fPIC")

set (CMAKE_CXX_FLAGS_ "${CMAKE_CXX_FLAGS_} --std=c++17 -fvisibility-inlines-hidden -Werror")

if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    set(WARNINGS "${WARNINGS} -Wno-c++11-narrowing -Wno-c++2a-extensions")
else()
    set(WARNINGS "${WARNINGS} -Wmaybe-uninitialized")
endif()

set(CFLAGS_ "${WARNINGS} -MMD -I. ${CONFIG_CFLAGS} -mavx2 -mavx2 -DBUILD_DIR=\"${CMAKE_BINARY_DIR}\"")

# This will build the shared library libdevice.so in build/lib where tt_metal can then find and link it
include(ExternalProject)
ExternalProject_Add(
    umd_device
    PREFIX ${UMD_HOME}
    SOURCE_DIR ${UMD_HOME}
    BINARY_DIR ${CMAKE_BINARY_DIR}
    INSTALL_DIR ${CMAKE_BINARY_DIR}
    STAMP_DIR "${CMAKE_BINARY_DIR}/umd_waste/umd_stamp"
    TMP_DIR "${CMAKE_BINARY_DIR}/umd_waste/umd_tmp"
    DOWNLOAD_COMMAND ""
    CONFIGURE_COMMAND ""
    INSTALL_COMMAND ""
    BUILD_COMMAND
        make -f ${UMD_HOME}/device/module.mk umd_device
        OUT=${CMAKE_BINARY_DIR}
        LIBDIR=${CMAKE_BINARY_DIR}/lib
        OBJDIR=${CMAKE_BINARY_DIR}/obj
        UMD_HOME=${UMD_HOME}
        UMD_VERSIM_STUB=${UMD_VERSIM_STUB}
        UMD_VERSIM_HEADERS=${TT_METAL_VERSIM_ROOT}/versim/
        UMD_USER_ROOT=$ENV{TT_METAL_HOME}
        WARNINGS=${WARNINGS}
        SHARED_LIB_FLAGS=${SHARED_LIB_FLAGS_}
        STATIC_LIB_FLAGS=${STATIC_LIB_FLAGS_}
        LDFLAGS=${LDFLAGS_}
        CXXFLAGS=${CMAKE_CXX_FLAGS_}
        CFLAGS=${CFLAGS_}
)