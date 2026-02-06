# ABRAXAS - Dynamic color temperature daemon
# Statically links libmeridian.a for a single binary.

CC       := gcc
CFLAGS   := -std=c2x -O2 -march=native -Wall -Wextra -Wpedantic
CFLAGS   += -Iinclude -Ilibmeridian/include

BUILDDIR := build
SOURCES  := src/main.c src/json.c src/solar.c src/sigmoid.c \
            src/zipdb.c src/config.c src/weather.c src/daemon.c
OBJECTS  := $(patsubst src/%.c,$(BUILDDIR)/%.o,$(SOURCES))
TARGET   := abraxas

# libmeridian static archive
LIBM_DIR := libmeridian
LIBM_A   := $(LIBM_DIR)/libmeridian.a

# Libraries
LIBS     := -lcurl -lm

# Backend libs from libmeridian's dependencies (same detection)
WL_AVAILABLE    := $(shell pkg-config --exists wayland-client 2>/dev/null && echo yes || echo no)
GNOME_AVAILABLE := $(shell pkg-config --exists libsystemd 2>/dev/null && echo yes || echo no)
X11_AVAILABLE   := $(shell pkg-config --exists x11 xrandr 2>/dev/null && echo yes || echo no)

ifeq ($(WL_AVAILABLE),yes)
    LIBS += $(shell pkg-config --libs wayland-client)
endif
ifeq ($(GNOME_AVAILABLE),yes)
    LIBS += $(shell pkg-config --libs libsystemd)
endif
ifeq ($(X11_AVAILABLE),yes)
    LIBS += $(shell pkg-config --libs x11 xrandr)
endif

.PHONY: all clean libmeridian

all: libmeridian $(BUILDDIR) $(TARGET)

libmeridian:
	$(MAKE) -C $(LIBM_DIR) static

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(TARGET): $(OBJECTS) $(LIBM_A)
	$(CC) -o $@ $(OBJECTS) $(LIBM_A) $(LIBS)

$(BUILDDIR)/%.o: src/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILDDIR) $(TARGET)
	$(MAKE) -C $(LIBM_DIR) clean
