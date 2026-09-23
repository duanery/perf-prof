CC=gcc

CFLAGS=-Wall -g -O2

SRCS=pthread.c binding-workload.c

CFLAGS_pthread=-lpthread
CFLAGS_binding-workload=-lpthread

OBJS=$(SRCS:.c=.o)

BINS=$(SRCS:.c=)

REVIEW_INCLUDES=-I.. -I../src -I../lib/perf/include -I../lib/subcmd -I../lib/traceevent -I../lib -I../include -I../include/uapi -I../arch/$(shell uname -m | sed 's/x86_64/x86/;s/aarch64/arm64/')/include
REVIEW_LIBS=../lib/perf/libperf.a ../lib/string.o ../lib/ctype.o ../lib/rbtree.o ../lib/hweight.o

all: $(BINS)

bindings-order: bindings-order.c ../src/order.c ../src/bindings.c ../src/bindings.h
	$(CC) $(CFLAGS) $(REVIEW_INCLUDES) -ffunction-sections -fdata-sections -Wl,--gc-sections -o $@ bindings-order.c ../src/bindings.c $(REVIEW_LIBS)

check-bindings: bindings-order
	./bindings-order

%: %.c
	$(CC) $(CFLAGS) $(CFLAGS_$@) -o $@ $<

clean:
	rm -f $(OBJS) $(BINS) bindings-order
