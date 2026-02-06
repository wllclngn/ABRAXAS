# libmeridian - Gamma control library for Linux
# Named for the solar meridian - when sun crosses your longitude
#
# Backends (auto-detected via pkg-config):
#   DRM:     always compiled (no dependencies beyond libc)
#   Wayland: requires wayland-client, wayland-scanner
#   GNOME:   requires libsystemd (sd-bus)
#   X11:     requires libX11, libXrandr

VERSION  := 1.0.0

CC       := gcc
CFLAGS   := -std=c2x -O2 -march=native -fPIC -Wall -Wextra -Wpedantic
CFLAGS   += -I./include
LDFLAGS  := -shared
LIBS     := -lm

SRCDIR   := src
INCDIR   := include
BUILDDIR := build

# Protocol XML
WLR_PROTO_XML := $(SRCDIR)/wayland/wlr-gamma-control-unstable-v1.xml

# Core sources (always built)
SOURCES  := $(SRCDIR)/colorramp.c $(SRCDIR)/gamma_drm.c $(SRCDIR)/gamma_auto.c

# ============================================================
# Auto-detect optional backends
# ============================================================

# Wayland: wayland-client + wayland-scanner
WL_AVAILABLE := $(shell pkg-config --exists wayland-client 2>/dev/null && which wayland-scanner >/dev/null 2>&1 && echo yes || echo no)

ifeq ($(WL_AVAILABLE),yes)
    WAYLAND_SCANNER := $(shell which wayland-scanner)
    CFLAGS  += -DMERIDIAN_HAS_WAYLAND $(shell pkg-config --cflags wayland-client) -I$(BUILDDIR)
    LIBS    += $(shell pkg-config --libs wayland-client)
    SOURCES += $(SRCDIR)/gamma_wl.c
    WL_STATUS := enabled
    # Generated protocol files
    WL_PROTO_H := $(BUILDDIR)/wlr-gamma-control-unstable-v1-client-protocol.h
    WL_PROTO_C := $(BUILDDIR)/wlr-gamma-control-unstable-v1-protocol.c
    WL_PROTO_O := $(BUILDDIR)/wlr-gamma-control-unstable-v1-protocol.o
else
    WL_STATUS := disabled (install wayland-client, wayland-scanner)
    WL_PROTO_H :=
    WL_PROTO_C :=
    WL_PROTO_O :=
endif

# GNOME: libsystemd (for sd-bus)
GNOME_AVAILABLE := $(shell pkg-config --exists libsystemd 2>/dev/null && echo yes || echo no)

ifeq ($(GNOME_AVAILABLE),yes)
    CFLAGS  += -DMERIDIAN_HAS_GNOME $(shell pkg-config --cflags libsystemd)
    LIBS    += $(shell pkg-config --libs libsystemd)
    SOURCES += $(SRCDIR)/gamma_gnome.c
    GNOME_STATUS := enabled
else
    GNOME_STATUS := disabled (install libsystemd-dev)
endif

# X11: libX11 + libXrandr
X11_AVAILABLE := $(shell pkg-config --exists x11 xrandr 2>/dev/null && echo yes || echo no)

ifeq ($(X11_AVAILABLE),yes)
    CFLAGS  += -DMERIDIAN_HAS_X11 $(shell pkg-config --cflags x11 xrandr)
    LIBS    += $(shell pkg-config --libs x11 xrandr)
    SOURCES += $(SRCDIR)/gamma_x11.c
    X11_STATUS := enabled
else
    X11_STATUS := disabled (install libx11-dev libxrandr-dev)
endif

# ============================================================
# Object files
# ============================================================

OBJECTS  := $(patsubst $(SRCDIR)/%.c,$(BUILDDIR)/%.o,$(SOURCES))
# Add Wayland protocol object if available
OBJECTS  += $(WL_PROTO_O)
TARGET   := libmeridian.so

.PHONY: all clean install test info

all: info $(BUILDDIR) $(TARGET)

info:
	@echo "Building libmeridian.so"
	@echo "  Wayland backend: $(WL_STATUS)"
	@echo "  GNOME backend:   $(GNOME_STATUS)"
	@echo "  X11 backend:     $(X11_STATUS)"
	@echo "  DRM backend:     always enabled"
	@echo ""

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

# ============================================================
# Wayland protocol generation
# ============================================================

ifeq ($(WL_AVAILABLE),yes)
$(WL_PROTO_H): $(WLR_PROTO_XML) | $(BUILDDIR)
	$(WAYLAND_SCANNER) client-header $< $@

$(WL_PROTO_C): $(WLR_PROTO_XML) | $(BUILDDIR)
	$(WAYLAND_SCANNER) private-code $< $@

$(WL_PROTO_O): $(WL_PROTO_C) $(WL_PROTO_H)
	$(CC) -std=c2x -O2 -fPIC -Wall -I$(BUILDDIR) -c $< -o $@

# gamma_wl.c depends on generated header
$(BUILDDIR)/gamma_wl.o: $(SRCDIR)/gamma_wl.c $(INCDIR)/meridian.h $(WL_PROTO_H)
	$(CC) $(CFLAGS) -c $< -o $@
endif

# ============================================================
# Main build rules
# ============================================================

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

$(BUILDDIR)/%.o: $(SRCDIR)/%.c $(INCDIR)/meridian.h | $(BUILDDIR)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILDDIR) $(TARGET)

install: $(TARGET)
	install -Dm755 $(TARGET) /usr/local/lib/$(TARGET)
	install -Dm644 $(INCDIR)/meridian.h /usr/local/include/meridian.h
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
