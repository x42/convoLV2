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
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "convolution.h"

#include "lv2/lv2plug.in/ns/ext/atom/forge.h"
#include "lv2/lv2plug.in/ns/lv2core/lv2.h"
#include "lv2/lv2plug.in/ns/ext/worker/worker.h"

#include "./uris.h"

typedef enum {
  P_INPUT      = 0,
  P_OUTPUT     = 1,
  P_CONTROL    = 2,
  P_NOTIFY     = 3,
} PortIndex;

typedef struct {
  LV2_URID_Map*        map;
  LV2_Worker_Schedule *schedule;

  LV2_Atom_Forge forge;

  float* input;
  float* output;
  const LV2_Atom_Sequence* control_port;
  LV2_Atom_Sequence*       notify_port;

  LV2_Atom_Forge_Frame notify_frame;

  ConvoLV2URIs uris;

  LV2convolv *instance;
  int bufsize;
  int rate;
  int reinit_in_progress;

} convoLV2;

static LV2_Handle
instantiate(const LV2_Descriptor*     descriptor,
            double                    rate,
            const char*               bundle_path,
            const LV2_Feature* const* features)
{
  int i;
  convoLV2* clv = (convoLV2*)calloc(1, sizeof(convoLV2));
  if(!clv) { return NULL ;}

  for (i = 0; features[i]; ++i) {
    if (!strcmp(features[i]->URI, LV2_URID__map)) {
      clv->map = (LV2_URID_Map*)features[i]->data;
    } else if (!strcmp(features[i]->URI, LV2_WORKER__schedule)) {
      clv->schedule = (LV2_Worker_Schedule*)features[i]->data;
    }
  }

  if (!clv->map) {
    fprintf(stderr, "Missing feature uri:map.\n");
    free(clv);
    return NULL;
  }

  if (!clv->schedule) {
    fprintf(stderr, "Missing feature work:schedule.\n");
    free(clv);
    return NULL;
  }

  /* Map URIs and initialise forge */
  map_convolv2_uris(clv->map, &clv->uris);
  lv2_atom_forge_init(&clv->forge, clv->map);

  if (!(clv->instance = allocConvolution())) {
    free(clv);
    return NULL;
  }

  clv->bufsize = 1024;
  clv->rate = rate;
  clv->reinit_in_progress = 0;

  /* setting an IR file is required */
  configConvolution(clv->instance, "convolution.ir.file", "/tmp/example_ir-48k.wav");

#if 0 // config examples
  /* use channel 2 of IR file for channel 1 of convolution
   * channel count = 1..n
   */
  configConvolution(clv->instance, "convolution.ir.channel.1", "2");

  /* apply GAIN factor of 0.5 to the IR-sample used for channel 1 */
  configConvolution(clv->instance, "convolution.ir.gain.1", "0.5");

  /* offset the IR sample used for channel 1
   * value must be >= 0 */
  configConvolution(clv->instance, "convolution.ir.delay.1", "0");
#endif

  if (initConvolution(clv->instance, clv->rate,
	/*num channels*/ 1,
	/*64 <= buffer-size <=4096*/ clv->bufsize))
  {
    freeConvolution(clv->instance);
    free(clv);
    return NULL;
  }

  return (LV2_Handle)clv;
}


/*
   Do work in a non-realtime thread.

   This is called for every piece of work scheduled in the audio thread using
   self->schedule->schedule_work().  A reply can be sent back to the audio
   thread using the provided respond function.
*/
static LV2_Worker_Status
work(LV2_Handle                  instance,
     LV2_Worker_Respond_Function respond,
     LV2_Worker_Respond_Handle   handle,
     uint32_t                    size,
     const void*                 data)
{
  convoLV2* clv = (convoLV2*)instance;
  LV2convolv *newinst = allocConvolution();

  if (!newinst) {
    clv->reinit_in_progress = 0;
    return LV2_WORKER_ERR_NO_SPACE; // OOM
  }

  cloneConvolutionParams(newinst, clv->instance);
  if (initConvolution(newinst, clv->rate,
	/*num channels*/ 1,
	/*64 <= buffer-size <=4096*/ clv->bufsize));

  respond(handle, sizeof(newinst), &newinst);
  return LV2_WORKER_SUCCESS;
}

/**
   Handle a response from work() in the audio thread.

   When running normally, this will be called by the host after run().  When
   freewheeling, this will be called immediately at the point the work was
   scheduled.
*/
static LV2_Worker_Status
work_response(LV2_Handle  instance,
              uint32_t    size,
              const void* data)
{
  convoLV2* clv = (convoLV2*)instance;
  LV2convolv *old = clv->instance;
  clv->instance = *(LV2convolv*const*) data;
  freeConvolution(old);
  clv->reinit_in_progress = 0;
  return LV2_WORKER_SUCCESS;
}


static void
connect_port(LV2_Handle instance,
             uint32_t   port,
             void*      data)
{
  convoLV2* clv = (convoLV2*)instance;

  switch ((PortIndex)port) {
    case P_INPUT:
      clv->input = (float*)data;
      break;
    case P_OUTPUT:
      clv->output = (float*)data;
      break;
    case P_CONTROL:
      clv->control_port = (const LV2_Atom_Sequence*)data;
      break;
    case P_NOTIFY:
      clv->notify_port = (LV2_Atom_Sequence*)data;
      break;
  }
}

static void
static void
run(LV2_Handle instance, uint32_t n_samples)
{
  convoLV2* clv = (convoLV2*)instance;
  ConvoLV2URIs* uris = &clv->uris;
  int rv = 0;

  const float *input[MAX_AUDIO_CHANNELS];
  float *output[MAX_AUDIO_CHANNELS];
  input[0] = clv->input;
  output[0] = clv->output;

  /* Set up forge to write directly to notify output port. */
  const uint32_t notify_capacity = clv->notify_port->atom.size;
  lv2_atom_forge_set_buffer(&clv->forge,
			    (uint8_t*)clv->notify_port,
			    notify_capacity);

  /* Start a sequence in the notify output port. */
  lv2_atom_forge_sequence_head(&clv->forge, &clv->notify_frame, 0);

  /* Read incoming events */
  LV2_ATOM_SEQUENCE_FOREACH(clv->control_port, ev) {
    //clv->frame_offset = ev->time.frames;
    if (is_object_type(uris, ev->body.type)) {
      const LV2_Atom_Object* obj = (LV2_Atom_Object*)&ev->body;
      if (obj->body.otype == uris->patch_Set) {
	//clv->schedule->schedule_work(clv->schedule->handle, lv2_atom_total_size(&ev->body), &ev->body);
#if 1 // TODO this is not RT-save -> worker
	const LV2_Atom_Object* obj = (const LV2_Atom_Object*) &ev->body;
	const LV2_Atom* file_path = read_set_file(&clv->uris, obj);
	const char *fn = (char*)(file_path+1);
	//const char *fn = LV2_ATOM_BODY_CONST(file_path);
	fprintf(stderr, "load %s\n", fn);
	configConvolution(clv->instance, "convolution.ir.file", fn);
	clv->bufsize = 0; // force reload below
#endif
      } else {
	fprintf(stderr, "Unknown object type %d\n", obj->body.otype);
      }
    }
  }

  if (clv->bufsize != n_samples) {
    // re-initialize convolver
    clv->bufsize = n_samples;
    if (!clv->reinit_in_progress) {
      clv->reinit_in_progress = 1;
      clv->schedule->schedule_work(clv->schedule->handle, 0, NULL);
      clv = (convoLV2*)instance;
    }
  }

  rv = convolve(clv->instance, input, output, /*num channels*/1, n_samples);

  if (rv<0) {
#if 0 // DEBUG
    if (rv==-1)
      fprintf(stderr, "fragment size mismatch -> reconfiguration is needed\n"); // XXX non RT
    else /* -2 */
      fprintf(stderr, "channel count mismatch -> reconfiguration is needed\n"); // XXX non RT
#endif
    if (!clv->reinit_in_progress) {
      clv->reinit_in_progress = 1;
      clv->schedule->schedule_work(clv->schedule->handle, 0, NULL);
    }
  }

  convolve(clv->instance, input, output, /*num channels*/1, n_samples);
}

static void
cleanup(LV2_Handle instance)
{
  convoLV2* clv = (convoLV2*)instance;
  freeConvolution(clv->instance);
  free(instance);
}

const void*
extension_data(const char* uri)
{
  static const LV2_Worker_Interface worker = { work, work_response, NULL };
  if (!strcmp(uri, LV2_WORKER__interface)) {
    return &worker;
  }
  return NULL;
}

static const LV2_Descriptor descriptor = {
  CONVOLV2_URI,
  instantiate,
  connect_port,
  NULL, // activate,
  run,
  NULL, // deactivate,
  cleanup,
  extension_data
};

LV2_SYMBOL_EXPORT
const LV2_Descriptor*
lv2_descriptor(uint32_t index)
{
  switch (index) {
  case 0:
    return &descriptor;
  default:
    return NULL;
  }
}
/* vi:set ts=8 sts=2 sw=2: */
