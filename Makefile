#!/usr/bin/make -f

# these can be overrideen using make variables. e.g.
#   make CXXFLAGS=-O2
#   make install DESTDIR=$(CURDIR)/debian/convoLV2 PREFIX=/usr
#
OPTIMIZATIONS ?= -msse -msse2 -mfpmath=sse -ffast-math -fomit-frame-pointer -O3 -fno-finite-math-only
PREFIX ?= /usr/local
CXXFLAGS ?= $(OPTIMIZATIONS) -Wall
LIBDIR ?= lib

###############################################################################

LV2DIR ?= $(PREFIX)/$(LIBDIR)/lv2
LV2NAME=convoLV2
LV2GUI=convoLV2UI
BUNDLE=convo.lv2

UNAME=$(shell uname)
ifeq ($(UNAME),Darwin)
  LV2LDFLAGS=-dynamiclib
  LIB_EXT=.dylib
else
  LV2LDFLAGS=-Wl,-Bstatic -Wl,-Bdynamic
  LIB_EXT=.so
endif

# check for build-dependencies

ifeq ($(shell pkg-config --exists lv2 || echo no), no)
  $(error "LV2 SDK was not found")
endif

ifeq ($(shell pkg-config --atleast-version=1.4 lv2 || echo no), no)
  $(error "LV2 SDK needs to be version 1.4 or later")
endif

ifneq ($(shell pkg-config --exists sndfile samplerate\
        && test -f /usr/include/zita-convolver.h -o -f /usr/local/include/zita-convolver.h \
        && echo yes), yes)
  $(error "libzita-convolver3, libsndfile and libsamplerate are required")
endif

ifeq ($(shell pkg-config --exists glib-2.0 gtk+-2.0 || echo no), no)
  $(error "This plugin requires glib-2.0 and gtk+-2.0")
endif


# add library dependent flags and libs

override CXXFLAGS +=-fPIC
override CXXFLAGS +=`pkg-config --cflags glib-2.0 lv2 sndfile samplerate`

LOADLIBES = -lm -lzita-convolver `pkg-config --libs sndfile samplerate`
GTKCFLAGS = `pkg-config --cflags gtk+-2.0`
GTKLIBS   = `pkg-config --libs gtk+-2.0`


# build target definitions

default: all

all: manifest.ttl $(LV2NAME)$(LIB_EXT) $(LV2GUI)$(LIB_EXT)

manifest.ttl: manifest.ttl.in
	sed "s/@LV2NAME@/$(LV2NAME)/;s/@LV2GUI@/$(LV2GUI)/;s/@LIB_EXT@/$(LIB_EXT)/" \
	  manifest.ttl.in > manifest.ttl

$(LV2NAME)$(LIB_EXT): lv2.c convolution.cc uris.h
	$(CXX) $(CXXFLAGS) \
	  -o $(LV2NAME)$(LIB_EXT) lv2.c convolution.cc \
	  -shared $(LV2LDFLAGS) $(LDFLAGS) $(LOADLIBES)

$(LV2GUI)$(LIB_EXT): ui.c uris.h
	$(CXX) $(CXXFLAGS) $(GTKCFLAGS) \
	  -o $(LV2GUI)$(LIB_EXT) ui.c \
		-shared $(LV2LDFLAGS) $(LDFLAGS) $(GTKLIBS)


# install/uninstall/clean target definitions

install: all
	install -d $(DESTDIR)$(LV2DIR)/$(BUNDLE)
	install -m755 $(LV2NAME)$(LIB_EXT) $(DESTDIR)$(LV2DIR)/$(BUNDLE)
	install -m755 $(LV2GUI)$(LIB_EXT) $(DESTDIR)$(LV2DIR)/$(BUNDLE)
	install -m644 manifest.ttl $(LV2NAME).ttl $(DESTDIR)$(LV2DIR)/$(BUNDLE)

uninstall:
	rm -f $(DESTDIR)$(LV2DIR)/$(BUNDLE)/manifest.ttl
	rm -f $(DESTDIR)$(LV2DIR)/$(BUNDLE)/*.ttl
	rm -f $(DESTDIR)$(LV2DIR)/$(BUNDLE)/$(LV2NAME)$(LIB_EXT)
	rm -f $(DESTDIR)$(LV2DIR)/$(BUNDLE)/$(LV2GUI)$(LIB_EXT)
	-rmdir $(DESTDIR)$(LV2DIR)/$(BUNDLE)

clean:
	rm -f manifest.ttl $(LV2NAME)$(LIB_EXT) $(LV2GUI)$(LIB_EXT)

.PHONY: clean all install uninstall
