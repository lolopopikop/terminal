CXX = g++
CC = gcc
CXXFLAGS = -std=c++17 -Wall -Wextra -O2 -D_FILE_OFFSET_BITS=64
CFLAGS = -Wall -Wextra -O2 -D_FILE_OFFSET_BITS=64 -D_GNU_SOURCE
LDFLAGS = -lreadline -lfuse3 -lstdc++ -lpthread
TARGET = kubsh
PACKAGE = kubsh
VERSION = 1.0
ARCH = amd64

.PHONY: all clean run deb install uninstall test

all: $(TARGET)

$(TARGET): kubsh.o vfs.o
	$(CXX) $^ -o $@ $(LDFLAGS)

kubsh.o: kubsh.cpp vfs.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

vfs.o: vfs.c vfs.h
	$(CC) $(CFLAGS) -c $< -o $@

run: $(TARGET)
	@echo "Starting kubsh... (use Ctrl+D or \q to exit)"
	./$(TARGET)

clean:
	rm -f *.o $(TARGET) $(TARGET)-$(VERSION).tar.gz
	rm -rf debian/$(TARGET)
	rm -f $(TARGET)_$(VERSION)-1_$(ARCH).deb

deb: $(TARGET)
	@echo "Building DEB package..."
	@mkdir -p debian/$(TARGET)/usr/local/bin
	@mkdir -p debian/$(TARGET)/DEBIAN
	@cp $(TARGET) debian/$(TARGET)/usr/local/bin/
	
	@echo "Package: $(PACKAGE)" > debian/$(TARGET)/DEBIAN/control
	@echo "Version: $(VERSION)-1" >> debian/$(TARGET)/DEBIAN/control
	@echo "Architecture: $(ARCH)" >> debian/$(TARGET)/DEBIAN/control
	@echo "Maintainer: kubsh maintainer <kubsh@example.com>" >> debian/$(TARGET)/DEBIAN/control
	@echo "Description: Custom shell with VFS support" >> debian/$(TARGET)/DEBIAN/control
	@echo " A shell implementation with virtual filesystem for user information" >> debian/$(TARGET)/DEBIAN/control
	
	fakeroot dpkg-deb --build debian/$(TARGET) $(TARGET)_$(VERSION)-1_$(ARCH).deb
	@echo "✅ DEB package created: $(TARGET)_$(VERSION)-1_$(ARCH).deb"

install: $(TARGET)
	cp $(TARGET) /usr/local/bin/
	@echo "Installed to /usr/local/bin/$(TARGET)"

uninstall:
	rm -f /usr/local/bin/$(TARGET)
	@echo "Uninstalled from /usr/local/bin/$(TARGET)"

test:
	@echo "To run tests, install pytest and run:"
	@echo "  python3 -m pytest"
	@echo "Or if test files are in the repo:"
	@echo "  python3 -m pytest test_basic.py test_vfs.py"