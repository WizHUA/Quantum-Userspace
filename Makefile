CC      = gcc
CFLAGS  = -Wall -Wextra -O2
LIB_OBJ = libquantum.o

TOOLS   = qrun qstat qresult qresource qcancel

.PHONY: all clean

all: $(TOOLS)

libquantum.o: libquantum.c libquantum.h
	$(CC) $(CFLAGS) -c -o $@ $<

qrun:      qrun.o      $(LIB_OBJ)
qstat:     qstat.o     $(LIB_OBJ)
qresult:   qresult.o   $(LIB_OBJ)
qresource: qresource.o $(LIB_OBJ)
qcancel:   qcancel.o   $(LIB_OBJ)

%.o: %.c libquantum.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f *.o $(TOOLS)