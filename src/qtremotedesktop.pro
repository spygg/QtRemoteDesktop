QT += core widgets network websockets


# DEFINES += USE_FFMPEG


SOURCES +=\
    $$PWD/main.cpp \
    $$PWD/server/inputmanager.cpp \
    $$PWD/server/rdpserver.cpp \
    $$PWD/server/screencapturer.cpp \
    $$PWD/server/websocketserver.cpp

HEADERS += \
    server/inputmanager.h \
    server/rdpserver.h \
    server/screencapturer.h \
    server/websocketserver.h

RESOURCES += \
    resources.qrc

contains(DEFINES, USE_FFMPEG){
    HEADERS += \
    server/videoencoder.h

    SOURCES +=\
       $$PWD/server/videoencoder.cpp

    win32{
        LIBS+=\
            $$PWD/thridparty/ffmpeg/windows/lib/avcodec.lib\
            $$PWD/thridparty/ffmpeg/windows/lib/avutil.lib\
            $$PWD/thridparty/ffmpeg/windows/lib/swscale.lib

        INCLUDEPATH +=$$PWD/thridparty/ffmpeg/windows/include
    }

    unix{
        LIBS+= -L$$PWD/thridparty/ffmpeg/linux/lib -lavcodec -lavutil -lswscale

        INCLUDEPATH +=$$PWD/thridparty/ffmpeg/linux/include
    }

}


win32{
    SOURCES +=\
        server/screencapturer_win.cpp

    LIBS += -ld3d11 -ldxgi -lgdi32 -luser32
}

unix{
    SOURCES +=\
        server/screencapturer_linux.cpp

    LIBS += -lX11  -lXtst -lXdamage -lXcomposite -lXrender
}


DESTDIR = $$PWD/../bin

msvc{
    QMAKE_CXXFLAGS += -execution-charset:utf-8
    QMAKE_CXXFLAGS += -source-charset:utf-8
    QMAKE_CXXFLAGS -= -Zc:strictStrings
    # QMAKE_CXXFLAGS += -Ze
    DEFINES += _CRT_SECURE_NO_WARNINGS #禁用 strcpy之类的警告
}
