CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -Iinclude
SRC := $(wildcard src/*.c)
.PHONY: test clean

test: tests/test_core
	./tests/test_core

tests/test_core: tests/test_core.c $(SRC) $(wildcard include/*.h)
	$(CC) $(CFLAGS) -o $@ tests/test_core.c $(SRC)

tests/dump_fixture: tests/dump_fixture.c $(SRC) $(wildcard include/*.h)
	$(CC) $(CFLAGS) -o $@ tests/dump_fixture.c $(SRC)

clean:
	rm -f tests/test_core tests/dump_fixture
