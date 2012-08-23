convoLV2
--------

convoLV2 is a [LV2](http://lv2plug.in) plugin to convolve audio signals.

It uses [libzita-convolver](http://kokkinizita.linuxaudio.org/linuxaudio/downloads/index.html)
to do the convolution and [libsndfile](http://www.mega-nerd.com/libsndfile/),
[libsamplerate](http://www.mega-nerd.com/SRC/) to read the impulse-response file.

Currently it is in early Alpha development stage and used to prototype
LV2 config-options extension.

The plugin works properly, but it is not [yet] possible to
*   change the IR file or configuration settings (gain, delay, channel-map)
*   use it with blocksize other than 1024

Installation & Usage
--------------------

    make
    sudo make install
    jalv http://gareus.org/oss/lv2/convoLV2
    
    sudo make uninstall

Make accepts the following parameters: `CFLAGS, LDFLAGS, PREFIX, DESTDIR`

    make CFLAGS=-O2
    make install DESTDIR=$(CURDIR)/debian/convoLV2 PREFIX=/usr
