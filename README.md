convoLV2
--------

convoLV2 is a [LV2](http://lv2plug.in) plugin to convolve audio signals.

It uses
[libzita-convolver](http://kokkinizita.linuxaudio.org/linuxaudio/downloads/) to
do the convolution and [libsndfile](http://www.mega-nerd.com/libsndfile/),
[libsamplerate](http://www.mega-nerd.com/SRC/) to read the impulse-response
file.

Currently, it is in early alpha stage of development, but is useful because it
provides latency-free convolution, and serves as a testing implementation of
the new "options" and "buf-size" extensions added in LV2 1.2.0.  For convoLV2
to work, the host must:

 * Support the feature http://lv2plug.in/ns/ext/buf-size#powerOf2BlockLength
 * Pass the option http://lv2plug.in/ns/ext/buf-size#maxBlockLength to
   instantiate()

The plugin works properly, but currently lacks:

 * Any way to change configuration settings (e.g. gain, delay, channel map)
 * A decent GUI (a very basic GUI is included to load an IR file)

Installation & Usage
--------------------

    make
    sudo make install
    jalv http://gareus.org/oss/lv2/convoLV2
    
    sudo make uninstall

Make accepts the following parameters: `CFLAGS, LDFLAGS, PREFIX, DESTDIR`

    make CFLAGS=-O2
    make install DESTDIR=$(CURDIR)/debian/convoLV2 PREFIX=/usr

Report bugs to <robin@gareus.org> or on irc.freenode.net #lv2 
