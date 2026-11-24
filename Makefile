CXX     := g++
TARGET  := simple_backup
SRCS    := src/main.cpp src/config.cpp src/configurator.cpp src/destination.cpp src/process.cpp src/watcher.cpp src/copier.cpp \
           src/local_storage.cpp src/logger.cpp

# WebDAV and SFTP destinations come from the remote_files library, found
# through pkg-config. REMOTE_FILES=0 builds without the library, and a config
# naming a server then fails to load. Point PKG_CONFIG_PATH at a remote_files
# deploy, such as ~/remote_files/lib/pkgconfig, to build against one outside /usr.
# SFTP needs a remote_files built with libssh, which remote_files/version.h
# reports as REMOTE_FILES_HAS_SSH.
REMOTE_FILES ?= 1

ifeq ($(REMOTE_FILES),1)
SRCS                  += src/remote_file_storage.cpp
REMOTE_FILES_CFLAGS   := -DSIMPLE_BACKUP_REMOTE_FILES=1 $(shell pkg-config --cflags remote_files 2>/dev/null)
REMOTE_FILES_LIBS     := $(shell pkg-config --libs remote_files 2>/dev/null)
REMOTE_FILES_LIBDIR   := $(shell pkg-config --variable=libdir remote_files 2>/dev/null)
REMOTE_FILES_INCDIR   := $(abspath $(shell pkg-config --variable=includedir remote_files 2>/dev/null))
REMOTE_FILES_VERSION  := $(shell pkg-config --modversion remote_files 2>/dev/null)

# A remote_files outside the system library directories needs a run path, so
# the binary finds the library without LD_LIBRARY_PATH.
ifneq ($(filter /usr/lib% /lib%,$(abspath $(REMOTE_FILES_LIBDIR))),)
REMOTE_FILES_RPATH    :=
else
REMOTE_FILES_RPATH    := -Wl,-rpath,$(abspath $(REMOTE_FILES_LIBDIR))
endif
else
REMOTE_FILES_CFLAGS   := -DSIMPLE_BACKUP_REMOTE_FILES=0
endif

# -MMD -MP writes a .d file per object so a header change rebuilds whatever
# included it. Without it a header edit leaves stale objects behind.
CXXFLAGS_COMMON  := -std=c++17 -Wall -Wextra -I include -MMD -MP $(REMOTE_FILES_CFLAGS)
CXXFLAGS_RELEASE := $(CXXFLAGS_COMMON) -O2 -DNDEBUG
CXXFLAGS_DEBUG   := $(CXXFLAGS_COMMON) -g3 -O0 -DDEBUG -fsanitize=address,undefined
LDFLAGS_DEBUG    := -fsanitize=address,undefined
LDLIBS           := $(REMOTE_FILES_LIBS) $(REMOTE_FILES_RPATH)

OBJS_RELEASE := $(patsubst src/%.cpp, build/release/%.o, $(SRCS))
OBJS_DEBUG   := $(patsubst src/%.cpp, build/debug/%.o,   $(SRCS))

.PHONY: all release debug clean install package check-deps

all: debug

release: check-deps build/release/$(TARGET)
debug:   check-deps build/debug/$(TARGET)

build/release/$(TARGET): $(OBJS_RELEASE) | build/release
	$(CXX) -o $@ $^ $(LDLIBS)

build/debug/$(TARGET): $(OBJS_DEBUG) | build/debug
	$(CXX) $(LDFLAGS_DEBUG) -o $@ $^ $(LDLIBS)

build/release/%.o: src/%.cpp | build/release
	$(CXX) $(CXXFLAGS_RELEASE) -c -o $@ $<

build/debug/%.o: src/%.cpp | build/debug
	$(CXX) $(CXXFLAGS_DEBUG) -c -o $@ $<

build/release build/debug:
	mkdir -p $@

-include $(OBJS_RELEASE:.o=.d) $(OBJS_DEBUG:.o=.d)

clean:
	rm -rf build/ $(DIST_DIR)/

install: release
	install -Dm755 build/release/$(TARGET)              $(DESTDIR)/usr/bin/$(TARGET)
	install -Dm644 systemd/$(TARGET).service             $(DESTDIR)/lib/systemd/system/$(TARGET).service

# ── Debian package ────────────────────────────────────────────────────────────
PKG_NAME    := simple-backup
PKG_VERSION := 1.0.0
PKG_ARCH    := $(shell dpkg --print-architecture 2>/dev/null || echo amd64)
PKG_STEM    := $(PKG_NAME)_$(PKG_VERSION)_$(PKG_ARCH)
PKG_DIR     := build/pkg/$(PKG_STEM)
DIST_DIR    := dist

ifeq ($(REMOTE_FILES),1)
PKG_REMOTE_FILES_DEPENDS := , libremote-files0 (>= $(REMOTE_FILES_VERSION))
else
PKG_REMOTE_FILES_DEPENDS :=
endif

package: release
	rm -rf build/pkg
	mkdir -p $(PKG_DIR)/DEBIAN
	mkdir -p $(PKG_DIR)/usr/bin
	mkdir -p $(PKG_DIR)/lib/systemd/system
	mkdir -p $(PKG_DIR)/etc/simple_backup
	cp build/release/$(TARGET)      $(PKG_DIR)/usr/bin/$(TARGET)
	cp systemd/$(TARGET).service    $(PKG_DIR)/lib/systemd/system/$(TARGET).service
	sed 's/@VERSION@/$(PKG_VERSION)/; s/@ARCH@/$(PKG_ARCH)/; s/@REMOTE_FILES_DEPENDS@/$(PKG_REMOTE_FILES_DEPENDS)/' \
	    debian/control > $(PKG_DIR)/DEBIAN/control
	install -m755 debian/preinst  $(PKG_DIR)/DEBIAN/preinst
	install -m755 debian/postinst $(PKG_DIR)/DEBIAN/postinst
	install -m755 debian/postrm   $(PKG_DIR)/DEBIAN/postrm
	mkdir -p $(DIST_DIR)
	rm -f $(DIST_DIR)/$(PKG_STEM).deb
	dpkg-deb --root-owner-group --build $(PKG_DIR) $(DIST_DIR)/$(PKG_STEM).deb
	@echo ""
	@echo "Package ready: $(DIST_DIR)/$(PKG_STEM).deb"

# ── Dependency check ──────────────────────────────────────────────────────────
check-deps:
	@which $(CXX) > /dev/null 2>&1 || \
	    (echo "ERROR: $(CXX) not found. Install: sudo apt-get install g++"; exit 1)
	@echo 'int main(){}' | $(CXX) -std=c++17 -x c++ - -o /dev/null 2>/dev/null || \
	    (echo "ERROR: g++ does not support C++17"; exit 1)
	@test -f /usr/include/nlohmann/json.hpp || \
	    (echo "ERROR: nlohmann/json not found. Install: sudo apt-get install nlohmann-json3-dev"; exit 1)
ifeq ($(REMOTE_FILES),1)
	@pkg-config --exists remote_files || \
	    (echo "ERROR: remote_files not found by pkg-config. Install libremote-files-dev, set PKG_CONFIG_PATH to a"; \
	     echo "       remote_files deploy such as ~/remote_files/lib/pkgconfig, or build with REMOTE_FILES=0"; exit 1)
	@grep -q 'define REMOTE_FILES_HAS_SSH 1' $(REMOTE_FILES_INCDIR)/remote_files/version.h 2>/dev/null || \
	    echo "WARNING: remote_files at $(REMOTE_FILES_INCDIR) has no SFTP support, so an sftp destination won't load"
	@grep -q 'define REMOTE_FILES_HAS_CURL 1' $(REMOTE_FILES_INCDIR)/remote_files/version.h 2>/dev/null || \
	    echo "WARNING: remote_files at $(REMOTE_FILES_INCDIR) has no WebDAV support, so a webdav destination won't load"
endif
