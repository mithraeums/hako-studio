CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c99
LDLIBS  := -lpthread
BIN     := hako-serve

UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
	CFLAGS += -D_GNU_SOURCE
endif

all: $(BIN)

$(BIN): serve.c
	$(CC) $(CFLAGS) -o $@ serve.c $(LDLIBS)

asan: serve.c
	$(CC) -O1 -g -Wall -Wextra -std=c99 -fsanitize=address,undefined -o $(BIN)-asan serve.c $(LDLIBS)

run: $(BIN)
	./$(BIN) --web .

clean:
	rm -f $(BIN) $(BIN)-asan

.PHONY: all asan run clean
