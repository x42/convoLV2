#!/usr/bin/make -f

# these can be overridden using make variables. e.g.
#   make CXXFLAGS=-O2
#   make install DESTDIR=$(CURDIR)/debian/convoLV2 PREFIX=/usr
#
OPTIMIZATIONS ?= -msse -msse2 -mfpmath=sse -ffast-math -fomit-frame-pointer -O3 -fno-finite-math-only
PREFIX ?= /usr/local
CXXFLAGS ?= $(OPTIMIZATIONS) -Wall
LIBDIR ?= lib
BUILDGTK ?= no

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

ifneq ($(XWIN),)
  CC=$(XWIN)-gcc
  CXX=$(XWIN)-g++
  LV2LDFLAGS=-Wl,-Bstatic -Wl,-Bdynamic -Wl,--as-needed -lpthread
  LIB_EXT=.dll
  BUILDGTK=no
  override LDFLAGS += -static-libgcc -static-libstdc++
endif

# check for build-dependencies

ifeq ($(shell pkg-config --exists lv2 || echo no), no)
  $(error "LV2 SDK was not found")
endif

ifeq ($(shell pkg-config --atleast-version=1.4 lv2 || echo no), no)
  $(error "LV2 SDK needs to be version 1.4 or later")
endif

ifneq ($(shell pkg-config --exists sndfile samplerate\
        && echo yes), yes)
  $(error "libsndfile and libsamplerate are required")
endif

ifneq ($(BUILDGTK), no)
  ifeq ($(shell pkg-config --exists glib-2.0 gtk+-2.0 || echo no), no)
    $(warning "The optional plugin GUI requires glib-2.0 and gtk+-2.0")
    $(warning "call  make BUILDGTK=no  to disable the GUI.")
    $(error "Aborting build.")
  endif
endif

ifeq ($(LIBZITACONVOLVER),)
  ifeq ($(shell test -f /usr/include/zita-convolver.h -o -f /usr/local/include/zita-convolver.h || echo no ), no)
    $(error "libzita-convolver3, is required")
  endif
  LOADLIBES += -lzita-convolver
endif

# add library dependent flags and libs

override CXXFLAGS +=-fPIC
override CXXFLAGS +=`pkg-config --cflags glib-2.0 lv2 sndfile samplerate`
override LOADLIBES +=`pkg-config --libs sndfile samplerate` -lm

ifeq ($(shell pkg-config --atleast-version=1.8.1 lv2 && echo yes), yes)
	override CXXFLAGS += -DHAVE_LV2_1_8
endif


GTKCFLAGS = `pkg-config --cflags gtk+-2.0`
GTKLIBS   = `pkg-config --libs gtk+-2.0`

targets= $(LV2NAME)$(LIB_EXT)

ifneq ($(BUILDGTK), no)
	targets+=$(LV2GUI)$(LIB_EXT)
endif

# build target definitions

default: all

all: manifest.ttl $(LV2NAME).ttl $(targets)

manifest.ttl: lv2ttl/manifest.ttl.in lv2ttl/manifest.gui.ttl.in
	sed "s/@LV2NAME@/$(LV2NAME)/;s/@LV2GUI@/$(LV2GUI)/;s/@LIB_EXT@/$(LIB_EXT)/" \
	  lv2ttl/manifest.ttl.in > manifest.ttl
ifneq ($(BUILDGTK), no)
	sed "s/@LV2NAME@/$(LV2NAME)/;s/@LV2GUI@/$(LV2GUI)/;s/@LIB_EXT@/$(LIB_EXT)/" \
		lv2ttl/manifest.gui.ttl.in >> manifest.ttl
endif

$(LV2NAME).ttl: lv2ttl/$(LV2NAME).ttl.in lv2ttl/$(LV2NAME).gui.ttl.in
	cat lv2ttl/$(LV2NAME).ttl.in > $(LV2NAME).ttl
ifneq ($(BUILDGTK), no)
	cat lv2ttl/$(LV2NAME).gui.ttl.in >> $(LV2NAME).ttl
endif

$(LV2NAME)$(LIB_EXT): lv2.c convolution.cc uris.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) \
	  -o $(LV2NAME)$(LIB_EXT) lv2.c convolution.cc \
	  $(LIBZITACONVOLVER) \
	  -shared $(LV2LDFLAGS) $(LDFLAGS) $(LOADLIBES)

$(LV2GUI)$(LIB_EXT): ui.c uris.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(GTKCFLAGS) \
	  -o $(LV2GUI)$(LIB_EXT) ui.c \
		-shared $(LV2LDFLAGS) $(LDFLAGS) $(GTKLIBS)


# install/uninstall/clean target definitions

install: all
	install -d $(DESTDIR)$(LV2DIR)/$(BUNDLE)
	install -m755 $(targets) $(DESTDIR)$(LV2DIR)/$(BUNDLE)
	install -m644 manifest.ttl $(LV2NAME).ttl $(DESTDIR)$(LV2DIR)/$(BUNDLE)

uninstall:
	rm -f $(DESTDIR)$(LV2DIR)/$(BUNDLE)/manifest.ttl
	rm -f $(DESTDIR)$(LV2DIR)/$(BUNDLE)/$(LV2NAME).ttl
	rm -f $(DESTDIR)$(LV2DIR)/$(BUNDLE)/$(LV2NAME)$(LIB_EXT)
	rm -f $(DESTDIR)$(LV2DIR)/$(BUNDLE)/$(LV2GUI)$(LIB_EXT)
	-rmdir $(DESTDIR)$(LV2DIR)/$(BUNDLE)

clean:
	rm -f manifest.ttl $(LV2NAME).ttl $(LV2NAME)$(LIB_EXT) $(LV2GUI)$(LIB_EXT)

.PHONY: clean all install uninstall
