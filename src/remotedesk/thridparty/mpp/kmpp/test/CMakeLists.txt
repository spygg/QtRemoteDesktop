# vim: syntax=cmake
# ----------------------------------------------------------------------------
# kmpp/base built-in unit test case
# ----------------------------------------------------------------------------
# macro for adding osal sub-module unit test
macro(add_kmpp_test module)
    set(test_name ${module}_test)
    string(TOUPPER ${test_name} test_tag)
    #message(STATUS "moduule   : ${module}")
    #message(STATUS "test_name : ${test_name}")
    #message(STATUS "test_tag  : ${test_tag}")

    option(${test_tag} "Build kmpp ${module} unit test" ${BUILD_TEST})
    if(${test_tag})
        add_executable(${test_name} ${test_name}.c)
        target_link_libraries(${test_name} ${MPP_SHARED} utils)
        set_target_properties(${test_name} PROPERTIES FOLDER "kmpp/test")
        install(TARGETS ${test_name} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()
endmacro()

# kmpp venc unit test
add_kmpp_test(kmpp_venc)

# kmpp vdec unit test
add_kmpp_test(kmpp_vdec)

# mpp_enc_frm_cfg objdef + JSON round-trip test
add_kmpp_test(mpp_enc_frm_cfg)

# kmpp ctrl_cfg test
add_kmpp_test(kmpp_ctrl)

