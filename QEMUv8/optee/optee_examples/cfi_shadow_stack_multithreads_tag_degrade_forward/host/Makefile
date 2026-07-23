CC ?= $(CROSS_COMPILE)gcc

CFLAGS += -Wall -I../ta/include -I$(TEEC_EXPORT)/include
LDLIBS += -lteec -L$(TEEC_EXPORT)/lib 
LDFLAGS += -Wl,-rpath-link=$(TEEC_EXPORT)/lib

BINARY = cfi_test

.PHONY: all clean

all: $(BINARY)

$(BINARY): cfi_test.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS) $(LDFLAGS)

clean:
	rm -f $(BINARY)
