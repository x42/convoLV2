convoLV2
========

convoLV2 is a [LV2](http://lv2plug.in) plugin to convolve audio signals with
zero latency.

It is a very basic plugin: Currently the only parameter is the Impulse-Response file
and hence it is robust and efficient convolver.


The plugin comes in three variants:
*   Mono:  1 channel in, 1 channel out. Mono IR file
*   Mono To Stereo:  1 channel in, 2 channel out. Stereo IR file. (L, R)
*   True Stereo: 2 in, 2 out.  4 channel IR file (L -> L, R -> R, L -> R, R -> L)

Excess channels in an IR file are ignored. If an IR file has insufficient channels
for the required configuration, channel-assignment wraps around (modulo file channel count).

convoLV2's main use-case is cabinet-emulation and generic signal processing where latency matters.

For fancy reverb applications, see also [IR.lv2](https://tomszilagyi.github.io/plugins/ir.lv2/)

Install
-------

```bash
make
sudo make install PREFIX=/usr

# Test in jalv LV2 host
jalv.gtk http://gareus.org/oss/lv2/convoLV2#Mono
# or
jalv.gtk http://gareus.org/oss/lv2/convoLV2#MonoToStereo
# or
jalv.gtk http://gareus.org/oss/lv2/convoLV2#Stereo
```


Note to packagers: The Makefile honors `PREFIX` and `DESTDIR` variables as well
as `CFLAGS`, `LDFLAGS` and `OPTIMIZATIONS` (additions to `CFLAGS`), also
see the first 10 lines of the Makefile.
You really want to package the superset of [x42-plugins](https://github.com/x42/x42-plugins).


Under the hood
--------------

[libzita-convolver](http://kokkinizita.linuxaudio.org/linuxaudio/downloads/) is used to
perform the convolution, [libsndfile](http://www.mega-nerd.com/libsndfile/) to read
the impulse-response and [libsamplerate](http://www.mega-nerd.com/SRC/) to resample
the IR if necessary.

convoLV2 was written to demonstrate new features of LV2 1.2.0 (back in 2012):

*   http://lv2plug.in/ns/ext/buf-size/#powerOf2BlockLength - the plugin requires a blocksize that is a power of two.
*   http://lv2plug.in/ns/ext/buf-size/#maxBlockLength - the plugin only works with blocksizes between 64 and 8192 samples per period.
*   http://lv2plug.in/ns/ext/patch/ - allow a host to pass filenames to a plugin.
*   http://lv2plug.in/ns/ext/worker/ - on/offline instances. Re-loading an IR file is performed in the background, making the plugin realtime safe.

It since serves as example code for those LV2 extensions.


While the convolution engine supports pre-delay, channel-mapping and per-channel gain settings, these parameters
are currently not exposed in the LV2 interface (hack tip: they are supported in the LV2 DSP and saved as
text in the plugin-state which can be directly edited).
