PREFIX ?= /usr/local
DESTDIR ?=
BINDIR ?= $(PREFIX)/bin
LIBDIR ?= $(PREFIX)/lib
INCLUDEDIR ?= $(PREFIX)/include
PKGCONFIGDIR ?= $(LIBDIR)/pkgconfig

BUILD_DIR ?= build

CXX ?= g++
AR ?= ar
QT_HOST_BINS := $(shell pkg-config --variable=host_bins Qt6Core 2>/dev/null)
QT_LIBEXECDIR := $(shell pkg-config --variable=libexecdir Qt6Core 2>/dev/null)
MOC := $(if $(QT_HOST_BINS),$(QT_HOST_BINS)/moc,$(if $(QT_LIBEXECDIR),$(QT_LIBEXECDIR)/moc,moc))

QT_PACKAGES := Qt6Core Qt6Bluetooth Qt6DBus Qt6Network
QT_CFLAGS := $(shell pkg-config --cflags $(QT_PACKAGES))
QT_LIBS_CORE := $(shell pkg-config --libs Qt6Core Qt6Bluetooth Qt6Network)
QT_LIBS_CLI := $(shell pkg-config --libs Qt6Core Qt6Bluetooth Qt6DBus Qt6Network)

CXXSTD := -std=c++17
WARNFLAGS := -Wall -Wextra -Wpedantic
CPPFLAGS += -Iinclude -I. $(QT_CFLAGS)
CXXFLAGS ?= -O2 -g
ARFLAGS ?= rcs

CORE_OBJS := \
	$(BUILD_DIR)/src/core/airpods_core_client.o \
	$(BUILD_DIR)/moc/moc_airpods_core_client.o \
	$(BUILD_DIR)/moc/moc_battery.o \
	$(BUILD_DIR)/moc/moc_eardetection.o

CLI_OBJS := $(BUILD_DIR)/src/cli/main.o
DAEMON_OBJS := $(BUILD_DIR)/src/cli/main_daemon.o

LIB_TARGET := $(BUILD_DIR)/vibepods_core.a
CLI_TARGET := $(BUILD_DIR)/vibepods-cli
DAEMON_TARGET := $(BUILD_DIR)/vibepods-daemon

.PHONY: all lib cli daemon clean install
.NOTPARALLEL:

all: lib cli daemon

lib: $(LIB_TARGET)

cli: $(CLI_TARGET)

daemon: $(DAEMON_TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/src/core:
	mkdir -p $(BUILD_DIR)/src/core

$(BUILD_DIR)/src/cli:
	mkdir -p $(BUILD_DIR)/src/cli

$(BUILD_DIR)/moc:
	mkdir -p $(BUILD_DIR)/moc

$(BUILD_DIR)/src/core/airpods_core_client.o: src/core/airpods_core_client.cpp | $(BUILD_DIR)/src/core
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CXXSTD) $(WARNFLAGS) -fPIC -c $< -o $@

$(BUILD_DIR)/src/cli/main.o: src/cli/main.cpp | $(BUILD_DIR)/src/cli
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CXXSTD) $(WARNFLAGS) -fPIC -c $< -o $@

$(BUILD_DIR)/src/cli/main_daemon.o: src/cli/main.cpp | $(BUILD_DIR)/src/cli
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CXXSTD) $(WARNFLAGS) -DVIBEPODS_BUILD_DAEMON -fPIC -c $< -o $@

$(BUILD_DIR)/moc/moc_airpods_core_client.cpp: include/librepods/core/airpods_core_client.h | $(BUILD_DIR)/moc
	$(MOC) $< -o $@

$(BUILD_DIR)/moc/moc_battery.cpp: include/librepods/battery.hpp | $(BUILD_DIR)/moc
	$(MOC) $< -o $@

$(BUILD_DIR)/moc/moc_eardetection.cpp: include/librepods/eardetection.hpp | $(BUILD_DIR)/moc
	$(MOC) $< -o $@

$(BUILD_DIR)/moc/moc_airpods_core_client.o: $(BUILD_DIR)/moc/moc_airpods_core_client.cpp | $(BUILD_DIR)/moc
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CXXSTD) $(WARNFLAGS) -fPIC -c $< -o $@

$(BUILD_DIR)/moc/moc_battery.o: $(BUILD_DIR)/moc/moc_battery.cpp | $(BUILD_DIR)/moc
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CXXSTD) $(WARNFLAGS) -fPIC -c $< -o $@

$(BUILD_DIR)/moc/moc_eardetection.o: $(BUILD_DIR)/moc/moc_eardetection.cpp | $(BUILD_DIR)/moc
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CXXSTD) $(WARNFLAGS) -fPIC -c $< -o $@

$(LIB_TARGET): $(CORE_OBJS) | $(BUILD_DIR)
	$(AR) $(ARFLAGS) $@ $^

$(CLI_TARGET): $(CLI_OBJS) $(LIB_TARGET) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CXXSTD) $(WARNFLAGS) $^ -o $@ $(QT_LIBS_CLI)

$(DAEMON_TARGET): $(DAEMON_OBJS) $(LIB_TARGET) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(CXXSTD) $(WARNFLAGS) $^ -o $@ $(QT_LIBS_CLI)

install: all
	install -d "$(DESTDIR)$(LIBDIR)"
	install -d "$(DESTDIR)$(BINDIR)"
	install -d "$(DESTDIR)$(INCLUDEDIR)"
	install -d "$(DESTDIR)$(PKGCONFIGDIR)"
	install -m644 "$(LIB_TARGET)" "$(DESTDIR)$(LIBDIR)/vibepods_core.a"
	install -m755 "$(CLI_TARGET)" "$(DESTDIR)$(BINDIR)/vibepods-cli"
	install -m755 "$(DAEMON_TARGET)" "$(DESTDIR)$(BINDIR)/vibepods-daemon"
	cp -r include/librepods "$(DESTDIR)$(INCLUDEDIR)/"
	@{ \
		echo "prefix=$(PREFIX)"; \
		echo "exec_prefix=\$${prefix}"; \
		echo "libdir=\$${exec_prefix}/lib"; \
		echo "includedir=\$${prefix}/include"; \
		echo ""; \
		echo "Name: vibepods-cli"; \
		echo "Description: VibePods CLI core static library"; \
		echo "Version: 0.1.0"; \
		echo "Requires: Qt6Core Qt6Bluetooth"; \
		echo "Libs: -L\$${libdir} -lvibepods_core"; \
		echo "Cflags: -I\$${includedir}"; \
	} > "$(BUILD_DIR)/vibepods.pc"
	install -m644 "$(BUILD_DIR)/vibepods.pc" "$(DESTDIR)$(PKGCONFIGDIR)/vibepods.pc"

clean:
	rm -rf "$(BUILD_DIR)"
