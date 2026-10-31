add_executable(test_nlsql test_nlsql.c)
target_link_libraries(test_nlsql PRIVATE nlsql_core)
add_test(NAME test_nlsql COMMAND test_nlsql)

add_executable(test_wire test_wire.c)
target_link_libraries(test_wire PRIVATE nlsql_core)
add_test(NAME test_wire COMMAND test_wire)

add_executable(test_recover test_recover.c)
target_link_libraries(test_recover PRIVATE nlsql_core)
add_test(NAME test_recover COMMAND test_recover)

# TLS 回环测试：需要 OpenSSL 客户端库；运行期还需 NetLeaf TLS 扩展，
# 扩展缺失时测试返回 77（CTest 记为 skipped）。
find_package(OpenSSL QUIET)
if(OpenSSL_FOUND)
    add_executable(test_wire_tls test_wire_tls.c)
    target_link_libraries(test_wire_tls PRIVATE nlsql_core OpenSSL::SSL OpenSSL::Crypto)
    add_test(NAME test_wire_tls COMMAND test_wire_tls)
    set_tests_properties(test_wire_tls PROPERTIES SKIP_RETURN_CODE 77)
else()
    message(STATUS "test_wire_tls skipped (OpenSSL not found)")
endif()

# ---------- 扩展插件测试（需已构建扩展 .so / .dll） ----------
# 测试通过 dlopen 加载扩展，与 NetLeaf 宿主加载方式一致；断言校验真实值。
if(TARGET nlsql_ext_base)
    set(NLSQL_EXT_TEST_ENV "NLSQL_EXT_DIR=${CMAKE_BINARY_DIR}")
    foreach(t base store sql info)
        add_executable(test_ext_${t} test_ext_${t}.c)
        target_link_libraries(test_ext_${t} PRIVATE netleaf_nlsql ${CMAKE_DL_LIBS})
        add_test(NAME test_ext_${t} COMMAND test_ext_${t})
        set_tests_properties(test_ext_${t} PROPERTIES
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
            ENVIRONMENT "${NLSQL_EXT_TEST_ENV}")
    endforeach()

    # WAN 网关依赖宿主 NetLeaf 网络 API；缺少库时测试返回 77（skipped）
    add_executable(test_ext_wan test_ext_wan.c)
    target_link_libraries(test_ext_wan PRIVATE netleaf_nlsql ${CMAKE_DL_LIBS})
    add_test(NAME test_ext_wan COMMAND test_ext_wan)
    set_tests_properties(test_ext_wan PROPERTIES
        SKIP_RETURN_CODE 77
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        ENVIRONMENT "${NLSQL_EXT_TEST_ENV};NLSQL_NETLEAF_LIB=${NETLEAF_LIB_DIR}/libnetleaf${CMAKE_SHARED_LIBRARY_SUFFIX}")
endif()
