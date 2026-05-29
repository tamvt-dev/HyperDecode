CC = gcc

CFLAGS = -Wall -Wextra -O2 -Wno-cast-function-type \
	-I../core/include \
	-IC:/msys64/mingw64/include/glib-2.0 \
	-IC:/msys64/mingw64/lib/glib-2.0/include

LDFLAGS = -LC:/msys64/mingw64/lib -lglib-2.0 -lintl -liconv -lssl -lcrypto -lz

SOURCES = \
	main.c \
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

TARGET = bin/hyperdecode.exe

all: $(TARGET)

$(TARGET): $(SOURCES)
	@if not exist bin mkdir bin
	$(CC) $(CFLAGS) $(SOURCES) $(LDFLAGS) -o $(TARGET)

clean:
	@if exist bin\\hyperdecode.exe del /f /q bin\\hyperdecode.exe

.PHONY: all clean
