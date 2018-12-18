/* convoLV2 -- LV2 convolution plugin
 *
 * Copyright (C) 2012 Robin Gareus <robin@gareus.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/* Usage information:
 *
 * Non-realtime, initialization:
 *  1)  clv_alloc();
 *  2)  clv_configure(); // can be called multiple times
 *  3)  clv_initialize();   // fix settings
 *
 * Realtime process
 *  4)  convolve();
 *
 * Non-rt, cleanup
 *  5A) clv_release(); // -> goto (2) or (3)
 * OR
 *  5B) clv_free(); // -> The End
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <math.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <assert.h>

#include <zita-convolver.h>
#include <sndfile.h>
#include <samplerate.h>
#include "convolution.h"

#if ZITA_CONVOLVER_MAJOR_VERSION != 3 && ZITA_CONVOLVER_MAJOR_VERSION != 4
# error "This programs requires zita-convolver 3 or 4"
#endif

#ifndef SRC_QUALITY // alternatives: SRC_SINC_FASTEST, SRC_SINC_MEDIUM_QUALITY, (SRC_ZERO_ORDER_HOLD, SRC_LINEAR)
# define SRC_QUALITY SRC_SINC_BEST_QUALITY
#endif

static pthread_mutex_t fftw_planner_lock = PTHREAD_MUTEX_INITIALIZER;

struct LV2convolv {
	Convproc *convproc;

	/* IR file */
	char *ir_fn; ///< path to IR file
	unsigned int chn_inp[MAX_CHANNEL_MAPS]; ///< I/O channel map: ir_map[id] = in-channel ;
	unsigned int chn_out[MAX_CHANNEL_MAPS]; ///< I/O channel map: ir_map[id] = out-channel ;
	unsigned int ir_chan[MAX_CHANNEL_MAPS]; ///< IR channel map: ir_chan[id] = file-channel;
	unsigned int ir_delay[MAX_CHANNEL_MAPS]; ///< pre-delay ; value >=0
	float ir_gain[MAX_CHANNEL_MAPS]; ///< IR-gain value: float -inf..+inf

	/* convolution settings*/
	unsigned int size; ///< max length of convolution computation
	float density; ///< density; 0<= dens <= 1.0 ; '0' = auto (1.0 / min(inchn,outchn)

	/* process settings */
	unsigned int fragment_size; ///< process period-size
};


/** read an audio-file completely into memory
 * allocated memory needs to be free()ed by caller
 */
static int audiofile_read (const char *fn, const int sample_rate, float **buf, unsigned int *n_ch, unsigned int *n_sp) {
	SF_INFO nfo;
	SNDFILE  *sndfile;
	float resample_ratio = 1.0;

	memset(&nfo, 0, sizeof(SF_INFO));

	if ((sndfile = sf_open(fn, SFM_READ, &nfo)) == 0) {
		return -1;
	}

	if (n_ch) *n_ch = (unsigned int) nfo.channels;
	if (n_sp) *n_sp = (unsigned int) nfo.frames;

	if (sample_rate != nfo.samplerate) {
		fprintf(stderr, "convoLV2: samplerate mismatch file:%d host:%d\n", nfo.samplerate, sample_rate);
		resample_ratio = (float) sample_rate / (float) nfo.samplerate;
	}

	if (buf) {
		const size_t frames_in = nfo.channels * nfo.frames;
		const size_t frames_out = nfo.channels * ceil(nfo.frames * resample_ratio);
		sf_count_t rd;
		float *rdb;
		*buf = (float*) malloc(frames_out*sizeof(float));

		if (!*buf) {
			fprintf (stderr, "convoLV2: memory allocation failed for IR audio-file buffer.\n");
			sf_close (sndfile);
			return (-2);
		}

		if (resample_ratio == 1.0) {
			rdb = *buf;
		} else {
			rdb = (float*) malloc(frames_in*sizeof(float));
			if (!rdb) {
				fprintf (stderr, "convoLV2: memory allocation failed for IR resample buffer.\n");
				sf_close (sndfile);
				free(*buf);
				return -1;
			}
		}

		if(nfo.frames != (rd=sf_readf_float(sndfile, rdb, nfo.frames))) {
			fprintf(stderr, "convoLV2: IR short read %ld of %ld\n", (long int) rd, (long int) nfo.frames);
			free(*buf);
			if (resample_ratio != 1.0) {
				free(rdb);
			}
			sf_close (sndfile);
			return -3;
		}

		if (resample_ratio != 1.0) {
			VERBOSE_printf("convoLV2: resampling IR %ld -> %ld [frames * channels].\n",
					(long int) frames_in,
					(long int) frames_out);
			SRC_STATE* src_state = src_new(SRC_QUALITY, nfo.channels, NULL);
			SRC_DATA src_data;

			src_data.input_frames  = nfo.frames;
			src_data.output_frames = nfo.frames * resample_ratio;
			src_data.end_of_input  = 1;
			src_data.src_ratio     = resample_ratio;
			src_data.input_frames_used = 0;
			src_data.output_frames_gen = 0;
			src_data.data_in       = rdb;
			src_data.data_out      = *buf;
			src_process(src_state, &src_data);
			VERBOSE_printf("convoLV2: resampled IR  %ld -> %ld [frames * channels].\n",
					src_data.input_frames_used * nfo.channels,
					src_data.output_frames_gen * nfo.channels);

			if (n_sp) *n_sp = (unsigned int) src_data.output_frames_gen;
			free(rdb);
			src_delete  (src_state);
		}
	}

	sf_close (sndfile);
	return (0);
}

LV2convolv *clv_alloc() {
	int i;
	LV2convolv *clv = (LV2convolv*) calloc(1, sizeof(LV2convolv));
	if (!clv) {
		return NULL;
	}

	clv->convproc = NULL;
	for (i = 0; i < MAX_CHANNEL_MAPS; ++i) {
		clv->ir_chan[i]  = i + 1;
		clv->chn_inp[i]  = i + 1;
		clv->chn_out[i]  = i + 1;
		clv->ir_delay[i] = 0;
		clv->ir_gain[i]  = 0.5f;
	}
	clv->ir_fn = NULL;
	clv->density = 0.f;
	clv->size = 0x00100000;
	return clv;
}

void clv_release (LV2convolv *clv) {
	if (!clv) return;
	if (clv->convproc) {
		clv->convproc->stop_process ();
		delete (clv->convproc);
	}
	clv->convproc = NULL;
}

void clv_clone_settings(LV2convolv *clv_new, LV2convolv *clv) {
	if (!clv) return;
	memcpy (clv_new, clv, sizeof(LV2convolv));
	clv_new->convproc = NULL;
	if (clv->ir_fn) {
		clv_new->ir_fn = strdup (clv->ir_fn);
	}
}

void clv_free (LV2convolv *clv) {
	if (!clv) return;
	clv_release (clv);
	free (clv->ir_fn);
	free (clv);
}

int clv_configure (LV2convolv *clv, const char *key, const char *value) {
	if (!clv) return 0;
	int n;
	if (strcasecmp (key, "convolution.ir.file") == 0) {
		free(clv->ir_fn);
		clv->ir_fn = strdup(value);
	} else if (!strncasecmp (key, "convolution.out.source.", 23)) {
		if (sscanf (key, "convolution.source.%d", &n) == 1) {
			if ((0 < n) && (n <= MAX_CHANNEL_MAPS))
				clv->chn_inp[n] = atoi(value);
		}
	} else if (!strncasecmp (key, "convolution.out.source.", 23)) {
		if (sscanf (key, "convolution.output.%d", &n) == 1) {
			if ((0 <= n) && (n < MAX_CHANNEL_MAPS))
				clv->chn_out[n] = atoi(value);
		}
	} else if (!strncasecmp (key, "convolution.ir.channel.", 23)) {
		if (sscanf (key, "convolution.ir.channel.%d", &n) == 1) {
			if ((0 <= n) && (n < MAX_CHANNEL_MAPS))
				clv->ir_chan[n] = atoi(value);
		}
	} else if (!strncasecmp (key, "convolution.ir.gain.", 20)) {
		if (sscanf (key, "convolution.ir.gain.%d", &n) == 1) {
			if ((0 <= n) && (n < MAX_CHANNEL_MAPS))
				clv->ir_gain[n] = atof(value);
		}
	} else if (!strncasecmp (key, "convolution.ir.delay.", 21)) {
		if (sscanf (key, "convolution.ir.delay.%d", &n) == 1) {
			if ((0 <= n) && (n < MAX_CHANNEL_MAPS))
				clv->ir_delay[n] = atoi(value);
		}
	} else if (strcasecmp (key, "convolution.maxsize") == 0) {
		clv->size = atoi(value);
		if (clv->size > 0x00400000) {
			clv->size = 0x00400000;
		}
		if (clv->size < 0x00001000) {
			clv->size = 0x00001000;
		}
	} else {
		return 0;
	}
	return 1; // OK
}

char *clv_dump_settings (LV2convolv *clv) {
	if (!clv) return NULL;

#define MAX_CFG_SIZE ( MAX_CHANNEL_MAPS * 160 + 60 + (clv->ir_fn ? strlen(clv->ir_fn) : 0) )
	int i;
	size_t off = 0;
	char *rv = (char*) malloc (MAX_CFG_SIZE * sizeof (char));
#undef MAX_CFG_SIZE

	for (i = 0; i < MAX_CHANNEL_MAPS; ++i) {
		// f=12 ; d= 3 ; v=10
		off+= sprintf (rv + off, "convolution.ir.gain.%d=%e\n",    i, clv->ir_gain[i]); // 22 + d + f
		off+= sprintf (rv + off, "convolution.ir.delay.%d=%d\n",   i, clv->ir_delay[i]);// 23 + d + v
		off+= sprintf (rv + off, "convolution.ir.channel.%d=%d\n", i, clv->ir_chan[i]); // 25 + d + d
		off+= sprintf (rv + off, "convolution.source.%d=%d\n",     i, clv->chn_inp[i]); // 21 + d + d
		off+= sprintf (rv + off, "convolution.output.%d=%d\n",     i, clv->chn_out[i]); // 21 + d + d
	}
	off+= sprintf(rv + off, "convolution.maxsize=%u\n", clv->size);                         // 21 + v
	return rv;
}

int clv_query_setting (LV2convolv *clv, const char *key, char *value, size_t val_max_len) {
	int rv = 0;
	if (!clv || !value || !key) {
		return -1;
	}

	if (strcasecmp (key, "convolution.ir.file") == 0) {
		if (clv->ir_fn) {
			if (strlen(clv->ir_fn) >= val_max_len) {
				rv = -1;
			}
			else {
				rv=snprintf(value, val_max_len, "%s", clv->ir_fn);
			}
		}
	}
	// TODO allow querying other settings
	return rv;
}


int clv_initialize (
		LV2convolv *clv,
		const unsigned int sample_rate,
		const unsigned int in_channel_cnt,
		const unsigned int out_channel_cnt,
		const unsigned int buffersize)
{
	unsigned int c;
	const unsigned int n_elem = in_channel_cnt * out_channel_cnt;

	/* zita-conv settings */
	const unsigned int options = 0;

	/* IR file */
	unsigned int n_chan = 0;
	unsigned int n_frames = 0;
	unsigned int max_size = 0;

	float *p = NULL;  /* temp. IR file buffer */
	float *gb = NULL; /* temp. gain-scaled IR file buffer */

	clv->fragment_size = buffersize;

	if (clv->convproc) {
		fprintf (stderr, "convoLV2: already initialized.\n");
		return (-1);
	}

	if (!clv->ir_fn) {
		fprintf (stderr, "convoLV2: No IR file was configured.\n");
		return -1;
	}

	if (access(clv->ir_fn, R_OK) != 0) {
		fprintf(stderr, "convoLV2: cannot stat IR: %s\n", clv->ir_fn);
		return -1;
	}

	pthread_mutex_lock(&fftw_planner_lock);

	clv->convproc = new Convproc;
	clv->convproc->set_options (options);
#if ZITA_CONVOLVER_MAJOR_VERSION == 3
	clv->convproc->set_density (clv->density);
#endif

	if (audiofile_read (clv->ir_fn, sample_rate, &p, &n_chan, &n_frames)) {
		fprintf(stderr, "convoLV2: failed to read IR.\n");
		goto errout;
	}

	if (n_frames == 0 || n_chan == 0) {
		fprintf(stderr, "convoLV2: invalid IR file.\n");
		goto errout;
	}

	for (c = 0; c < MAX_CHANNEL_MAPS; c++) {
		// TODO only relevant channels
		if (clv->ir_delay[c] > max_size) {
			max_size = clv->ir_delay[c];
		}
	}

	max_size += n_frames;

	if (max_size > clv->size) {
		max_size = clv->size;
	}

	VERBOSE_printf("convoLV2: max-convolution length %d samples (limit %d), period: %d samples\n", max_size, clv->size, buffersize);


	if (clv->convproc->configure (
				/*in*/  in_channel_cnt,
				/*out*/ out_channel_cnt,
				/*max-convolution length */ max_size,
				/*quantum*/  buffersize,
				/*min-part*/ buffersize /* must be >= fragm */,
				/*max-part*/ buffersize /* Convproc::MAXPART -> stich output every period */
#if ZITA_CONVOLVER_MAJOR_VERSION == 4
				, clv->density
#endif
				)) {
		fprintf (stderr, "convoLV2: Cannot initialize convolution engine.\n");
		goto errout;
	}

	gb = (float*) malloc (n_frames * sizeof(float));
	if (!gb) {
		fprintf (stderr, "convoLV2: memory allocation failed for convolution buffer.\n");
		goto errout;
	}

	VERBOSE_printf("convoLV2: Proc: in: %d, out: %d || IR-file: %d chn, %d samples\n",
			in_channel_cnt, out_channel_cnt, n_chan, n_frames);

	// TODO use pre-configured channel-map (from state), IFF set and valid for the current file

	// reset channel map
	for (c = 0; c < MAX_CHANNEL_MAPS; ++c) {
		clv->ir_chan[c] = 0;
		clv->chn_inp[c] = 0;
		clv->chn_out[c] = 0;
	}

	// follow channel map conventions
	if (n_elem == n_chan) {
		// exact match: for every input-channel, iterate over all outputs
		// eg.  1: L -> L , 2: L -> R, 3: R -> L, 4: R -> R
		for (c = 0; c < n_chan && c < MAX_CHANNEL_MAPS; ++c) {
			clv->ir_chan[c] = 1 + c;
			clv->chn_inp[c] = 1 + ((c / out_channel_cnt) % in_channel_cnt);
			clv->chn_out[c] = 1 +  (c % out_channel_cnt);
		}
	}
	else if (n_elem > n_chan) {
		VERBOSE_printf("convoLV2: IR file has too few channels for given processor config.\n");
		// missing some channels, first assign  in -> out, then x-over
		// eg.  1: L -> L , 2: R -> R,  3: L -> R,  4: R -> L
		// this allows to e.g load a 2-channel (stereo) IR into a
		// 2x2 true-stereo effect instance
		for (c = 0; c < n_chan && c < MAX_CHANNEL_MAPS; ++c) {
			clv->ir_chan[c] = 1 + c;
			clv->chn_inp[c] = 1 + (c % in_channel_cnt);
			clv->chn_out[c] = 1 + (((c + c / in_channel_cnt) % in_channel_cnt) % out_channel_cnt);
		}
		// assign mono input to 1: L -> L , 2: R -> R,
		for (;n_chan == 1 && c < 2; ++c) {
			clv->ir_chan[c] = 1;
			clv->chn_inp[c] = 1 + (c % in_channel_cnt);
			clv->chn_out[c] = 1 + (c % out_channel_cnt);
		}
	}
	else {
		assert (n_elem < n_chan);
		VERBOSE_printf("convoLV2: IR file has too many channels for given processor config.\n");
		// allow loading a quad file to a mono-in stereo-out
		// eg.  1: L -> L , 2: L -> R
		for (c = 0; c < n_elem && c < MAX_CHANNEL_MAPS; ++c) {
			clv->ir_chan[c] = 1 + c;
			clv->chn_inp[c] = 1 + ((c / out_channel_cnt) % in_channel_cnt);
			clv->chn_out[c] = 1 +  (c % out_channel_cnt);
		}
	}

	// assign channel map to convolution engine
	for (c = 0; c < MAX_CHANNEL_MAPS; ++c) {
		unsigned int i;
		if (clv->chn_inp[c] == 0 || clv->chn_out[c] == 0 || clv->ir_chan[c] == 0) {
			continue;
		}

		assert (clv->ir_chan[c] <= n_chan);

		for (i = 0; i < n_frames; ++i) {
			// decode interleaved channels, apply gain scaling
			gb[i] = p[i * n_chan + clv->ir_chan[c] - 1] * clv->ir_gain[c];
		}

		VERBOSE_printf ("convoLV2: SET in %d -> out %d [IR chn:%d gain:%+.3f dly:%d]\n",
				clv->chn_inp[c],
				clv->chn_out[c],
				clv->ir_chan[c],
				clv->ir_gain[c],
				clv->ir_delay[c]
			       );

		clv->convproc->impdata_create (
				clv->chn_inp[c] - 1,
				clv->chn_out[c] - 1,
				1, gb, clv->ir_delay[c], clv->ir_delay[c] + n_frames);
	}

	free(gb); gb = NULL;
	free(p);  p  = NULL;

#if 1 // INFO
	clv->convproc->print (stderr);
#endif

	if (clv->convproc->start_process (0, 0)) {
		fprintf(stderr, "convoLV2: Cannot start processing.\n");
		goto errout;
	}

	pthread_mutex_unlock(&fftw_planner_lock);
	return 0;

errout:
	free(gb);
	free(p);
	delete(clv->convproc);
	clv->convproc = NULL;
	pthread_mutex_unlock(&fftw_planner_lock);
	return -1;
}

int clv_is_active (LV2convolv *clv) {
	if (!clv || !clv->convproc || !clv->ir_fn) {
		return 0;
	}
	return 1;
}

static void silent_output(float * const * outbuf, size_t n_channels, size_t n_samples) {
	unsigned int c;
	for (c = 0; c < n_channels; ++c) {
		memset (outbuf[c], 0, n_samples * sizeof(float));
	}
}

int clv_convolve (LV2convolv *clv,
		const float * const * inbuf,
		float * const * outbuf,
		const unsigned int in_channel_cnt,
		const unsigned int out_channel_cnt,
		const unsigned int n_samples,
		const float output_gain)
{
	unsigned int c;

	if (!clv || !clv->convproc) {
		silent_output(outbuf, out_channel_cnt, n_samples);
		return (0);
	}

	if (clv->convproc->state () == Convproc::ST_WAIT) {
		clv->convproc->check_stop ();
	}

	if (clv->fragment_size != n_samples) {
		silent_output(outbuf, out_channel_cnt, n_samples);
		return -1;
	}

#if 1
	if (clv->convproc->state () != Convproc::ST_PROC) {
		/* This cannot happen in sync-mode, but... */
		assert (0);
		silent_output(outbuf, out_channel_cnt, n_samples);
		return (n_samples);
	}
#endif

	for (c = 0; c < in_channel_cnt; ++c)
#if 0 // no denormal protection
		memcpy (clv->convproc->inpdata (c), inbuf[c], n_samples * sizeof (float));
#else // prevent denormals
	{
		unsigned int i;
		float *id = clv->convproc->inpdata(c);
		for (i = 0; i < n_samples; ++i) {
			id[i] = inbuf[c][i] + 1e-20f;
		}
	}
#endif

	int f = clv->convproc->process (false);

	if (f /*&Convproc::FL_LOAD)*/ ) {
		/* Note this will actually never happen in sync-mode */
		assert (0);
		silent_output(outbuf, out_channel_cnt, n_samples);
		return (n_samples);
	}

	for (c = 0; c < out_channel_cnt; ++c) {
		if (output_gain == 1.0) {
			memcpy (outbuf[c], clv->convproc->outdata (c), n_samples * sizeof (float));
		} else {
			unsigned int s;
			float const * const od = clv->convproc->outdata (c);
			for (s = 0; s < n_samples; ++s) {
				outbuf[c][s] = od[s] * output_gain;
			}
		}
	}

	return (n_samples);
}
