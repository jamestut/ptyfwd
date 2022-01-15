CFLAGS=-O0 -g3 -Wall -D_GNU_SOURCE -Wno-deprecated-declarations
LDFLAGS=-lcrypto

DEPS=app.o socks.o server.o client.o utils.o protocol.o global.o session.o

ptyfwd: $(DEPS) Makefile
	$(CC) $(CFLAGS) -o ptyfwd $(DEPS) $(LDFLAGS)

%.o: %.c *.h Makefile
	$(CC) $(CFLAGS) -c $< -o $@

format:
	clang-format -i *.c *.h

clean:
	rm -f *.o ptyfwd

.PHONY: clean format
