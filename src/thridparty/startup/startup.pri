INCLUDEPATH += $$PWD

HEADERS += \
    $$PWD/startup.h

SOURCES += \
    $$PWD/startup.cpp


win32{
SOURCES += \
    $$PWD/startup_windows.cpp
}

unix{
SOURCES += \
    $$PWD/startup_linux.cpp
}



win32{
   LIBS += -lole32
   LIBS += -luuid
}
