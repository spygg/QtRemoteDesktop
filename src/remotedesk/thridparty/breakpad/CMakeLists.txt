# 对照 3rdparty/breakpad/breakpad.pri 忠实翻译
#
# pri 顶部说明注释 (保留):
#   Implemented using http://blog.inventic.eu/2012/08/qt-and-google-breakpad/
#   as a the reference.
#   Get Google Breakpad here: https://code.google.com/p/google-breakpad/
#   The required breakpad sources have been copied into /src in order to make
#   integration with the application smooth and easy.
#   To use source from Google Breakpad SVN checkout instead, change $$PWD/src
#   to path where it was checked out.

# pri: HEADERS += $$PWD/crashhandler.h
# pri: SOURCES += $$PWD/crashhandler.cpp
add_library(radiometer_breakpad STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/crashhandler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/crashhandler.h
)

# pri: INCLUDEPATH += $$PWD
# pri: INCLUDEPATH += $$PWD/src
target_include_directories(radiometer_breakpad PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# breakpad 源码使用 QtCore (QString/QProcess/QCoreApplication), pri 无 QT 行,
# 链接 Core 保证头文件与依赖可见。
target_link_libraries(radiometer_breakpad PUBLIC Qt5::Core)

# ---------------------------------------------------------------
# Windows
# pri: win32:HEADERS += ...
# pri: win32:SOURCES += ...
# ---------------------------------------------------------------
if(WIN32)
    target_sources(radiometer_breakpad PRIVATE
        # win32:HEADERS
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common/windows/string_utils-inl.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common/windows/guid_string.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/client/windows/handler/exception_handler.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/client/windows/common/ipc_protocol.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/google_breakpad/common/minidump_format.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/google_breakpad/common/breakpad_types.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/client/windows/crash_generation/crash_generation_client.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common/scoped_ptr.h
        # win32:SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/client/windows/handler/exception_handler.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common/windows/string_utils.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common/windows/guid_string.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/src/client/windows/crash_generation/crash_generation_client.cc
    )
endif()

# ---------------------------------------------------------------
# Linux
# pri: unix:HEADERS += ...
# pri: unix:SOURCES += ...
# pri: unix:QMAKE_CXXFLAGS += -g
# pri: unix:QMAKE_LFLAGS += -z noexecstack
# pri: unix:QMAKE_CFLAGS += -Wa,--noexecstack
# pri: unix:QMAKE_CXXFLAGS += -Wa,--noexecstack
# ---------------------------------------------------------------
if(UNIX)
    # breakpad_getcontext.S 为汇编源文件, 需要启用 ASM 语言
    enable_language(ASM)

    target_sources(radiometer_breakpad PRIVATE
        # unix:HEADERS
        ${CMAKE_CURRENT_SOURCE_DIR}/src/client/linux/minidump_writer/cpu_set.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/client/linux/minidump_writer/proc_cpuinfo_reader.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/client/linux/handler/exception_handler.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/client/linux/crash_generation/crash_generation_client.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/client/linux/handler/minidump_descriptor.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/client/linux/minidump_writer/minidump_writer.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/client/linux/minidump_writer/line_reader.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/client/linux/minidump_writer/linux_dumper.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/client/linux/minidump_writer/linux_ptrace_dumper.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/client/linux/minidump_writer/directory_reader.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/client/linux/log/log.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/client/minidump_file_writer-inl.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/client/minidump_file_writer.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common/linux/linux_libc_support.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common/linux/eintr_wrapper.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common/linux/ignore_ret.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common/linux/file_id.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common/linux/memory_mapped_file.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common/linux/safe_readlink.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common/linux/guid_creator.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common/linux/elfutils.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common/linux/elfutils-inl.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common/linux/elf_gnu_compat.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common/using_std_string.h
        # pri 中此行被注释: #unix:HEADERS += $$PWD/src/common/memory.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common/basictypes.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common/memory_range.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common/string_conversion.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common/convert_UTF.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/google_breakpad/common/minidump_format.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/google_breakpad/common/minidump_size.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/google_breakpad/common/breakpad_types.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common/scoped_ptr.h
        ${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/lss/linux_syscall_support.h
        # unix:SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/src/client/linux/crash_generation/crash_generation_client.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/src/client/linux/handler/exception_handler.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/src/client/linux/handler/minidump_descriptor.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/src/client/linux/minidump_writer/minidump_writer.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/src/client/linux/minidump_writer/linux_dumper.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/src/client/linux/minidump_writer/linux_ptrace_dumper.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/src/client/linux/log/log.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/src/client/minidump_file_writer.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common/linux/linux_libc_support.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common/linux/file_id.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common/linux/memory_mapped_file.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common/linux/safe_readlink.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common/linux/guid_creator.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common/linux/elfutils.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common/string_conversion.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common/convert_UTF.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/src/common/linux/breakpad_getcontext.S
        ${CMAKE_CURRENT_SOURCE_DIR}/src/client/linux/microdump_writer/microdump_writer.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/src/client/linux/dump_writer_common/ucontext_reader.cc
        ${CMAKE_CURRENT_SOURCE_DIR}/src/client/linux/dump_writer_common/thread_info.cc
    )

    # breakpad app need debug info inside binaries (pri 注释, 保留)
    # pri: unix:QMAKE_CXXFLAGS += -g
    # pri: unix:QMAKE_CXXFLAGS += -Wa,--noexecstack
    # pri: unix:QMAKE_CFLAGS += -Wa,--noexecstack
    target_compile_options(radiometer_breakpad PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-g>
        $<$<COMPILE_LANGUAGE:CXX>:-Wa,--noexecstack>
        $<$<COMPILE_LANGUAGE:C>:-Wa,--noexecstack>
    )

    # pri: unix:QMAKE_LFLAGS += -z noexecstack
    # (静态库本身不链接, 经 INTERFACE 传播到最终可执行文件链接;
    #  CMake 3.6 兼容: 不用 target_link_options/SHELL: 前缀)
    target_link_libraries(radiometer_breakpad INTERFACE "-Wl,-z,noexecstack")
endif()
