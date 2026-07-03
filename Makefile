PREFIX ?= /usr/local
CFLAGS ?= -O2 -Wall -Wextra
CFLAGS += $(shell pkg-config --cflags libpipewire-0.3)
LDLIBS = $(shell pkg-config --libs libpipewire-0.3) -lm

waybar-vis: waybar-vis.o fft.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

waybar-vis.o: waybar-vis.c fft.h
fft.o: fft.c fft.h

clean:
	rm -f waybar-vis *.o

install: waybar-vis
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 waybar-vis $(DESTDIR)$(PREFIX)/bin
