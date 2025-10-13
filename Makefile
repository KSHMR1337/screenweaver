CC        = gcc
CFLAGS    = -Wall -Wextra -std=c99 -O2
LIBS      = `pkg-config --cflags --libs sdl2 SDL2_image` -lGL -lX11 -lavformat -lavcodec -lswscale -lavutil -lpthread
INCLUDES  = -I/usr/include/SDL2

# Installation directories
PREFIX    ?= /usr/local
BINDIR    = $(PREFIX)/bin

# Source and target
SRC       = screenweaver.c
TARGET    = screenweaver

.PHONY: all install uninstall clean deps

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(INCLUDES) -o $(TARGET) $(SRC) $(LIBS)

install: $(TARGET)
	install -D -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)

clean:
	rm -f $(TARGET)

deps:
	@echo "Checking dependencies..."
	@pkg-config --exists sdl2 \
		|| (echo "SDL2 development libraries not found. Install with: sudo apt-get install libsdl2-dev" && exit 1)
	@pkg-config --exists SDL2_image \
		|| (echo "SDL2_image development libraries not found. Install with: sudo apt-get install libsdl2-image-dev" && exit 1)
	@pkg-config --exists gl \
		|| (echo "OpenGL development libraries not found. Install with: sudo apt-get install libgl1-mesa-dev" && exit 1)
	@pkg-config --exists x11 \
		|| (echo "X11 development libraries not found. Install with: sudo apt-get install libx11-dev" && exit 1)
	@echo "All dependencies found!"
