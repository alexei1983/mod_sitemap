APXS ?= apxs
WARN_CFLAGS = -Wc,-Wall -Wc,-Wextra -Wc,-Wconversion -Wc,-Wshadow

.PHONY: all install clean test
all:
	$(APXS) -c -Wc,-std=c11 $(WARN_CFLAGS) mod_sitemap.c

install: all
	$(APXS) -i -a -n sitemap .libs/mod_sitemap.so

clean:
	rm -f *.o *.lo *.slo *.la
	rm -rf .libs

test:
	sh ./tests/run.sh
