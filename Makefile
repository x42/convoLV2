#!/usr/bin/make -f

# these can be overrideen using make variables. e.g.
#   make CFLAGS=-O2
#   make install DESTDIR=$(CURDIR)/debian/convoLV2 PREFIX=/usr
#
OPTIMIZATIONS ?= -msse -msse2 -mfpmath=sse -ffast-math -fomit-frame-pointer -O3 -fno-finite-math-only
PREFIX ?= /usr/local
CFLAGS ?= $(OPTIMIZATIONS) -Wall

###############################################################################
LIB_EXT=.so

lv2dir = $(PREFIX)/lib/lv2
LOADLIBES=-lm
GTKCFLAGS=`pkg-config --cflags gtk+-2.0`
GTKLIBS=`pkg-config --libs gtk+-2.0`
LV2NAME=convoLV2
LV2GUI=convoLV2UI
CFLAGS+=-fPIC
CXXFLAGS=$(CFLAGS)

# check for build-dependencies
ifeq ($(shell pkg-config --exists lv2 lv2core || echo no), no)
  $(error "LV2 SDK was not found")
endif

ifeq ($(shell pkg-config --exists sndfile samplerate\
        && test -f /usr/include/zita-convolver.h -o -f /usr/local/include/zita-convolver.h \
        && echo yes), yes)
  CFLAGS+=`pkg-config --cflags sndfile samplerate`
  LOADLIBES+=-lzita-convolver `pkg-config --libs sndfile samplerate`
else
  $(error "libzita-convolver3, libsndfile and libsamplerate are required")
endif

# build target definitions
default: all

all: manifest.ttl $(LV2NAME)$(LIB_EXT) $(LV2GUI)$(LIB_EXT)

manifest.ttl: manifest.ttl.in
	sed "s/@LV2NAME@/$(LV2NAME)/;s/@LV2GUI@/$(LV2GUI)/;s/@LIB_EXT@/$(LIB_EXT)/" \
	  manifest.ttl.in > manifest.ttl

$(LV2NAME)$(LIB_EXT): lv2.c convolution.o uris.h
	$(CC) -o $(LV2NAME)$(LIB_EXT) $(CFLAGS) $(LDFLAGS) $(LOADLIBES) \
	  -std=c99 -shared -Wl,-Bstatic -Wl,-Bdynamic lv2.c convolution.o

$(LV2GUI)$(LIB_EXT): ui.c uris.h
	$(CC) -o $(LV2GUI)$(LIB_EXT) $(CFLAGS) $(LDFLAGS) $(GTKCFLAGS) $(GTKLIBS) \
	  -std=c99 -shared -Wl,-Bstatic -Wl,-Bdynamic ui.c

%.o: %.cc %.h

# install/uninstall/clean target definitions

install: all
	install -d $(DESTDIR)$(lv2dir)/$(LV2NAME)
	install -m755 $(LV2NAME)$(LIB_EXT) $(DESTDIR)$(lv2dir)/$(LV2NAME)
	install -m755 $(LV2GUI)$(LIB_EXT) $(DESTDIR)$(lv2dir)/$(LV2NAME)
	install -m644 manifest.ttl $(LV2NAME).ttl $(DESTDIR)$(lv2dir)/$(LV2NAME)

uninstall:
	rm -f $(DESTDIR)$(lv2dir)/$(LV2NAME)/manifest.ttl
	rm -f $(DESTDIR)$(lv2dir)/$(LV2NAME)/*.ttl
	rm -f $(DESTDIR)$(lv2dir)/$(LV2NAME)/$(LV2NAME)$(LIB_EXT)
	rm -f $(DESTDIR)$(lv2dir)/$(LV2GUI)/$(LV2NAME)$(LIB_EXT)
	-rmdir $(DESTDIR)$(lv2dir)/$(LV2NAME)

clean:
	rm -f *.o manifest.ttl $(LV2NAME)$(LIB_EXT) $(LV2GUI)$(LIB_EXT)

.PHONY: clean all install uninstall
