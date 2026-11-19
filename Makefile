CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
LDLIBS  += -lX11 -lXrender
PREFIX  ?= /usr/local

x11-loupe: loupe.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

install: x11-loupe
	install -D -m755 x11-loupe $(DESTDIR)$(PREFIX)/bin/x11-loupe
	install -D -m644 x11-loupe.1 $(DESTDIR)$(PREFIX)/share/man/man1/x11-loupe.1

clean:
	rm -f x11-loupe

.PHONY: install clean
