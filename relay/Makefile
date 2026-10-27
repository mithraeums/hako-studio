# the hako relay (RELAY.md §6). Server only: needs libcrypto (OpenSSL 3).
#   make          build ./relay
#   make test     accounts (test.c), then two hako clients through a local relay (needs ../../hako-code)
#   make fuzz     mutated requests through the handler under ASan+UBSan (FUZZ_ITERS, FUZZ_SEED)
OPENSSL ?= $(shell brew --prefix openssl@3 2>/dev/null)
CFLAGS  ?= -O2
CFLAGS  += -std=c99 -Wall -Wextra $(if $(OPENSSL),-I$(OPENSSL)/include)
LDLIBS  += $(if $(OPENSSL),-L$(OPENSSL)/lib) -lcrypto -lpthread

relay: relay.c
	$(CC) $(CFLAGS) relay.c -o relay $(LDLIBS)

test-accounts: relay test.c
	$(CC) $(CFLAGS) test.c -o /tmp/hako_relay_test $(LDLIBS)
	/tmp/hako_relay_test ./relay

test: test-accounts
	$(MAKE) -C ../../hako-code test-relay RELAY=../hako-studio/relay/relay

fuzz: fuzz.c relay.c
	$(CC) $(CFLAGS) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -Dmain=relay_main fuzz.c -o /tmp/hako_relay_fuzz $(LDLIBS)
	ASAN_OPTIONS=detect_leaks=0 /tmp/hako_relay_fuzz

clean:
	rm -f relay

.PHONY: test test-accounts fuzz clean
