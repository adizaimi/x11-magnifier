CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
LDLIBS  += -lX11 -lXrender -lXcomposite -lXdamage
PREFIX  ?= /usr/local

x11-magnifier: magnifier.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

install: x11-magnifier
	install -D -m755 x11-magnifier $(DESTDIR)$(PREFIX)/bin/x11-magnifier
	install -D -m644 x11-magnifier.1 $(DESTDIR)$(PREFIX)/share/man/man1/x11-magnifier.1

clean:
	rm -f x11-magnifier

.PHONY: install clean
