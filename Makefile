PREFIX ?= /usr/local
DESTDIR ?=

.PHONY: all lib cli clean install
.NOTPARALLEL:

all:
	$(MAKE) -C linux all

lib:
	$(MAKE) -C linux lib

cli:
	$(MAKE) -C linux cli

clean:
	$(MAKE) -C linux clean

install:
	$(MAKE) -C linux PREFIX="$(PREFIX)" DESTDIR="$(DESTDIR)" install
