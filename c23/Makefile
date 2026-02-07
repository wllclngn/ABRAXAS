# ABRAXAS - Dynamic color temperature daemon
# Statically links libmeridian.a for a single binary.
# Backend libraries (X11, GNOME, Wayland) loaded at runtime via dlopen.
#
# NOAA=0 disables weather features (no dependencies removed -- uses curl(1)).

CC       := gcc
CFLAGS   := -std=c2x -O2 -march=native -MMD -MP -Wall -Wextra -Wpedantic
CFLAGS   += -fstack-protector-strong -fstack-clash-protection -fcf-protection
CFLAGS   += -D_FORTIFY_SOURCE=3
CFLAGS   += -flto=auto -ffunction-sections -fdata-sections
CFLAGS   += -Iinclude -Ilibmeridian/include
LDFLAGS  := -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack
LDFLAGS  += -flto=auto -Wl,--gc-sections

BUILDDIR := build
SOURCES  := src/main.c src/json.c src/solar.c src/sigmoid.c \
            src/zipdb.c src/config.c src/weather.c src/daemon.c \
            src/uring.c src/seccomp.c src/landlock.c
OBJECTS  := $(patsubst src/%.c,$(BUILDDIR)/%.o,$(SOURCES))
TARGET   := abraxas

# libmeridian static archive
LIBM_DIR := libmeridian
LIBM_A   := $(LIBM_DIR)/libmeridian.a

# NOAA weather support (disable with NOAA=0 for non-US builds)
NOAA     ?= 1
LIBS     := -lm
ifeq ($(NOAA),0)
    CFLAGS += -DNOAA_DISABLED
endif

# Wayland plugin (built by libmeridian, copied alongside binary)
WL_AVAILABLE := $(shell pkg-config --exists wayland-client 2>/dev/null && which wayland-scanner >/dev/null 2>&1 && echo yes || echo no)
ifeq ($(WL_AVAILABLE),yes)
    WL_PLUGIN := meridian_wl.so
    LDFLAGS += -Wl,-rpath,'$$ORIGIN'
else
    WL_PLUGIN :=
endif

LIBM_EXTRA :=

.PHONY: all clean libmeridian static

all: libmeridian $(BUILDDIR) $(TARGET) $(WL_PLUGIN)

libmeridian:
	$(MAKE) -C $(LIBM_DIR) CC="$(CC)" $(LIBM_EXTRA) static

ifeq ($(WL_AVAILABLE),yes)
meridian_wl.so: $(LIBM_DIR)/meridian_wl.so
	cp $< $@
endif

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(TARGET): $(OBJECTS) $(LIBM_A)
	$(CC) $(LDFLAGS) -o $@ $(OBJECTS) $(LIBM_A) $(LIBS)

$(BUILDDIR)/%.o: src/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

-include $(OBJECTS:.o=.d)

# Static musl build: DRM-only, zero shared libs
# Requires: pacman -S musl
static: CC := musl-gcc
static: CFLAGS := -std=c2x -Os -MMD -MP -Wall -Wextra -Wpedantic
static: CFLAGS += -fstack-protector-strong
static: CFLAGS += -flto=auto -ffunction-sections -fdata-sections
static: CFLAGS += -DABXS_STATIC -Iinclude -Ilibmeridian/include -idirafter /usr/include
static: LDFLAGS := -static -flto=auto -Wl,--gc-sections
static: LIBS := -lm
static: LIBM_EXTRA := ABXS_STATIC=1
static: libmeridian $(BUILDDIR) $(TARGET)
	strip $(TARGET)
	@echo ""
	@echo "Static build complete:"
	@ls -la $(TARGET)
	@echo ""
	@file $(TARGET)

clean:
	rm -rf $(BUILDDIR) $(TARGET) meridian_wl.so
	$(MAKE) -C $(LIBM_DIR) clean
