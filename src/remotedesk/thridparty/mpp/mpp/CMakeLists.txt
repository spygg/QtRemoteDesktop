# vim: syntax=cmake
# ----------------------------------------------------------------------------
# setup mpp codec config first
# ----------------------------------------------------------------------------
include(soc.cmake)
include(codecs.cmake)
include(vproc.cmake)

# ----------------------------------------------------------------------------
# add include directory
# ----------------------------------------------------------------------------
include_directories(inc)
include_directories(common)
include_directories(base/inc)
include_directories(codec/inc)
include_directories(hal/inc)
include_directories(hal/common)
include_directories(vproc/inc)

# ----------------------------------------------------------------------------
# Helper for cleaning extracted .o files
# ----------------------------------------------------------------------------
if(CMAKE_HOST_WIN32)
    set(CLEAN_OBJ_FILES cmd /c "if exist *.o del /F /Q *.o")
else()
    set(CLEAN_OBJ_FILES rm *.o || true)
endif()

# ----------------------------------------------------------------------------
# add mpp base component
# ----------------------------------------------------------------------------
add_subdirectory(base)

# ----------------------------------------------------------------------------
# add codec parsers
# ----------------------------------------------------------------------------
add_subdirectory(codec)

# ----------------------------------------------------------------------------
# add video processor
# ----------------------------------------------------------------------------
add_subdirectory(vproc)

# ----------------------------------------------------------------------------
# add register generation hal
# ----------------------------------------------------------------------------
add_subdirectory(hal)

# ----------------------------------------------------------------------------
# add mpp implement
# ----------------------------------------------------------------------------
set (MPP_SRC
    mpp_info.c
    mpp_sys.c
    mpp.c
    mpp_impl.c
    mpi.c
    )

set(MPP_VERSION "0")
set(MPP_ABI_VERSION "1")

add_library(${MPP_SHARED} SHARED ${MPP_SRC})
target_link_libraries(${MPP_SHARED} PRIVATE ${ASAN_LIB})
set_target_properties(${MPP_SHARED} PROPERTIES C_VISIBILITY_PRESET default)
set_target_properties(${MPP_SHARED} PROPERTIES CXX_VISIBILITY_PRESET default)

# NOTE: due to legacy libray naming issue we can not support version on Android
if (NOT ANDROID)
set_target_properties(${MPP_SHARED} PROPERTIES VERSION ${MPP_VERSION})
set_target_properties(${MPP_SHARED} PROPERTIES SOVERSION ${MPP_ABI_VERSION})
endif()

set_target_properties(${MPP_SHARED} PROPERTIES FOLDER "mpp")
set_target_properties(${MPP_SHARED} PROPERTIES CLEAN_DIRECT_OUTPUT 1)
target_link_libraries(${MPP_SHARED} PRIVATE kmpp
    ${BEGIN_WHOLE_ARCHIVE} mpp_vproc mpp_codec mpp_hal mpp_base kmpp_base osal ${END_WHOLE_ARCHIVE})
target_link_libraries(${MPP_SHARED} PUBLIC ${LIBM} ${CMAKE_THREAD_LIBS_INIT})

# those special platform here
if(ANDROID)
    add_definitions(-static)
    # in Android pthread is in libc, also need liblog
    # Android 14 requires libc++ not libstdc++
    if("${ANDROID_STL}" STREQUAL "c++_static")
        target_link_libraries(${MPP_SHARED} PUBLIC log)
    else()
        target_link_libraries(${MPP_SHARED} PUBLIC log stdc++)
    endif()
endif(ANDROID)

# build static library
add_library(${MPP_STATIC} STATIC ${MPP_SRC})
set_target_properties(${MPP_STATIC} PROPERTIES FOLDER "mpp" OUTPUT_NAME "${MPP_SHARED}")
set_target_properties(${MPP_STATIC} PROPERTIES CLEAN_DIRECT_OUTPUT 1)
target_link_libraries(${MPP_STATIC} PRIVATE kmpp
    ${BEGIN_WHOLE_ARCHIVE} mpp_vproc mpp_codec mpp_hal mpp_base kmpp_base osal ${END_WHOLE_ARCHIVE})

add_custom_command(TARGET ${MPP_STATIC} POST_BUILD
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    # 1) Generate MRI script to merge all .a into one
    COMMAND ${CMAKE_COMMAND} -E echo "CREATE _all.a" > _merge.mri
    COMMAND ${CMAKE_COMMAND} -E echo "ADDLIB $<TARGET_FILE:${MPP_STATIC}>" >> _merge.mri
    COMMAND ${CMAKE_COMMAND} -E echo "ADDLIB $<TARGET_FILE:mpp_codec>" >> _merge.mri
    COMMAND ${CMAKE_COMMAND} -E echo "ADDLIB $<TARGET_FILE:mpp_hal>" >> _merge.mri
    COMMAND ${CMAKE_COMMAND} -E echo "ADDLIB $<TARGET_FILE:mpp_vproc>" >> _merge.mri
    COMMAND ${CMAKE_COMMAND} -E echo "SAVE" >> _merge.mri
    COMMAND ${CMAKE_COMMAND} -E echo "END" >> _merge.mri
    # 2) Merge archives, extract all .o, list members into response file
    COMMAND ${CMAKE_AR} -M < _merge.mri
    COMMAND ${CMAKE_AR} x _all.a
    COMMAND ${CMAKE_AR} t _all.a > _obj_list.txt
    # 3) ld -r merge all .o into single object (no glob needed)
    COMMAND ${CMAKE_LINKER} -r -o _merged.o @_obj_list.txt
    # 4) Create single-member archive
    COMMAND ${CMAKE_COMMAND} -E remove -f "lib${MPP_SHARED}.a"
    COMMAND ${CMAKE_AR} rcs "lib${MPP_SHARED}.a" _merged.o
    # 5) Strip and cleanup
    COMMAND ${CMAKE_STRIP} --strip-debug "lib${MPP_SHARED}.a"
    COMMAND ${CMAKE_COMMAND} -E remove _all.a _merge.mri _obj_list.txt _merged.o
    COMMAND ${CLEAN_OBJ_FILES}
    )

add_subdirectory(legacy)

install(TARGETS ${MPP_SHARED} LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(TARGETS ${MPP_STATIC} ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
