CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -std=c99 -D_POSIX_C_SOURCE=200809L
LIB_OBJ = libquantum.o

TOOLS = qrun qstat qresult qresource qcancel

.PHONY: all clean

all: $(TOOLS)

libquantum.o: libquantum.c libquantum.h
	$(CC) $(CFLAGS) -c -o $@ $<

qrun:      qrun.o      $(LIB_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

qstat:     qstat.o     $(LIB_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

qresult:   qresult.o   $(LIB_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

qresource: qresource.o $(LIB_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

qcancel:   qcancel.o   $(LIB_OBJ)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c libquantum.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f *.o $(TOOLS)