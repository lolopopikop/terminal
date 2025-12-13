CXX = g++
CC = gcc

CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -D_FILE_OFFSET_BITS=64
CFLAGS   = -Wall -Wextra -O2 -D_FILE_OFFSET_BITS=64

LDFLAGS = -lreadline -lfuse3 -lstdc++ -lpthread

TARGET  = kubsh
PACKAGE = kubsh
VERSION = 1.0
ARCH    = amd64

DEB_DIR = debian/$(PACKAGE)
DEB_OUT = $(PACKAGE).deb

.PHONY: all clean run deb install uninstall test

all: $(TARGET)

$(TARGET): kubsh.o vfs.o
	$(CXX) $^ -o $@ $(LDFLAGS)

kubsh.o: kubsh.cpp vfs.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

vfs.o: vfs.c vfs.h
	$(CC) $(CFLAGS) -c $< -o $@

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f *.o $(TARGET)
	rm -rf debian
	rm -f *.deb

# =========================
# DEB PACKAGE
# =========================

deb: $(TARGET)
	@echo "📦 Building DEB package..."

	mkdir -p $(DEB_DIR)/usr/local/bin
	mkdir -p $(DEB_DIR)/DEBIAN

	cp $(TARGET) $(DEB_DIR)/usr/local/bin/

	echo "Package: $(PACKAGE)" >  $(DEB_DIR)/DEBIAN/control
	echo "Version: $(VERSION)" >> $(DEB_DIR)/DEBIAN/control
	echo "Architecture: $(ARCH)" >> $(DEB_DIR)/DEBIAN/control
	echo "Maintainer: Student <student@example.com>" >> $(DEB_DIR)/DEBIAN/control
	echo "Description: Custom shell with VFS support" >> $(DEB_DIR)/DEBIAN/control

	fakeroot dpkg-deb --build $(DEB_DIR) $(DEB_OUT)

	@echo "✅ DEB created: $(DEB_OUT)"

install: deb
	apt install -y ./$(DEB_OUT)

uninstall:
	apt remove -y $(PACKAGE)

test:
	pytest -v
