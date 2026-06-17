if(WIN32)
    add_executable(QtRemoteDesktopKeyboardSvc
        main.cpp
        service.cpp
        service.h
        main.rc
    )
    target_link_libraries(QtRemoteDesktopKeyboardSvc PRIVATE user32 shell32 wtsapi32)
    set_target_properties(QtRemoteDesktopKeyboardSvc PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
        WIN32_EXECUTABLE FALSE
        AUTOMOC OFF
    )
    if(MINGW)
        target_link_options(QtRemoteDesktopKeyboardSvc PRIVATE -static-libgcc -static-libstdc++)
    endif()
endif()
