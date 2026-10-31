cmake_minimum_required(VERSION 3.16)
project(netleaf_nlsql VERSION 1.2.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

# ---------- 平台差异 ----------
# Windows（MinGW-w64，clang 或 gcc 均可）：套接字在 ws2_32，无 libm/libdl；
# POSIX：libm + libdl。其余源码通过 include/netleaf_nlsql_platform.h 抽象。
if(WIN32)
    set(NLSQL_OS_LIBS ws2_32 bcrypt)
    add_compile_definitions(WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        add_compile_options(-Wno-unknown-pragmas)
    endif()
else()
    set(NLSQL_OS_LIBS m ${CMAKE_DL_LIBS})
endif()

# ---------- 核心库（无第三方依赖） ----------
set(NLSQL_CORE_SOURCES
    src/nlsql_util.c
    src/nlsql_parser.c
    src/nlsql_storage.c
    src/nlsql_engine.c
    src/nlsql_async.c
    src/nlsql_wire.c
    src/nlsql_ext.c
    src/nlsql_ext_meta.c
    src/nlsql.c
)

add_library(nlsql_core STATIC ${NLSQL_CORE_SOURCES})
target_include_directories(nlsql_core PUBLIC include PRIVATE src)
target_link_libraries(nlsql_core PUBLIC Threads::Threads ${NLSQL_OS_LIBS})
target_compile_options(nlsql_core PRIVATE -Wall -Wextra)

add_library(netleaf_nlsql SHARED ${NLSQL_CORE_SOURCES})
set_target_properties(netleaf_nlsql PROPERTIES OUTPUT_NAME "netleaf_nlsql")
target_include_directories(netleaf_nlsql PUBLIC include PRIVATE src)
target_link_libraries(netleaf_nlsql PUBLIC Threads::Threads ${NLSQL_OS_LIBS})
target_compile_options(netleaf_nlsql PRIVATE -Wall -Wextra -O2 -finline-functions)

# ---------- NL 扩展插件（需要 NetLeaf 头文件，可选） ----------
set(NETLEAF_ROOT "" CACHE PATH "NetLeaf 根目录（提供 netleaf_module.h 时构建扩展插件）")
set(NETLEAF_LIB_DIR "${NETLEAF_ROOT}/build-nl/lib" CACHE PATH "NetLeaf 动态库目录（WAN 扩展测试用）")

if(NETLEAF_ROOT AND EXISTS "${NETLEAF_ROOT}/include/netleaf_module.h")
    set(NLSQL_EXT_SOURCES base sql store wan info)
    foreach(ext ${NLSQL_EXT_SOURCES})
        add_library(nlsql_ext_${ext} SHARED src/ext/nlsql_ext_${ext}.c)
        target_include_directories(nlsql_ext_${ext} PRIVATE include ${NETLEAF_ROOT}/include)
        target_link_libraries(nlsql_ext_${ext} PRIVATE netleaf_nlsql ${NLSQL_OS_LIBS})
        set_target_properties(nlsql_ext_${ext} PROPERTIES
            PREFIX "" OUTPUT_NAME "nlsql_ext_${ext}")
        if(NOT WIN32)
            # Windows 按 DLL 搜索路径加载，无需 $ORIGIN
            set_target_properties(nlsql_ext_${ext} PROPERTIES
                BUILD_RPATH "$ORIGIN" INSTALL_RPATH "$ORIGIN")
        endif()
        target_compile_options(nlsql_ext_${ext} PRIVATE -Wall -Wextra)
    endforeach()
    message(STATUS "NL extension plugins enabled (NETLEAF_ROOT=${NETLEAF_ROOT})")
else()
    message(STATUS "NL extension plugins skipped (set -DNETLEAF_ROOT=path/to/netleaf to build)")
endif()

# ---------- 测试 ----------
enable_testing()
add_subdirectory(test)
