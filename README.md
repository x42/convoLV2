convoLV2
--------

convoLV2 is a [LV2](http://lv2plug.in) audio plugin to convolve audio signals.

It uses [libzita-convolver](http://kokkinizita.linuxaudio.org/linuxaudio/downloads/index.html)
to do the convolution and [libsndfile](http://www.mega-nerd.com/libsndfile/),
[libsamplerate](http://www.mega-nerd.com/SRC/) to read the impulse-response file.

Currently it is in early Alpha development stage. It is used to prototype LV2 config-options
extension.

Installation & Usage
--------------------

    make
    sudo make install
    jalv http://gareus.org/oss/lv2/convoLV2

    sudo make uninstall
