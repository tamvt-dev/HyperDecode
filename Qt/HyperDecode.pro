TEMPLATE = app
TARGET = HyperDecode
QT += core widgets gui concurrent
CONFIG += c++11 console

# Force MinGW to avoid toolchain mismatch.
QMAKE_CC = gcc
QMAKE_CXX = g++

# Include paths
INCLUDEPATH += ../core/include
INCLUDEPATH += C:/msys64/mingw64/include/glib-2.0
INCLUDEPATH += C:/msys64/mingw64/lib/glib-2.0/include
INCLUDEPATH += C:/msys64/mingw64/include/json-glib-1.0

# Core sources
SOURCES += \
    ../core/src/core.c \
    ../core/src/decoder.c \
    ../core/src/encoder.c \
    ../core/src/plugin.c \
    ../core/src/lru_cache.c \
    ../core/src/logging.c \
    ../core/src/errors.c \
    ../core/src/crash_handler.c \
    ../core/src/score.c \
    ../core/src/buffer.c \
    ../core/src/pipeline.c \
    ../core/src/recipe.c

# Plugins
SOURCES += \
    ../core/src/plugins/aes_plugin.c \
    ../core/src/plugins/atbash_plugin.c \
    ../core/src/plugins/base64_plugin.c \
    ../core/src/plugins/caesar_plugin.c \
    ../core/src/plugins/gzip_plugin.c \
    ../core/src/plugins/rot13_plugin.c \
    ../core/src/plugins/scramble_plugin.c \
    ../core/src/plugins/sha256_plugin.c \
    ../core/src/plugins/url_plugin.c \
    ../core/src/plugins/xor_plugin.c

# Qt sources
SOURCES += \
    main.cpp \
    mainwindow.cpp \
    console_manager.cpp \
    decoder_engine.cpp \
    history_manager.cpp \
    history_tab.cpp \
    notification_widget.cpp \
    notification_manager.cpp \
    recipe_manager.cpp \
    recipe_tab.cpp \
    arg_editor_dialog.cpp \
    batch_processor.cpp

HEADERS += \
    mainwindow.h \
    console_manager.h \
    history_manager.h \
    history_tab.h \
    notification_widget.h \
    notification_manager.h \
    recipe_manager.h \
    recipe_tab.h \
    arg_editor_dialog.h \
    batch_processor.h

RESOURCES += resources.qrc
RC_ICONS = icons/app.ico

# Silence GLib function-pointer cast warnings on MinGW.
QMAKE_CFLAGS += -Wno-cast-function-type

# Link GLib from the local MSYS2 mingw64 environment.
LIBS += -LC:/msys64/mingw64/lib -lglib-2.0 -lgobject-2.0 -lintl -liconv -ljson-glib-1.0

# Link OpenSSL and zlib for crypto plugins
LIBS += -lssl -lcrypto -lz

# Debug / Release
CONFIG(debug, debug|release) {
    DEFINES += DEBUG
}
CONFIG(release, debug|release) {
    DEFINES += NDEBUG
}
