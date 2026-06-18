set(TARGET Service)
set(HEADERS service.h)
set(SOURCES)
if(WIN32)
    list(APPEND SOURCES service_win.cpp windows_service.cpp)
elseif(UNIX AND NOT APPLE AND NOT ANDROID)
    list(APPEND SOURCES service_linux.cpp linux_service.cpp)
elseif(ANDROID)
    list(APPEND SOURCES service_stub.cpp)
endif()
add_library(${TARGET} STATIC ${HEADERS} ${SOURCES})
target_include_directories(${TARGET} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Core)
target_link_libraries(${TARGET} PUBLIC Qt${QT_VERSION_MAJOR}::Core)
if(WIN32)
    target_link_libraries(${TARGET} PUBLIC
        HelperProcess
        SecureInputProcess
        Rdpserver
        wtsapi32
    )
endif()
if(UNIX AND NOT APPLE AND NOT ANDROID)
    target_link_libraries(${TARGET} PUBLIC
        Rdpserver
    )
endif()
