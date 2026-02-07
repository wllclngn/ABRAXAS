# libmeridian - Gamma control library for Linux
# Named for the solar meridian - when sun crosses your longitude
#
# Backends (auto-detected via pkg-config):
#   DRM:     always compiled (no dependencies beyond libc)
#   Wayland: separate .so plugin (links -lwayland-client)
#   GNOME:   libsystemd loaded at runtime via dlopen
#   X11:     libX11/libXrandr loaded at runtime via dlopen

VERSION  := 1.0.0

CC       := gcc
AR       := gcc-ar
CFLAGS   := -std=c2x -O2 -march=native -fPIC -MMD -MP -Wall -Wextra -Wpedantic
CFLAGS   += -fstack-protector-strong -fstack-clash-protection -fcf-protection
CFLAGS   += -D_FORTIFY_SOURCE=3
CFLAGS   += -flto=auto -ffunction-sections -fdata-sections
CFLAGS   += -I./include
LDFLAGS  := -shared -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack
LDFLAGS  += -flto=auto -Wl,--gc-sections
LIBS     := -lm

SRCDIR   := src
INCDIR   := include
BUILDDIR := build

# Protocol XML
WLR_PROTO_XML := $(SRCDIR)/wayland/wlr-gamma-control-unstable-v1.xml

# Core sources (always built into libmeridian.a)
SOURCES  := $(SRCDIR)/colorramp.c $(SRCDIR)/gamma_drm.c $(SRCDIR)/gamma_auto.c

# ============================================================
# Auto-detect optional backends
# ============================================================

# Static build: DRM-only, no dlopen backends
ifdef ABXS_STATIC
    CFLAGS += -DABXS_STATIC -idirafter /usr/include
    WL_AVAILABLE := no
    GNOME_AVAILABLE := no
    X11_AVAILABLE := no
else
    # Wayland: compiled as meridian_wl.so plugin (not linked into .a)
    WL_AVAILABLE := $(shell pkg-config --exists wayland-client 2>/dev/null && which wayland-scanner >/dev/null 2>&1 && echo yes || echo no)
    # GNOME: headers for compilation, library loaded at runtime via dlopen
    GNOME_AVAILABLE := $(shell pkg-config --exists libsystemd 2>/dev/null && echo yes || echo no)
    # X11: headers for compilation, libraries loaded at runtime via dlopen
    X11_AVAILABLE := $(shell pkg-config --exists x11 xrandr 2>/dev/null && echo yes || echo no)
endif

ifeq ($(WL_AVAILABLE),yes)
    WAYLAND_SCANNER := $(shell which wayland-scanner)
    CFLAGS  += -DMERIDIAN_HAS_WAYLAND
    WL_STATUS := enabled (plugin)
    WL_PLUGIN := meridian_wl.so
    WL_PROTO_H := $(BUILDDIR)/wlr-gamma-control-unstable-v1-client-protocol.h
    WL_PROTO_C := $(BUILDDIR)/wlr-gamma-control-unstable-v1-protocol.c
    WL_PROTO_O := $(BUILDDIR)/wlr-gamma-control-unstable-v1-protocol.o
else
    WL_STATUS := disabled (install wayland-client, wayland-scanner)
    WL_PLUGIN :=
    WL_PROTO_H :=
    WL_PROTO_C :=
    WL_PROTO_O :=
endif

ifeq ($(GNOME_AVAILABLE),yes)
    CFLAGS  += -DMERIDIAN_HAS_GNOME $(shell pkg-config --cflags libsystemd)
    SOURCES += $(SRCDIR)/gamma_gnome.c
    GNOME_STATUS := enabled (dlopen)
else
    GNOME_STATUS := disabled (install libsystemd-dev)
endif

ifeq ($(X11_AVAILABLE),yes)
    CFLAGS  += -DMERIDIAN_HAS_X11 $(shell pkg-config --cflags x11 xrandr)
    SOURCES += $(SRCDIR)/gamma_x11.c
    X11_STATUS := enabled (dlopen)
else
    X11_STATUS := disabled (install libx11-dev libxrandr-dev)
endif

# ============================================================
# Object files
# ============================================================

OBJECTS  := $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(SOURCES))
TARGET   := libmeridian.so

.PHONY: all static clean install test info

all: info $(BUILDDIR) $(TARGET) $(WL_PLUGIN)

static: info $(BUILDDIR) libmeridian.a $(WL_PLUGIN)

libmeridian.a: $(OBJECTS)
	$(AR) rcs $@ $^

info:
	@echo "Building libmeridian"
	@echo "  Wayland backend: $(WL_STATUS)"
	@echo "  GNOME backend:   $(GNOME_STATUS)"
	@echo "  X11 backend:     $(X11_STATUS)"
	@echo "  DRM backend:     always enabled"
	@echo ""

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

# ============================================================
# Wayland plugin (.so linking libwayland-client)
# ============================================================

ifeq ($(WL_AVAILABLE),yes)
$(WL_PROTO_H): $(WLR_PROTO_XML) | $(BUILDDIR)
	$(WAYLAND_SCANNER) client-header $< $@

$(WL_PROTO_C): $(WLR_PROTO_XML) | $(BUILDDIR)
	$(WAYLAND_SCANNER) private-code $< $@

$(WL_PROTO_O): $(WL_PROTO_C) $(WL_PROTO_H)
	$(CC) -std=c2x -O2 -fPIC -Wall -fstack-protector-strong -fstack-clash-protection -fcf-protection -I$(BUILDDIR) -c $< -o $@

$(BUILDDIR)/gamma_wl.o: $(SRCDIR)/gamma_wl.c $(INCDIR)/meridian.h $(WL_PROTO_H)
	$(CC) $(CFLAGS) $(shell pkg-config --cflags wayland-client) -I$(BUILDDIR) -c $< -o $@

meridian_wl.so: $(BUILDDIR)/gamma_wl.o $(WL_PROTO_O)
	$(CC) -shared -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack -o $@ $^ $(shell pkg-config --libs wayland-client) -lm
endif

# ============================================================
# Main build rules
# ============================================================

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

-include $(OBJECTS:.o=.d)

clean:
	rm -rf $(BUILDDIR) $(TARGET) libmeridian.a meridian_wl.so

install: $(TARGET) $(WL_PLUGIN)
	install -Dm755 $(TARGET) /usr/local/lib/$(TARGET)
	install -Dm644 $(INCDIR)/meridian.h /usr/local/include/meridian.h
ifeq ($(WL_AVAILABLE),yes)
	install -Dm755 meridian_wl.so /usr/local/lib/meridian_wl.so
endif
	ldconfig

# Test unified API
test: $(TARGET)
	@echo '#include <stdio.h>' > /tmp/test_meridian.c
	@echo '#include <unistd.h>' >> /tmp/test_meridian.c
	@echo '#include "meridian.h"' >> /tmp/test_meridian.c
	@echo 'int main() {' >> /tmp/test_meridian.c
	@echo '    meridian_state_t *state;' >> /tmp/test_meridian.c
	@echo '    meridian_error_t err = meridian_init(&state);' >> /tmp/test_meridian.c
	@echo '    if (err != MERIDIAN_OK) {' >> /tmp/test_meridian.c
	@echo '        printf("Init failed: %s\\n", meridian_strerror(err));' >> /tmp/test_meridian.c
	@echo '        return 1;' >> /tmp/test_meridian.c
	@echo '    }' >> /tmp/test_meridian.c
	@echo '    printf("Backend: %s\\n", meridian_get_backend_name(state));' >> /tmp/test_meridian.c
	@echo '    printf("CRTCs: %d\\n", meridian_get_crtc_count(state));' >> /tmp/test_meridian.c
	@echo '    for (int i = 0; i < meridian_get_crtc_count(state); i++) {' >> /tmp/test_meridian.c
	@echo '        printf("  CRTC %d: gamma_size=%d\\n", i, meridian_get_gamma_size(state, i));' >> /tmp/test_meridian.c
	@echo '    }' >> /tmp/test_meridian.c
	@echo '    printf("\\nSetting 4500K for 2 seconds...\\n");' >> /tmp/test_meridian.c
	@echo '    err = meridian_set_temperature(state, 4500, 1.0);' >> /tmp/test_meridian.c
	@echo '    if (err != MERIDIAN_OK) {' >> /tmp/test_meridian.c
	@echo '        printf("Set failed: %s\\n", meridian_strerror(err));' >> /tmp/test_meridian.c
	@echo '    } else {' >> /tmp/test_meridian.c
	@echo '        sleep(2);' >> /tmp/test_meridian.c
	@echo '    }' >> /tmp/test_meridian.c
	@echo '    printf("Restoring...\\n");' >> /tmp/test_meridian.c
	@echo '    meridian_free(state);' >> /tmp/test_meridian.c
	@echo '    printf("Done.\\n");' >> /tmp/test_meridian.c
	@echo '    return 0;' >> /tmp/test_meridian.c
	@echo '}' >> /tmp/test_meridian.c
	$(CC) -I$(INCDIR) -L. -Wl,-rpath,. /tmp/test_meridian.c -lmeridian -o /tmp/test_meridian
	/tmp/test_meridian
