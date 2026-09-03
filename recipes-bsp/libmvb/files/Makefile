# libmvb — MVB-Userspace-Bibliothek + Diagnosewerkzeug
CC      ?= gcc
CFLAGS  ?= -O2 -Wall
LIBVER   = 1.0
SONAME   = libmvb.so.1
LIBFILE  = libmvb.so.$(LIBVER)

prefix     ?= /usr
libdir     ?= $(prefix)/lib
includedir ?= $(prefix)/include
bindir     ?= $(prefix)/bin

all: $(LIBFILE) mvbtool

libmvb.o: libmvb.c libmvb.h
	$(CC) $(CFLAGS) -fPIC -c libmvb.c -o libmvb.o

$(LIBFILE): libmvb.o
	$(CC) $(LDFLAGS) -shared -Wl,-soname,$(SONAME) -o $@ libmvb.o

# Das Werkzeug bindet die Lib direkt ein (keine Laufzeitabhaengigkeit).
mvbtool: mvbtool.c libmvb.o libmvb.h
	$(CC) $(CFLAGS) $(LDFLAGS) mvbtool.c libmvb.o -o mvbtool

install: all
	install -d $(DESTDIR)$(libdir) $(DESTDIR)$(includedir) $(DESTDIR)$(bindir)
	install -m 0755 $(LIBFILE) $(DESTDIR)$(libdir)/
	ln -sf $(LIBFILE) $(DESTDIR)$(libdir)/$(SONAME)
	ln -sf $(SONAME)  $(DESTDIR)$(libdir)/libmvb.so
	install -m 0644 libmvb.h mvbc.h $(DESTDIR)$(includedir)/
	install -m 0755 mvbtool $(DESTDIR)$(bindir)/

clean:
	rm -f *.o *.so *.so.* mvbtool

.PHONY: all install clean
