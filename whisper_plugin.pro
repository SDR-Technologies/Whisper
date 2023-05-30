QT       -= core gui
CONFIG += c++14

TARGET = /opt/vmbase/extensions/whisper
TEMPLATE = lib

LIBS += -lpthread
        QMAKE_CXXFLAGS = -mavx -mavx2 -mfma -mf16c -msse3
        QMAKE_CFLAGS =  -mavx -mavx2 -mfma -mf16c -msse3
        QMAKE_LFLAGS  =  -mavx2 -mavx

SOURCES +=  \
    iqaccumulator.cpp \
    vmwisperinterface.cpp \
    whisper++/ggml.c \
    whisper++/whisper.cpp \
    whisper_plugin.cpp 

HEADERS += \
    iqaccumulator.h \
    vmplugins.h \
    vmsystem.h \
    plugin_factory.h \
    plugin_interface.h \
    vmtoolbox.h \
    vmtypes.h \
    vmwisperinterface.h \
    whisper++/ggml.h \
    whisper++/whisper.h

DISTFILES += \
    README.md \
    js/example.js
