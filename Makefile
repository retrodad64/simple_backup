CXX     := g++
TARGET  := simple_backup
SRCS    := src/main.cpp src/config.cpp src/watcher.cpp src/copier.cpp src/logger.cpp

CXXFLAGS_COMMON  := -std=c++17 -Wall -Wextra -I include
CXXFLAGS_RELEASE := $(CXXFLAGS_COMMON) -O2 -DNDEBUG
CXXFLAGS_DEBUG   := $(CXXFLAGS_COMMON) -g3 -O0 -DDEBUG -fsanitize=address,undefined
LDFLAGS_DEBUG    := -fsanitize=address,undefined

OBJS_RELEASE := $(patsubst src/%.cpp, build/release/%.o, $(SRCS))
OBJS_DEBUG   := $(patsubst src/%.cpp, build/debug/%.o,   $(SRCS))

.PHONY: all release debug clean install package check-deps

all: debug

release: check-deps build/release/$(TARGET)
debug:   check-deps build/debug/$(TARGET)

build/release/$(TARGET): $(OBJS_RELEASE) | build/release
	$(CXX) -o $@ $^

build/debug/$(TARGET): $(OBJS_DEBUG) | build/debug
	$(CXX) $(LDFLAGS_DEBUG) -o $@ $^

build/release/%.o: src/%.cpp | build/release
	$(CXX) $(CXXFLAGS_RELEASE) -c -o $@ $<

build/debug/%.o: src/%.cpp | build/debug
	$(CXX) $(CXXFLAGS_DEBUG) -c -o $@ $<

build/release build/debug:
	mkdir -p $@

clean:
	rm -rf build/

install: release
	install -Dm755 build/release/$(TARGET)              $(DESTDIR)/usr/bin/$(TARGET)
	install -Dm644 systemd/$(TARGET).service             $(DESTDIR)/lib/systemd/system/$(TARGET).service

# ── Debian package ────────────────────────────────────────────────────────────
PKG_NAME    := simple-backup
PKG_VERSION := 1.0.0
PKG_ARCH    := $(shell dpkg --print-architecture 2>/dev/null || echo amd64)
PKG_STEM    := $(PKG_NAME)_$(PKG_VERSION)_$(PKG_ARCH)
PKG_DIR     := build/pkg/$(PKG_STEM)

package: release
	rm -rf build/pkg
	mkdir -p $(PKG_DIR)/DEBIAN
	mkdir -p $(PKG_DIR)/usr/bin
	mkdir -p $(PKG_DIR)/lib/systemd/system
	mkdir -p $(PKG_DIR)/etc/simple_backup
	cp build/release/$(TARGET)      $(PKG_DIR)/usr/bin/$(TARGET)
	cp systemd/$(TARGET).service    $(PKG_DIR)/lib/systemd/system/$(TARGET).service
	sed 's/@VERSION@/$(PKG_VERSION)/; s/@ARCH@/$(PKG_ARCH)/' \
	    debian/control > $(PKG_DIR)/DEBIAN/control
	install -m755 debian/preinst  $(PKG_DIR)/DEBIAN/preinst
	install -m755 debian/postinst $(PKG_DIR)/DEBIAN/postinst
	install -m755 debian/postrm   $(PKG_DIR)/DEBIAN/postrm
	dpkg-deb --root-owner-group --build $(PKG_DIR) build/pkg/$(PKG_STEM).deb
	@echo ""
	@echo "Package ready: build/pkg/$(PKG_STEM).deb"

# ── Dependency check ──────────────────────────────────────────────────────────
check-deps:
	@which $(CXX) > /dev/null 2>&1 || \
	    (echo "ERROR: $(CXX) not found. Install: sudo apt-get install g++"; exit 1)
	@echo 'int main(){}' | $(CXX) -std=c++17 -x c++ - -o /dev/null 2>/dev/null || \
	    (echo "ERROR: g++ does not support C++17"; exit 1)
	@test -f /usr/include/nlohmann/json.hpp || \
	    (echo "ERROR: nlohmann/json not found. Install: sudo apt-get install nlohmann-json3-dev"; exit 1)
