NAME = awsc
LIB = lib$(NAME)
OBJECTS = $(NAME).o memmgr.o
LIBRARY_VERSION = 0.0.1
HEADERS = awsc.h
CC = g++
CPPFLAGS = -DAWS_CUSTOM_MEMORY_MANAGEMENT -DUSE_IMPORT_EXPORT `pkg-config --cflags memarena aws-cpp-sdk` -g -Wall -std=c++11 -pedantic -fPIC -O3 -Wno-write-strings
LDLIBS = `pkg-config --libs memarena aws-cpp-sdk`
DESTDIR = $(HOME)/opt

all: shared static

shared: $(OBJECTS)
	$(CC) -shared -o $(LIB).so.$(LIBRARY_VERSION) $<

static: $(OBJECTS)
	ar rcs $(LIB).a $<

clean:
	rm -f $(OBJECTS) $(LIB).so.$(LIBRARY_VERSION) $(LIB).a check.o check

install: all
	mkdir -p $(DESTDIR)/include $(DESTDIR)/lib $(DESTDIR)/lib/pkgconfig
	cp -f $(HEADERS) $(DESTDIR)/include
	cp -f $(LIB).so.$(LIBRARY_VERSION) $(LIB).a $(DESTDIR)/lib
	(cd $(DESTDIR)/lib && ln -sf $(LIB).so.$(LIBRARY_VERSION) $(LIB).so)
	cp -f $(NAME).pc $(DESTDIR)/lib/pkgconfig
	sed -i "s#HOME#$(HOME)#g" $(DESTDIR)/lib/pkgconfig/$(NAME).pc

check: $(OBJECTS) check.o
	$(CC) -o check $(OBJECTS) check.o $(LDLIBS)

runcheck: check
	./check

.PHONY: all shared static clean install check runcheck
