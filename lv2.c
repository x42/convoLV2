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

#include "lv2/lv2plug.in/ns/lv2core/lv2.h"
#include "lv2/lv2plug.in/ns/ext/state/state.h"
#include "lv2/lv2plug.in/ns/ext/worker/worker.h"

#include "./uris.h"

#ifndef MAX
#define MAX(a,b) ( ((a)<(b))?(b):(a) )
#endif

/* note: when whaning this, update MAX_CHANNEL_MAPS in convolution.h to (MAX_CHN * MAX_CHN) */
#define MAX_CHN (2)
/* loop MACRO from 0 .. MAX_CHN */
#define LOOP_DEFINE_PORTS(def) \
  def(0) \
  def(1)


#define ENUMPORT(n) \
  P_OUTPUT ## n = (n*2+2), \
  P_INPUT ## n  = (n*2+3),

typedef enum {
  P_CONTROL    = 0,
  P_NOTIFY     = 1,

  LOOP_DEFINE_PORTS(ENUMPORT)
} PortIndex;

enum {
  CMD_APPLY    = 0,
  CMD_FREE     = 1,
};

typedef struct {
  LV2_URID_Map*        map;
  LV2_Worker_Schedule *schedule;

  LV2_Atom_Forge forge;

  float* input[MAX_CHN];
  float* output[MAX_CHN];
  const LV2_Atom_Sequence* control_port;
  LV2_Atom_Sequence*       notify_port;

  LV2_Atom_Forge_Frame notify_frame;

  ConvoLV2URIs uris;

  LV2convolv *clv_online; ///< currently active engine
  LV2convolv *clv_offline; ///< inactive engine being configured
  int bufsize;
  int rate;
  int reinit_in_progress;
  int chn_in;
  int chn_out;

} convoLV2;

static LV2_Handle
instantiate(const LV2_Descriptor*     descriptor,
            double                    rate,
            const char*               bundle_path,
            const LV2_Feature* const* features)
{
  int i;
  convoLV2* self = (convoLV2*)calloc(1, sizeof(convoLV2));
  if(!self) { return NULL ;}

  for (i = 0; features[i]; ++i) {
    if (!strcmp(features[i]->URI, LV2_URID__map)) {
      self->map = (LV2_URID_Map*)features[i]->data;
    } else if (!strcmp(features[i]->URI, LV2_WORKER__schedule)) {
      self->schedule = (LV2_Worker_Schedule*)features[i]->data;
    }
  }

  if (!self->map) {
    fprintf(stderr, "Missing feature uri:map.\n");
    free(self);
    return NULL;
  }

  if (!self->schedule) {
    fprintf(stderr, "Missing feature work:schedule.\n");
    free(self);
    return NULL;
  }

  /* Map URIs and initialise forge */
  map_convolv2_uris(self->map, &self->uris);
  lv2_atom_forge_init(&self->forge, self->map);

  self->bufsize = 1024;
  self->rate = rate;
  self->chn_in = 1;
  self->chn_out = 1;
  self->reinit_in_progress = 0;
  self->clv_online = NULL;
  self->clv_offline = NULL;

  return (LV2_Handle)self;
}


static LV2_Worker_Status
work(LV2_Handle                  instance,
     LV2_Worker_Respond_Function respond,
     LV2_Worker_Respond_Handle   handle,
     uint32_t                    size,
     const void*                 data)
{
  convoLV2* self = (convoLV2*)instance;
  int apply = 0;

  /* prepare new engine instance */
  if (!self->clv_offline) {
    fprintf(stderr, "allocate offline instance\n"); // SAFE DEBUG
    self->clv_offline = clv_alloc();

    if (!self->clv_offline) {
      self->reinit_in_progress = 0;
      return LV2_WORKER_ERR_NO_SPACE; // OOM
    }
    clv_clone_settings(self->clv_offline, self->clv_online);
  }

  if (size == sizeof(int)) {
    switch(*((int*)data)) {
      case CMD_APPLY:
	fprintf(stderr, "apply offline instance\n"); // SAFE DEBUG
	apply = 1;
	break;
      case CMD_FREE:
	fprintf(stderr, "free offline instance\n"); // SAFE DEBUG
	clv_free(self->clv_offline);
	self->clv_offline=NULL;
	break;
    }
  } else {
    /* handle message described in Atom */
    const LV2_Atom_Object* obj = (const LV2_Atom_Object*) data;
    ConvoLV2URIs* uris = &self->uris;

    if (obj->body.otype == uris->patch_Set) {
      const LV2_Atom* file_path = read_set_file(uris, obj);
      if (file_path) {
	const char *fn = (char*)(file_path+1);
	fprintf(stderr, "load IR %s\n", fn); // SAFE DEBUG
	clv_configure(self->clv_offline, "convolution.ir.file", fn);
	apply = 1;
      }
    } else {
      fprintf(stderr, "Unknown message/object type %d\n", obj->body.otype);
    }
  }

  if (apply) {
    if (!clv_initialize(self->clv_offline, self->rate,
	  self->chn_in, self->chn_out,
	  /*64 <= buffer-size <=4096*/ self->bufsize)) /*XXX*/ ; /* < respond anyway */
    respond(handle, 1, ""); // must not be 0, A3 will ignore it.
    //respond(handle, 0, NULL);
  }
  return LV2_WORKER_SUCCESS;
}

static void inform_ui(LV2_Handle instance) {
  convoLV2* self = (convoLV2*)instance;
  // message to UI
  char fn[1024];
  if (clv_query_setting(self->clv_online, "convolution.ir.file", fn, 1024) > 0) {
    lv2_atom_forge_frame_time(&self->forge, 0);
    write_set_file(&self->forge, &self->uris, fn);
  }

  // TODO: notify GUI if convolution is running:
  // clv_is_active(clv->clv_online) == 1 if it is
  // TODO: also send plugin channel-counts and convolution configuration to GUI

#if 0 // DEBUG -- not rt-safe
  char *cfg = clv_dump_settings(self->clv_online);
  if (cfg) {
    lv2_atom_forge_frame_time(&self->forge, 0);
    write_set_file(&self->forge, &self->uris, cfg);
    free(cfg);
  }
#endif
}

static LV2_Worker_Status
work_response(LV2_Handle  instance,
              uint32_t    size,
              const void* data)
{
  // swap engine instances
  convoLV2* self = (convoLV2*)instance;
  LV2convolv *old  = self->clv_online;
  self->clv_online  = self->clv_offline;
  self->clv_offline = old;

  inform_ui(instance);

  int d = CMD_FREE;
  self->schedule->schedule_work(self->schedule->handle, sizeof(int), &d);

  self->reinit_in_progress = 0;
  return LV2_WORKER_SUCCESS;
}

#define IOPORT(i) \
    case P_INPUT ## i: \
      self->input[i] = (float*)data; \
      self->chn_in = MAX(self->chn_in,i+1); \
      break; \
    case P_OUTPUT ## i: \
      self->output[i] = (float*)data; \
      self->chn_out = MAX(self->chn_out,i+1); \
      break;

static void
connect_port(LV2_Handle instance,
             uint32_t   port,
             void*      data)
{
  convoLV2* self = (convoLV2*)instance;

  switch ((PortIndex)port) {
    LOOP_DEFINE_PORTS(IOPORT)
    case P_CONTROL:
      self->control_port = (const LV2_Atom_Sequence*)data;
      break;
    case P_NOTIFY:
      self->notify_port = (LV2_Atom_Sequence*)data;
      break;
  }
}

static void
run(LV2_Handle instance, uint32_t n_samples)
{
  convoLV2* self = (convoLV2*)instance;

  const float *input[MAX_CHN];
  float *output[MAX_CHN];
  int i;

  for (i=0; i < self->chn_in; i++ ) {
    input[i] = self->input[i];
  }
  for (i=0; i < self->chn_out; i++ ) {
    output[i] = self->output[i];
  }

  /* Set up forge to write directly to notify output port. */
  const uint32_t notify_capacity = self->notify_port->atom.size;
  lv2_atom_forge_set_buffer(&self->forge,
			    (uint8_t*)self->notify_port,
			    notify_capacity);

  /* Start a sequence in the notify output port. */
  lv2_atom_forge_sequence_head(&self->forge, &self->notify_frame, 0);

  /* Read incoming events */
  LV2_ATOM_SEQUENCE_FOREACH(self->control_port, ev) {
    const LV2_Atom_Object* obj = (LV2_Atom_Object*)&ev->body;
    ConvoLV2URIs* uris = &self->uris;
    if (obj->body.otype == uris->clv2_uiinit) {
      inform_ui(instance);
    } else {
      self->schedule->schedule_work(self->schedule->handle, lv2_atom_total_size(&ev->body), &ev->body);
    }
  }

  if (self->bufsize != n_samples) {
    // re-initialize convolver with new buffersize
    if (n_samples < 64 || n_samples > 4096 ||
	/* not power of two */ (n_samples & (n_samples - 1))
	) {
      // silence output port ?
      // TODO: notify user (once for each change)
      return;
    }
    if (!self->reinit_in_progress) {
      self->reinit_in_progress = 1;
      self->bufsize = n_samples;
      int d = CMD_APPLY;
      self->schedule->schedule_work(self->schedule->handle, sizeof(int), &d);
    }
  }

  clv_convolve(self->clv_online, input, output,
      self->chn_in,
      self->chn_out,
      n_samples);
}

static void
cleanup(LV2_Handle instance)
{
  convoLV2* self = (convoLV2*)instance;
  clv_free(self->clv_online);
  clv_free(self->clv_offline);
  free(instance);
}

static LV2_State_Status
save(LV2_Handle                instance,
     LV2_State_Store_Function  store,
     LV2_State_Handle          handle,
     uint32_t                  flags,
     const LV2_Feature* const* features)
{
  int i;
  convoLV2* self = (convoLV2*)instance;
  LV2_State_Map_Path* map_path = NULL;

  char *cfg = clv_dump_settings(self->clv_online);
  if (cfg) {
    store(handle, self->uris.clv2_state,
	cfg, strlen(cfg) + 1,
	self->uris.atom_String,
	LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
    free(cfg);
  }

  for (i=0; features[i]; ++i) {
    if (!strcmp(features[i]->URI, LV2_STATE__mapPath)) {
    map_path = (LV2_State_Map_Path*)features[i]->data;
    }
  }

  if (map_path) {
    char fn[1024]; // PATH_MAX
    if (clv_query_setting(self->clv_online, "convolution.ir.file", fn, 1024) > 0 ) {
      char* apath = map_path->abstract_path(map_path->handle, fn);
      store(handle, self->uris.clv2_ir_file,
	  apath, strlen(apath) + 1,
	  self->uris.atom_Path,
	  LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
      free(apath);
    }
  }
  return LV2_STATE_SUCCESS;
}

static LV2_State_Status
restore(LV2_Handle                  instance,
        LV2_State_Retrieve_Function retrieve,
        LV2_State_Handle            handle,
        uint32_t                    flags,
        const LV2_Feature* const*   features)
{
  convoLV2* self = (convoLV2*)instance;
  size_t   size;
  uint32_t type;
  uint32_t valflags;

  /* prepare new engine instance */
  if (!self->clv_offline) {
    fprintf(stderr, "allocate offline instance\n"); // SAFE DEBUG
    self->clv_offline = clv_alloc();

    if (!self->clv_offline) {
      return LV2_STATE_ERR_UNKNOWN; // OOM
    }
  }

  const void* value = retrieve(handle, self->uris.clv2_state, &size, &type, &valflags);

  if (value) {
    const char* cfg = (const char*)value;
    const char *te,*ts = cfg;
    while (ts && *ts && (te=strchr(ts, '\n'))) {
      char *val;
      char kv[1024];
      memcpy(kv, ts, te-ts);
      kv[te-ts]=0;
      fprintf(stderr, "CFG: %s\n", kv); // SAFE DEBUG
      if((val=strchr(kv,'='))) {
	*val=0;
	clv_configure(self->clv_offline, kv, val+1);
      }
      ts=te+1;
    }
  }

  value = retrieve(handle, self->uris.clv2_ir_file, &size, &type, &valflags);

  if (value) {
    const char* path = (const char*)value;
    fprintf(stderr, "PTH: convolution.ir.file=%s\n", path); // SAFE DEBUG
    clv_configure(self->clv_offline, "convolution.ir.file", path);
  }

  self->bufsize = 0; // kick worker thread in next run cb -> notifies UI
  return LV2_STATE_SUCCESS;
}




const void*
extension_data(const char* uri)
{
  static const LV2_Worker_Interface worker = { work, work_response, NULL };
  static const LV2_State_Interface  state  = { save, restore };
  if (!strcmp(uri, LV2_WORKER__interface)) {
    return &worker;
  }
  else if (!strcmp(uri, LV2_STATE__interface)) {
    return &state;
  }
  return NULL;
}

static const LV2_Descriptor descriptor0 = {
  CONVOLV2_URI,
  instantiate,
  connect_port,
  NULL, // activate,
  run,
  NULL, // deactivate,
  cleanup,
  extension_data
};

static const LV2_Descriptor descriptor1 = {
  CONVOLV2_URI "#Stereo",
  instantiate,
  connect_port,
  NULL, // activate,
  run,
  NULL, // deactivate,
  cleanup,
  extension_data
};

static const LV2_Descriptor descriptor2 = {
  CONVOLV2_URI "#MonoToStereo",
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
    return &descriptor0;
  case 1:
    return &descriptor1;
  case 2:
    return &descriptor2;
  default:
    return NULL;
  }
}
/* vi:set ts=8 sts=2 sw=2: */
