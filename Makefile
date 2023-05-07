#!/usr/bin/make -f

# these can be overridden using make variables. e.g.
#   make CXXFLAGS=-O2
#   make install DESTDIR=$(CURDIR)/debian/convoLV2 PREFIX=/usr
#
PREFIX ?= /usr/local
LV2DIR ?= $(PREFIX)/lib/lv2

OPTIMIZATIONS ?= -msse -msse2 -mfpmath=sse -ffast-math -fomit-frame-pointer -O3 -fno-finite-math-only -DNDEBUG
CXXFLAGS ?= $(OPTIMIZATIONS) -Wall

PKG_CONFIG?=pkg-config
STRIP ?= strip

BUILDGTK ?= no

###############################################################################
BUILDDIR=build/

LV2NAME=convoLV2
LV2GUI=convoLV2UI
BUNDLE=convo.lv2

targets=

UNAME=$(shell uname)
ifeq ($(UNAME),Darwin)
  LV2LDFLAGS=-dynamiclib
  LIB_EXT=.dylib
  STRIPFLAGS=-u -r -arch all -s lv2syms
  UISTRIPFLAGS=-u -r -arch all -s lv2uisyms
  targets+=lv2syms lv2uisyms
else
  LV2LDFLAGS=-Wl,-Bstatic -Wl,-Bdynamic
  LIB_EXT=.so
  STRIPFLAGS=-s
  UISTRIPFLAGS=-s
endif

ifneq ($(XWIN),)
  CC=$(XWIN)-gcc
  CXX=$(XWIN)-g++
  STRIP=$(XWIN)-strip
  LV2LDFLAGS=-Wl,-Bstatic -Wl,-Bdynamic -Wl,--as-needed -lpthread
  LIB_EXT=.dll
  BUILDGTK=no
  override LDFLAGS += -static-libgcc -static-libstdc++
else
  override CXXFLAGS += -fPIC -fvisibility=hidden -pthread
endif

# check for build-dependencies

ifeq ($(shell $(PKG_CONFIG) --exists lv2 || echo no), no)
  $(error "LV2 SDK was not found")
endif

ifeq ($(shell $(PKG_CONFIG) --atleast-version=1.4 lv2 || echo no), no)
  $(error "LV2 SDK needs to be version 1.4 or later")
endif

ifneq ($(shell $(PKG_CONFIG) --exists sndfile samplerate\
        && echo yes), yes)
  $(error "libsndfile and libsamplerate are required")
endif

CLV2UI=
ifneq ($(BUILDGTK), no)
  ifeq ($(shell $(PKG_CONFIG) --exists glib-2.0 gtk+-2.0 || echo no), no)
    $(warning "The optional plugin GUI requires glib-2.0 and gtk+-2.0")
    $(warning "call  make BUILDGTK=no  to disable the GUI.")
    $(error "Aborting build.")
  endif
	CLV2UI=ui:ui clv2:ui;
endif

ifeq ($(LIBZITACONVOLVER),)
  ifeq ($(shell test -f /usr/include/zita-convolver.h -o -f /usr/local/include/zita-convolver.h || echo no ), no)
    $(error "libzita-convolver3 or 4, is required")
  endif
  LOADLIBES += -lzita-convolver
endif

# add library dependent flags and libs

override CXXFLAGS +=`$(PKG_CONFIG) --cflags glib-2.0 lv2 sndfile samplerate`
override LOADLIBES +=`$(PKG_CONFIG) --libs sndfile samplerate` -lm

ifeq ($(shell $(PKG_CONFIG) --atleast-version=1.8.1 lv2 && echo yes), yes)
	override CXXFLAGS += -DHAVE_LV2_1_8
endif

ifeq ($(shell $(PKG_CONFIG) --atleast-version=1.18.6 lv2 && echo yes), yes)
  override CXXFLAGS += -DHAVE_LV2_1_18_6
endif

GTKCFLAGS = `$(PKG_CONFIG) --cflags gtk+-2.0`
GTKLIBS   = `$(PKG_CONFIG) --libs gtk+-2.0`

targets+= $(BUILDDIR)$(LV2NAME)$(LIB_EXT)

ifneq ($(BUILDGTK), no)
	targets+=$(BUILDDIR)$(LV2GUI)$(LIB_EXT)
endif

# build target definitions

default: all

all: $(BUILDDIR)manifest.ttl $(BUILDDIR)$(LV2NAME).ttl $(targets)

lv2syms:
	echo "_lv2_descriptor" > lv2syms

lv2uisyms:
	echo "_lv2ui_descriptor" > lv2uisyms

$(BUILDDIR)manifest.ttl: lv2ttl/manifest.ttl.in lv2ttl/manifest.gui.ttl.in
	@mkdir -p $(BUILDDIR)
	sed "s/@LV2NAME@/$(LV2NAME)/;s/@LV2GUI@/$(LV2GUI)/;s/@LIB_EXT@/$(LIB_EXT)/" \
	  lv2ttl/manifest.ttl.in > $(BUILDDIR)manifest.ttl
ifneq ($(BUILDGTK), no)
	sed "s/@LV2NAME@/$(LV2NAME)/;s/@LV2GUI@/$(LV2GUI)/;s/@LIB_EXT@/$(LIB_EXT)/" \
		lv2ttl/manifest.gui.ttl.in >> $(BUILDDIR)manifest.ttl
endif

$(BUILDDIR)$(LV2NAME).ttl: lv2ttl/$(LV2NAME).ttl.in lv2ttl/$(LV2NAME).gui.ttl.in
	@mkdir -p $(BUILDDIR)
	sed "s/@CLV2UI@/$(CLV2UI)/" \
		lv2ttl/$(LV2NAME).ttl.in > $(BUILDDIR)$(LV2NAME).ttl
ifneq ($(BUILDGTK), no)
	cat lv2ttl/$(LV2NAME).gui.ttl.in >> $(BUILDDIR)$(LV2NAME).ttl
endif

$(BUILDDIR)$(LV2NAME)$(LIB_EXT): lv2.c convolution.cc uris.h
	@mkdir -p $(BUILDDIR)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) \
	  -o $(BUILDDIR)$(LV2NAME)$(LIB_EXT) lv2.c convolution.cc \
	  $(LIBZITACONVOLVER) \
	  -shared $(LV2LDFLAGS) $(LDFLAGS) $(LOADLIBES)
	$(STRIP) $(STRIPFLAGS) $(BUILDDIR)$(LV2NAME)$(LIB_EXT)

$(BUILDDIR)$(LV2GUI)$(LIB_EXT): ui.c uris.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(GTKCFLAGS) \
	  -o $(BUILDDIR)$(LV2GUI)$(LIB_EXT) ui.c \
		-shared $(LV2LDFLAGS) $(LDFLAGS) $(GTKLIBS)
	$(STRIP) $(UISTRIPFLAGS) $(BUILDDIR)$(LV2GUI)$(LIB_EXT)


# install/uninstall/clean target definitions

install: all
	install -d $(DESTDIR)$(LV2DIR)/$(BUNDLE)
	install -m755 $(targets) $(DESTDIR)$(LV2DIR)/$(BUNDLE)
	install -m644 $(BUILDDIR)manifest.ttl $(BUILDDIR)$(LV2NAME).ttl $(DESTDIR)$(LV2DIR)/$(BUNDLE)

uninstall:
	rm -f $(DESTDIR)$(LV2DIR)/$(BUNDLE)/manifest.ttl
	rm -f $(DESTDIR)$(LV2DIR)/$(BUNDLE)/$(LV2NAME).ttl
	rm -f $(DESTDIR)$(LV2DIR)/$(BUNDLE)/$(LV2NAME)$(LIB_EXT)
	rm -f $(DESTDIR)$(LV2DIR)/$(BUNDLE)/$(LV2GUI)$(LIB_EXT)
	-rmdir $(DESTDIR)$(LV2DIR)/$(BUNDLE)

clean:
	rm -f $(BUILDDIR)manifest.ttl $(BUILDDIR)$(LV2NAME).ttl \
		$(BUILDDIR)$(LV2NAME)$(LIB_EXT) $(BUILDDIR)$(LV2GUI)$(LIB_EXT) \
		lv2syms lv2uisyms
	rm -rf $(BUILDDIR)*.dSYM
	-test -d $(BUILDDIR) && rmdir $(BUILDDIR) || true

.PHONY: clean all install uninstall
