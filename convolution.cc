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
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.  
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

#define NOGUIFORASSIGNMENTS 1 // hack currently there is no UI for assigning channels - this allows 1->1, 2->2, 2->2(true stereo)

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <math.h>
#include <string.h>
#include <stdint.h>

#include <zita-convolver.h>
#include <sndfile.h>
#include <samplerate.h>
#include "convolution.h"

#ifndef SRC_QUALITY // alternatives: SRC_SINC_FASTEST, SRC_SINC_BEST_QUALITY, (SRC_ZERO_ORDER_HOLD, SRC_LINEAR)
#define SRC_QUALITY SRC_SINC_MEDIUM_QUALITY
#endif

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


#if ZITA_CONVOLVER_MAJOR_VERSION != 3
#error "This programs requires zita-convolver 3.x.x"
#endif

#define DENORMAL_HACK (1e-20)

/** read an audio-file completely into memory
 * allocated memory needs to be free()ed by caller
 */
int audiofile_read (const char *fn, const int sample_rate, float **buf, unsigned int *n_ch, unsigned int *n_sp) {
  SF_INFO nfo;
  SNDFILE  *sndfile;
  float resample_ratio = 1.0;

  memset(&nfo, 0, sizeof(SF_INFO));

  if ((sndfile = sf_open(fn, SFM_READ, &nfo)) == 0)
    return -1;

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
      fprintf(stderr, "convoLV2: resampling IR %ld -> %ld [frames * channels].\n",
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
      fprintf(stderr, "convoLV2: resampled IR  %ld -> %ld [frames * channels].\n",
	  src_data.input_frames_used * nfo.channels,
	  src_data.output_frames_gen * nfo.channels);

      if (n_sp) *n_sp = (unsigned int) src_data.output_frames_gen;
      free(rdb);
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
  for (i=0;i<MAX_CHANNEL_MAPS; i++) {
    clv->ir_chan[i]   = i+1;
#ifndef NOGUIFORASSIGNMENTS
    clv->chn_inp[i]   = i+1;
    clv->chn_out[i]   = i+1;
#else
    clv->chn_inp[i]   = (i%2)+1;
    clv->chn_out[i]   = ((i+i/2)%2)+1;
#endif
    clv->ir_delay[i]  = 0;
    clv->ir_gain[i]   = 0.5;
  }
  clv->ir_fn = NULL;

  clv->density = 0.0;
  clv->size = 204800;
  //fprintf(stderr,"%s", clv_dump_settings(clv));
  return clv;
}

void clv_release (LV2convolv *clv) {
  if (!clv) return;
  if (clv->convproc) {
    clv->convproc->stop_process ();
    delete(clv->convproc);
  }
  clv->convproc = NULL;
}

void clv_clone_settings(LV2convolv *clv_new, LV2convolv *clv) {
  if (!clv) return;
  memcpy(clv_new, clv, sizeof(LV2convolv));
  clv_new->convproc = NULL;
  if (clv->ir_fn) {
    clv_new->ir_fn = strdup(clv->ir_fn);
  }
}

void clv_free (LV2convolv *clv) {
  if (!clv) return;
  clv_release(clv);
  if (clv->ir_fn) {
    free(clv->ir_fn);
  }
  free(clv);
}

int clv_configure (LV2convolv *clv, const char *key, const char *value) {
  if (!clv) return 0;
  int n;
  if (strcasecmp (key, (char*)"convolution.ir.file") == 0) {
    free(clv->ir_fn);
    clv->ir_fn = strdup(value);
  } else if (!strncasecmp (key, (char*)"convolution.out.source.", 23)) {
    if (sscanf (key, (char*)"convolution.source.%d", &n) == 1) {
      if ((0 < n) && (n <= MAX_CHANNEL_MAPS))
	clv->chn_inp[n] = atoi(value);
    }
  } else if (!strncasecmp (key, (char*)"convolution.out.source.", 23)) {
    if (sscanf (key, (char*)"convolution.output.%d", &n) == 1) {
      if ((0 <= n) && (n < MAX_CHANNEL_MAPS))
	clv->chn_out[n] = atoi(value);
    }
  } else if (!strncasecmp (key, (char*)"convolution.ir.channel.", 23)) {
    if (sscanf (key, (char*)"convolution.ir.channel.%d", &n) == 1) {
      if ((0 <= n) && (n < MAX_CHANNEL_MAPS))
	clv->ir_chan[n] = atoi(value);
    }
  } else if (!strncasecmp (key, (char*)"convolution.ir.gain.", 20)) {
    if (sscanf (key, (char*)"convolution.ir.gain.%d", &n) == 1) {
      if ((0 <= n) && (n < MAX_CHANNEL_MAPS))
	clv->ir_gain[n] = atof(value);
    }
  } else if (!strncasecmp (key, (char*)"convolution.ir.delay.", 21)) {
    if (sscanf (key, (char*)"convolution.ir.delay.%d", &n) == 1) {
      if ((0 <= n) && (n < MAX_CHANNEL_MAPS))
	clv->ir_delay[n] = atoi(value);
    }
  } else if (strcasecmp (key, (char*)"convolution.size") == 0) {
    clv->size = atoi(value);
    if (clv->size > 0x00100000) {
      clv->size = 0x00100000;
    }
  } else {
    return 0;
  }
  return 1; // OK
}

char *clv_dump_settings (LV2convolv *clv) {
  if (!clv) return NULL;
  int i;
  size_t off = 0;
#define MAX_CFG_SIZE ( MAX_CHANNEL_MAPS * 158 + 50 + (clv->ir_fn?strlen(clv->ir_fn):0) )
  char *rv = (char*) malloc(MAX_CFG_SIZE * sizeof (char));

  for (i=0;i<MAX_CHANNEL_MAPS;i++) {
    // f=12 ; d= 3 ; v=10
    off+= sprintf(rv + off, "convolution.ir.gain.%d=%e\n",    i, clv->ir_gain[i]); // 22 + d + f
    off+= sprintf(rv + off, "convolution.ir.delay.%d=%d\n",   i, clv->ir_delay[i]);// 23 + d + v
    off+= sprintf(rv + off, "convolution.ir.channel.%d=%d\n", i, clv->ir_chan[i]); // 25 + d + d
    off+= sprintf(rv + off, "convolution.source.%d=%d\n",     i, clv->chn_inp[i]); // 21 + d + d
    off+= sprintf(rv + off, "convolution.output.%d=%d\n",     i, clv->chn_out[i]); // 21 + d + d
  }
  off+= sprintf(rv + off, "convolution.size=%u\n", clv->size);                     // 18 + v
  off+= sprintf(rv + off, "convolution.ir.file=%s\n", clv->ir_fn?clv->ir_fn:"");   // 21 + s
  fprintf(stderr, "%d / %d \n", off, MAX_CFG_SIZE);
  return rv;
}

int clv_query_setting (LV2convolv *clv, const char *key, char *value, size_t val_max_len) {
  int rv = 0;
  if (!clv || !value || !key) return -1;

  if (strcasecmp (key, (char*)"convolution.ir.file") == 0) {
    if (clv->ir_fn) {
      if (strlen(clv->ir_fn) >= val_max_len)
	rv = -1;
      else
	rv=snprintf(value, val_max_len, "%s", clv->ir_fn);
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
  unsigned int i,c;

  /* zita-conv settings */
  const unsigned int options = 0;

  /* IR file */
  unsigned int nchan = 0;
  unsigned int nfram = 0;
  float *p = NULL; /* temp. IR file buffer */
  float *gb; /* temp. gain-scaled IR file buffer */

  clv->fragment_size = buffersize;

  if (zita_convolver_major_version () != ZITA_CONVOLVER_MAJOR_VERSION) {
    fprintf (stderr, "convoLV2: Zita-convolver version does not match.\n");
    return -1;
  }

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

  clv->convproc = new Convproc;
  clv->convproc->set_options (options);
  clv->convproc->set_density (clv->density);

  if (clv->convproc->configure (
	/*in*/  in_channel_cnt,
	/*out*/ out_channel_cnt,
	/*max-convolution length */ clv->size,
	/*quantum*/  buffersize,
	/*min-part*/ buffersize /* must be >= fragm */,
	/*max-part*/ buffersize /* Convproc::MAXPART -> stich output every period */
	)) {
    fprintf (stderr, "convoLV2: Cannot initialize convolution engine.\n");
    delete(clv->convproc);
    clv->convproc = NULL;
    return -1;
  }

  if (audiofile_read(clv->ir_fn, sample_rate, &p, &nchan, &nfram)) {
    fprintf(stderr, "convoLV2: failed to read IR.\n");
    delete(clv->convproc);
    clv->convproc = NULL;
    return -1;
  }

  gb = (float*) malloc(nfram*sizeof(float));
  if (!gb) {
    fprintf (stderr, "convoLV2: memory allocation failed for convolution buffer.\n");
    free(p);
    return -1;
  }

  fprintf (stderr, "convoLV2: CFG %din, %dout | IR: %dchn, %dsamples\n",
      in_channel_cnt, out_channel_cnt, nchan, nfram);

  for (c=0; c < MAX_CHANNEL_MAPS; c++) {
    if (clv->chn_inp[c]==0 || clv->chn_inp[c] > in_channel_cnt) break;

#ifdef NOGUIFORASSIGNMENTS
    if (c >= nchan && c>1) break; // XXX temp hack, -- XXX should be removed once UI assignments are possible
#endif

    if (clv->ir_chan[c] > nchan || clv->ir_chan[c] < 1) {
      fprintf(stderr, "convoLV2: invalid IR-file channel assigned; expected: 1 <= %d <= %d\n", clv->ir_chan[c], nchan);
#ifndef NOGUIFORASSIGNMENTS // fail if assignment is incorrect
      free(p); free(gb);
      delete(clv->convproc);
      clv->convproc = NULL;
      return -1;
#else
      clv->ir_chan[c] = ((clv->ir_chan[c]-1)%nchan)+1;
      fprintf(stderr, "convoLV2: using IR-file channel %d\n", clv->ir_chan[c]);
#endif
    }
    if (clv->ir_delay[c] < 0) {
      fprintf(stderr, "convoLV2: invalid delay; expected: 0 <= %d\n", clv->ir_delay[c]);
      free(p); free(gb);
      delete(clv->convproc);
      clv->convproc = NULL;
      return -1;
    }

    for (i=0; i < nfram; ++i) gb[i] = p[i*nchan + clv->ir_chan[c]-1] * clv->ir_gain[c];

    fprintf(stderr, "convoLV2: SET in %d -> out %d [IR chn:%d gain:%+.3f dly:%d]\n",
	((clv->chn_inp[c]-1)%in_channel_cnt) +1,
	((clv->chn_out[c]-1)%out_channel_cnt) +1,
	clv->ir_chan[c],
	clv->ir_gain[c],
	clv->ir_delay[c]
	);
    clv->convproc->impdata_create (
	(clv->chn_inp[c]-1)%in_channel_cnt,
	(clv->chn_out[c]-1)%out_channel_cnt,
	1, gb, clv->ir_delay[c], clv->ir_delay[c] + nfram);
  }

  free(gb);
  free(p);

#if 1 // INFO
  fprintf(stderr, "\n");
  clv->convproc->print (stderr);
#endif

  if (clv->convproc->start_process (0, 0)) {
    fprintf(stderr, "convoLV2: Cannot start processing.\n");
    delete(clv->convproc);
    clv->convproc = NULL;
    return -1;
  }

  return 0;
}

int clv_is_active (LV2convolv *clv) {
  if (!clv || !clv->convproc) {
    return 0;
  }
  return 1;
}

void silent_output(float * const * outbuf, size_t n_channels, size_t n_samples) {
  unsigned int c;
  for (c=0; c < n_channels; ++c)
    memset(outbuf[c], 0, n_samples * sizeof(float));
}

/*
 *
 */
int clv_convolve (LV2convolv *clv, const float * const * inbuf, float * const * outbuf, const unsigned int n_channels, const unsigned int n_samples) {
  unsigned int i,c;

  if (!clv || !clv->convproc) {
    return (0);
  }

  if (clv->convproc->state () == Convproc::ST_WAIT) clv->convproc->check_stop ();

  if (clv->fragment_size != n_samples) {
    silent_output(outbuf, n_channels, n_samples);
    return (-1);
  }

  if (n_channels > MAX_CHANNEL_MAPS) {
    silent_output(outbuf, n_channels, n_samples);
    return (-2);
  }

  if (clv->convproc->state () != Convproc::ST_PROC) {
    /* Note this will actually never happen in sync-mode */
    fprintf(stderr, "fons br0ke libzita-resampler :)\n");
    silent_output(outbuf, n_channels, n_samples);
    return (n_samples);
  }

  for (c = 0; c < n_channels; ++c)
#if 0
    memcpy (clv->convproc->inpdata (c), inbuf[c], n_samples * sizeof (float));
#else // prevent denormals
  {
    float *id = clv->convproc->inpdata(c);
    for (i=0;i<n_samples;++i) {
      id[i] = inbuf[c][i] + DENORMAL_HACK;
    }
  }
#endif

  int f = clv->convproc->process (false);

  if (f /*&Convproc::FL_LOAD)*/ ) {
    /* Note this will actually never happen in sync-mode */
    fprintf(stderr, "fons br0ke libzita-resampler :).\n");
    silent_output(outbuf, n_channels, n_samples);
    return (n_samples);
  }

  for (c = 0; c < n_channels; ++c)
    memcpy (outbuf[c], clv->convproc->outdata (c), n_samples * sizeof (float));

  return (n_samples);
}

/* vi:set ts=8 sts=2 sw=2: */
