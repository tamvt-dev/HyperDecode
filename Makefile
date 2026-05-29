CC = gcc
PKG_CONFIG = pkg-config
CFLAGS = -Wall -Wextra -pthread -DVERSION=\"2.0.0\" -D_GNU_SOURCE -O2
LDFLAGS = -lm -lssl -lcrypto -lz

GTK_CFLAGS = $(shell $(PKG_CONFIG) --cflags gtk+-3.0 glib-2.0 2>/dev/null)
GTK_LIBS = $(shell $(PKG_CONFIG) --libs gtk+-3.0 glib-2.0 2>/dev/null)

CFLAGS += $(GTK_CFLAGS)
LDFLAGS += $(GTK_LIBS)

SRCDIR = core/src
INCDIR = core/include
UIDIR = ui
OBJDIR = build/obj
BINDIR = build/bin

CORE_SRC = $(SRCDIR)/core.c $(SRCDIR)/decoder.c $(SRCDIR)/encoder.c \
           $(SRCDIR)/plugin.c $(SRCDIR)/lru_cache.c $(SRCDIR)/logging.c \
           $(SRCDIR)/errors.c $(SRCDIR)/crash_handler.c $(SRCDIR)/theme_manager.c \
           $(SRCDIR)/buffer.c $(SRCDIR)/pipeline.c $(SRCDIR)/score.c \
           $(SRCDIR)/decoders.c $(SRCDIR)/recipe.c \
           $(SRCDIR)/third_party/xxhash.c
PLUGIN_SRC = $(wildcard $(SRCDIR)/plugins/*.c)
UI_SRC = $(UIDIR)/gui.c
MAIN_SRC = cli/main.c

SOURCES = $(CORE_SRC) $(PLUGIN_SRC) $(UI_SRC) $(MAIN_SRC)
OBJECTS = $(SOURCES:%.c=$(OBJDIR)/%.o)

all: $(BINDIR)/auto_decoder_pro

$(BINDIR)/auto_decoder_pro: $(OBJECTS)
	@mkdir -p $(BINDIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
	@echo "✅ Build successful: $@"

$(OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(INCDIR) -I$(UIDIR) -c $< -o $@

clean:
	@echo "Cleaning..."
	rm -rf $(OBJDIR) $(BINDIR)
	@echo "✅ Clean complete"

.PHONY: all clean