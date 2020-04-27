CFLAGS=-std=c99 -Wall -Wextra -pedantic -Os

jl:

clean:
	rm -f jl

install: jl
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp -f jl $(DESTDIR)$(PREFIX)/bin
	chmod 755 $(DESTDIR)$(PREFIX)/bin/jl

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/jl

.PHONY: clean install uninstall
