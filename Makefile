CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
LDLIBS  += -lX11 -lXrender
PREFIX  ?= /usr/local

screen-loupe: loupe.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

install: screen-loupe
	install -D -m755 screen-loupe $(DESTDIR)$(PREFIX)/bin/screen-loupe
	install -D -m644 screen-loupe.1 $(DESTDIR)$(PREFIX)/share/man/man1/screen-loupe.1

clean:
	rm -f screen-loupe

.PHONY: install clean
