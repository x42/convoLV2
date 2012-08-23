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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <math.h>
#include <string.h>
#include <stdint.h>

#include <zita-convolver.h>
#include <sndfile.h>
#include "convolution.h"

struct LV2convolv {
  Convproc *convproc;
  char *ir_fn;
  unsigned int ir_chan[MAX_AUDIO_CHANNELS];
  unsigned int ir_delay[MAX_AUDIO_CHANNELS];
  float ir_gain[MAX_AUDIO_CHANNELS];
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
  int ok = -2;

  memset(&nfo, 0, sizeof(SF_INFO));

  if ((sndfile = sf_open(fn, SFM_READ, &nfo)) == 0)
    return -1;
  if (sample_rate != nfo.samplerate) {
    fprintf(stderr, "convoLV2: samplerate mismatch file:%d synth:%d\n", nfo.samplerate, sample_rate);
  }

  if (n_ch) *n_ch = (unsigned int) nfo.channels;
  if (n_sp) *n_sp = (unsigned int) nfo.frames;

  if (buf) {
    const size_t frames = nfo.channels*nfo.frames;
    *buf = (float*) malloc(frames*sizeof(float));
    if (*buf) {
      sf_count_t rd;
      if(nfo.frames == (rd=sf_readf_float(sndfile, *buf, nfo.frames))) {
	ok=0;
      } else {
	fprintf(stderr, "convoLV2: IR short read %ld of %ld\n", (long int) rd, (long int) nfo.frames);
      }
    } else {
      fprintf (stderr, "convoLV2: memory allocation failed for IR audio-file buffer.\n");
    }
  }
  sf_close (sndfile);
  return (ok);
}

LV2convolv *allocConvolution() {
  LV2convolv *clv = (LV2convolv*) calloc(1, sizeof(LV2convolv));
  if (!clv) {
    return NULL;
  }
  clv->convproc = NULL;
  clv->ir_chan[0] = 1; clv->ir_chan[1] = 2;
  clv->ir_delay[0]  = clv->ir_delay[1] = 0;
  clv->ir_gain[0] = clv->ir_gain[1] = 0.5;
  clv->ir_fn = NULL;
}

void freeConvolution (LV2convolv *clv) {
  if (clv->convproc) {
    clv->convproc->stop_process ();
    delete(clv->convproc);
  }
  if (clv->ir_fn) {
    free(clv->ir_fn);
  }
  free(clv);
}

int configConvolution (LV2convolv *clv, const char *key, const char *value) {
  double d;
  int n;
  if (strcasecmp (key, (char*)"convolution.ir.file") == 0) {
    free(clv->ir_fn);
    clv->ir_fn = strdup(value);
  } else if (!strncasecmp (key, (char*)"convolution.ir.channel.", 23)) {
    if (sscanf (key, (char*)"convolution.ir.channel.%d", &n) == 1) {
      if ((0 < n) && (n <= MAX_AUDIO_CHANNELS))
	clv->ir_chan[n-1] = atoi(value);
    }
  } else if (!strncasecmp (key, (char*)"convolution.ir.gain.", 20)) {
    if (sscanf (key, (char*)"convolution.ir.gain.%d", &n) == 1) {
      if ((0 < n) && (n <= MAX_AUDIO_CHANNELS))
	clv->ir_gain[n-1] = atof(value);
    }
  } else if (!strncasecmp (key, (char*)"convolution.ir.delay.", 21)) {
    if (sscanf (key, (char*)"convolution.ir.delay.%d", &n) == 1) {
      if ((0 < n) && (n <= MAX_AUDIO_CHANNELS))
	clv->ir_delay[n-1] = atoi(value);
    }
  } else {
    return 0;
  }
  return 1; // OK
}


int initConvolution (
    LV2convolv *clv,
    const unsigned int sample_rate,
    const unsigned int channels,
    const unsigned int buffersize,
    int sched_priority,
    int sched_policy)
{
  unsigned int i,c;

  /* zita-conv settings */
  const float dens = 0;
  const unsigned int size = 204800;
  const unsigned int options = 0;

  /* IR file */
  unsigned int nchan = 0;
  unsigned int nfram = 0;
  float *p = NULL; /* temp. IR file buffer */
  float *gb; /* temp. gain-scaled IR file buffer */

  if (zita_convolver_major_version () != ZITA_CONVOLVER_MAJOR_VERSION) {
    fprintf (stderr, "convoLV2: Zita-convolver version does not match.\n");
    exit(1);
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
  clv->convproc->set_density (dens);

  if (clv->convproc->configure (
	/*in*/channels,
	/*out*/ channels,
	size,
	/*fragm*/buffersize,
	/*min-part*/ buffersize,
	/*max-part*/ buffersize /*Convproc::MAXPART*/
	)) {
    fprintf (stderr, "convoLV2: Cannot initialize convolution engine.\n");
    return -1;
  }

  if (audiofile_read(clv->ir_fn, sample_rate, &p, &nchan, &nfram)) {
    fprintf(stderr, "convoLV2: failed to read IR \n");
    return -1;
  }

  free(clv->ir_fn); clv->ir_fn=NULL;

  gb = (float*) malloc(nfram*sizeof(float));
  if (!gb) {
    fprintf (stderr, "convoLV2: memory allocation failed for convolution buffer.\n");
    return -1;
  }

  for (c=0; c < MAX_AUDIO_CHANNELS, c < channels; c++) {

    if (clv->ir_chan[c] > nchan || clv->ir_chan[c] < 1) {
      fprintf(stderr, "convoLV2: invalid channel in IR file. required: 1 <= %d <= %d\n", clv->ir_chan[c], nchan);
      return -1;
    }
    if (clv->ir_delay[c] < 0) {
      fprintf(stderr, "convoLV2: invalid delay. required: 0 <= %d\n", clv->ir_delay[c]);
      return -1;
    }

    for (i=0; i < nfram; ++i) gb[i] = p[i*nchan + clv->ir_chan[c]-1] * clv->ir_gain[c];
    clv->convproc->impdata_create (c, c, 1, gb, clv->ir_delay[c], clv->ir_delay[c] + nfram);
  }

  free(gb);
  free(p);

#if 1 // INFO
  fprintf(stderr, "\n");
  clv->convproc->print (stderr);
#endif

  if (clv->convproc->start_process (sched_priority, sched_policy)) {
    fprintf(stderr, "convoLV2: Cannot start processing.\n");
    return -1;
  }

  return 0;
}

void copy_input_to_output(const float * const * inbuf, float * const * outbuf, size_t n_channels, size_t n_samples) {
  unsigned int c;
  for (c=0; c < n_channels; ++c)
    memcpy(outbuf[c], inbuf[c], n_samples * sizeof(float));
}

/*
 *
 */
void convolve (LV2convolv *clv, const float * const * inbuf, float * const * outbuf, size_t n_channels, size_t n_samples) {
  unsigned int i,c;

  if (clv->convproc->state () == Convproc::ST_WAIT) clv->convproc->check_stop ();

  if (n_channels > MAX_AUDIO_CHANNELS) {
    // XXX fail
    return;
  }

  if (clv->convproc->state () != Convproc::ST_PROC) {
    copy_input_to_output(inbuf, outbuf, n_channels, n_samples);
    return;
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
    copy_input_to_output(inbuf, outbuf, n_channels, n_samples);
    return;
  }

  for (c = 0; c < n_channels; ++c)
    memcpy (outbuf[c], clv->convproc->outdata (c), n_samples * sizeof (float));
}

/* vi:set ts=8 sts=2 sw=2: */
