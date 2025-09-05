# Minimal RIST Test Makefile
CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -O2 -g -D_GNU_SOURCE
INCLUDES = -I../librist/include -I../librist/build/include
LIBDIR = ../librist/build
LIBS = -L$(LIBDIR) -lrist -lcjson -lmbedtls -lmbedx509 -lmbedcrypto -lpthread -lm

# Static library path
RIST_LIB = $(LIBDIR)/librist.a

TARGETS = server client

all: $(TARGETS)

server: server.c $(RIST_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(RIST_LIB) -lcjson -lmbedtls -lmbedx509 -lmbedcrypto -lpthread -lm

client: client.c $(RIST_LIB)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(RIST_LIB) -lcjson -lmbedtls -lmbedx509 -lmbedcrypto -lpthread -lm

clean:
	rm -f $(TARGETS) *.o

test: all
	@echo "=== Starting RIST Test ==="
	@echo "Run './server' in one terminal and './client' in another"
	@echo "Or run 'make run-test' to run both automatically"

run-test: all
	@echo "Starting server in background..."
	./server &
	@sleep 2
	@echo "Starting client..."
	./client
	@echo "Killing any remaining processes..."
	@pkill -f "./server" || true

.PHONY: all clean test run-test

# Help target
help:
	@echo "Available targets:"
	@echo "  all       - Build server and client"
	@echo "  server    - Build server only" 
	@echo "  client    - Build client only"
	@echo "  clean     - Remove built files"
	@echo "  test      - Show test instructions"
	@echo "  run-test  - Run automated test"
	@echo "  help      - Show this help"