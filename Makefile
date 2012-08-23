OPTIMIZATIONS ?= -msse -msse2 -mfpmath=sse -ffast-math -fomit-frame-pointer -O3 -fno-finite-math-only

PREFIX ?= /usr/local
CFLAGS ?= $(OPTIMIZATIONS)

lv2dir = $(PREFIX)/lib/lv2
LOADLIBES=-lm
LIB_EXT=.so
LV2NAME=convoLV2

ifeq ($(shell pkg-config --exists lv2 lv2core || echo no), no)
  $(error "LV2 SDK was not found")
endif

ifeq ($(shell pkg-config --exists sndfile \
        && test -f /usr/include/zita-convolver.h -o -f /usr/local/include/zita-convolver.h \
        && echo yes), yes)
  CFLAGS+=`pkg-config --cflags sndfile`
  LOADLIBES+=-lzita-convolver `pkg-config --libs sndfile`
  BXCC=$(CXX)
else
  $(error "libzita-convolver3 and libsndfile are required")
endif


all: lv2

lv2: manifest.ttl $(LV2NAME)$(LIB_EXT) 

manifest.ttl: manifest.ttl.in
	sed "s/@LV2NAME@/$(LV2NAME)/;s/@LIB_EXT@/$(LIB_EXT)/" manifest.ttl.in > manifest.ttl

$(LV2NAME)$(LIB_EXT): lv2.c convolution.o
	$(CC) -o $(LV2NAME)$(LIB_EXT) $(CFLAGS) $(LDFLAGS) $(LOADLIBES) \
	  -shared -Wl,-Bstatic -Wl,-Bdynamic lv2.c convolution.o

%.o: %.cc %.h

install: manifest.ttl $(LV2NAME)$(LIB_EXT)
	install -d $(DESTDIR)$(lv2dir)/$(LV2NAME)
	install -m755 $(LV2NAME)$(LIB_EXT) $(DESTDIR)$(lv2dir)/$(LV2NAME)
	install -m644 manifest.ttl $(LV2NAME).ttl $(DESTDIR)$(lv2dir)/$(LV2NAME)

uninstall:
	rm -f $(DESTDIR)$(lv2dir)/$(LV2NAME)/*.ttl
	rm -f $(DESTDIR)$(lv2dir)/$(LV2NAME)/$(LV2NAME)$(LIB_EXT)
	-rmdir $(DESTDIR)$(lv2dir)/$(LV2NAME)

clean:
	rm -f *.o manifest.ttl $(LV2NAME)$(LIB_EXT)

.PHONY: clean all install uninstall lv2
