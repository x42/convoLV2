/* convoLV2 -- LV2 convolution plugin
 *
 * Copyright (C) 2012-2016 Robin Gareus <robin@gareus.org>
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

#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "convolution.h"

#include "lv2/lv2plug.in/ns/ext/buf-size/buf-size.h"
#include "lv2/lv2plug.in/ns/ext/log/log.h"
#include "lv2/lv2plug.in/ns/ext/log/logger.h"
#include "lv2/lv2plug.in/ns/ext/options/options.h"
#include "lv2/lv2plug.in/ns/ext/state/state.h"
#include "lv2/lv2plug.in/ns/ext/worker/worker.h"
#include "lv2/lv2plug.in/ns/lv2core/lv2.h"

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
  P_OUTPUT ## n = (n*2+3), \
  P_INPUT ## n  = (n*2+4),

typedef enum {
  P_CONTROL    = 0,
  P_NOTIFY     = 1,
  P_OUTGAIN    = 2,

  LOOP_DEFINE_PORTS(ENUMPORT)
} PortIndex;

enum {
  CMD_APPLY    = 0,
  CMD_FREE     = 1,
};

typedef struct {
  LV2_URID_Map*        map;
  LV2_Worker_Schedule* schedule;
  LV2_Log_Log*         log;

  LV2_Atom_Forge forge;
  LV2_Log_Logger logger;

  float* input[MAX_CHN];
  float* output[MAX_CHN];
  const LV2_Atom_Sequence* control_port;
  LV2_Atom_Sequence*       notify_port;

  float * p_output_gain;
  float output_gain_db, output_gain_target, output_gain;

  LV2_Atom_Forge_Frame notify_frame;

  ConvoLV2URIs uris;

  LV2convolv *clv_online; ///< currently active engine
  LV2convolv *clv_offline; ///< inactive engine being configured

  int rate; ///< sample-rate -- constant per instance
  int chn_in; ///< input channel count -- constant per instance
  int chn_out; ///< output channel count --constant per instance

  unsigned int bufsize;

  short flag_reinit_in_progress;
  short flag_notify_ui; ///< notify UI about setting on next run()

} convoLV2;

static LV2_Handle
instantiate(const LV2_Descriptor*     descriptor,
            double                    rate,
            const char*               bundle_path,
            const LV2_Feature* const* features)
{
  const LV2_Options_Option* options  = NULL;
  LV2_URID_Map*             map      = NULL;
  LV2_Worker_Schedule*      schedule = NULL;
  LV2_Log_Log*              log      = NULL;
  for (int i = 0; features[i]; ++i) {
    if (!strcmp(features[i]->URI, LV2_URID__map)) {
      map = (LV2_URID_Map*)features[i]->data;
    } else if (!strcmp(features[i]->URI, LV2_WORKER__schedule)) {
      schedule = (LV2_Worker_Schedule*)features[i]->data;
    } else if (!strcmp(features[i]->URI, LV2_OPTIONS__options)) {
      options = (const LV2_Options_Option*)features[i]->data;
    } else if (!strcmp(features[i]->URI, LV2_LOG__log)) {
      log = (LV2_Log_Log*)features[i]->data;
    }
  }

  // Initialise logger (if map is unavailable, will fallback to printf)
  LV2_Log_Logger logger;
  lv2_log_logger_init(&logger, map, log);

  if (!map) {
    lv2_log_error(&logger, "Missing feature uri:map\n");
    return NULL;
  } else if (!schedule) {
    lv2_log_error(&logger, "Missing feature work:schedule\n");
    return NULL;
  } else if (!options) {
    lv2_log_error(&logger, "Missing options\n");
    return NULL;
  }

  LV2_URID bufsz_max = map->map(map->handle, LV2_BUF_SIZE__maxBlockLength);
  LV2_URID atom_Int  = map->map(map->handle, LV2_ATOM__Int);
  uint32_t bufsize   = 0;
  for (const LV2_Options_Option* o = options; o->key; ++o) {
    if (o->context == LV2_OPTIONS_INSTANCE &&
        o->key == bufsz_max &&
        o->type == atom_Int) {
      bufsize = *(const int32_t*)o->value;
    }
  }

  if (bufsize == 0) {
    lv2_log_error(&logger, "No maximum buffer size given\n");
    return NULL;
  } else if (bufsize < 64 || bufsize > 8192) {
    lv2_log_error(&logger, "Buffer size %u out of range 64..8192\n", bufsize);
    return NULL;
  } else if (bufsize & (bufsize - 1)) {
    lv2_log_error(&logger, "Buffer size %u not a power of two\n", bufsize);
    return NULL;
  }

  lv2_log_trace(&logger, "Buffer size: %u\n", bufsize);

  convoLV2* self = (convoLV2*)calloc(1, sizeof(convoLV2));
  if (!self) {
    return NULL;
  }

  /* Map URIs and initialise forge */
  map_convolv2_uris(map, &self->uris);
  lv2_atom_forge_init(&self->forge, map);

  self->map = map;
  self->schedule = schedule;
  self->log = log;
  self->logger = logger;
  self->bufsize = bufsize;
  self->rate = rate;
  self->chn_in = 1;
  self->chn_out = 1;
  self->flag_reinit_in_progress = 0;
  self->clv_online = NULL;
  self->clv_offline = NULL;

  self->output_gain_db = 0;
  self->output_gain_target = self->output_gain = 1.0;

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
    DEBUG_printf("Work: allocate offline instance\n");
    self->clv_offline = clv_alloc();

    if (!self->clv_offline) {
      self->flag_reinit_in_progress = 0;
      return LV2_WORKER_ERR_NO_SPACE; // OOM
    }
    clv_clone_settings(self->clv_offline, self->clv_online);
  }

  if (size == sizeof(int)) {
    switch(*((const int*)data)) {
    case CMD_APPLY:
      DEBUG_printf("Work: apply offline instance\n");
      apply = 1;
      break;
    case CMD_FREE:
      DEBUG_printf("Work: free offline instance\n");
      clv_free(self->clv_offline);
      self->clv_offline=NULL;
      break;
    default:
      DEBUG_printf("Work: invalid command\n");
      break;
    }
  } else {
    /* handle message described in Atom */
    const LV2_Atom_Object* obj = (const LV2_Atom_Object*) data;
    ConvoLV2URIs* uris = &self->uris;

    if (obj->body.otype == uris->patch_Set) {
      DEBUG_printf("Work: Atom Patch\n");
      const LV2_Atom* file_path = read_set_file(uris, obj);
      if (file_path) {
        const char *fn = (const char*)(file_path+1);
        DEBUG_printf("load IR %s\n", fn);
        clv_configure(self->clv_offline, "convolution.ir.file", fn);
        apply = 1;
      }
    } else {
      DEBUG_printf("Work: Invalid Atom Msg\n");
    }
  }

  if (apply) {
    DEBUG_printf("Work: initialize offline instance\n");
    clv_initialize(self->clv_offline, self->rate,
                   self->chn_in, self->chn_out,
                   /*64 <= buffer-size <=4096*/ self->bufsize);
#if 1
    respond(handle, 1, ""); // size must not be 0. A3 before rev 13146 will ignore it
#else
    respond(handle, 0, NULL);
#endif
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
  convoLV2* self = (convoLV2*)instance;

  if (!self->clv_offline) {
    return LV2_WORKER_SUCCESS;
  }

  // swap engine instances
  DEBUG_printf("Work: swap instances\n");
  LV2convolv *old  = self->clv_online;
  self->clv_online  = self->clv_offline;
  self->clv_offline = old;

  inform_ui(instance);

  int d = CMD_FREE;
  self->schedule->schedule_work(self->schedule->handle, sizeof(int), &d);

  self->flag_reinit_in_progress = 0;
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
    case P_OUTGAIN:
      self->p_output_gain = (float*)data;
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

  if (*self->p_output_gain != self->output_gain_db) {
    float g = *self->p_output_gain;
    self->output_gain_db = *self->p_output_gain;
    if (g < -40) g = -40;
    if (g >  40) g =  40;
    self->output_gain_target = powf(10.f, 0.05f * g);
  }

  self->output_gain += .08f * (self->output_gain_target - self->output_gain);

  /* Set up forge to write directly to notify output port. */
  if (self->notify_port) {
    const uint32_t notify_capacity = self->notify_port->atom.size;
    lv2_atom_forge_set_buffer(&self->forge,
                              (uint8_t*)self->notify_port,
                              notify_capacity);

    /* Start a sequence in the notify output port. */
    lv2_atom_forge_sequence_head(&self->forge, &self->notify_frame, 0);
  }

  bool silent = false;

  /* re-init engine if block-size has changed */
  if (self->bufsize != n_samples) {
    /* verify if we support the new block-size */
    if (n_samples < 64 || n_samples > 8192 ||
        /* not power of two */ (n_samples & (n_samples - 1))
        ) {
      /* silence output ports */
      for (i=0; i < self->chn_out; i++ ) {
        memset(output[i], 0, sizeof(float) * n_samples);
      }
      silent = true;
    } else {
      if (!self->flag_reinit_in_progress && clv_is_active(self->clv_online)) {
	self->flag_reinit_in_progress = 1;
	self->bufsize = n_samples;
	int d = CMD_APPLY;
	self->schedule->schedule_work(self->schedule->handle, sizeof(int), &d);
      }
    }
  }

  /* don't touch any settings if re-init is scheduled or in progress
   * TODO re-queue them ?
   */
  if (!self->flag_reinit_in_progress && self->control_port && self->notify_port) {
    /* Read incoming events */
    LV2_ATOM_SEQUENCE_FOREACH(self->control_port, ev) {
      const LV2_Atom_Object* obj = (LV2_Atom_Object*)&ev->body;
      ConvoLV2URIs* uris = &self->uris;
      if (obj->body.otype == uris->patch_Get) {
        self->flag_notify_ui = 0;
        inform_ui(instance);
      } else {
        // TODO: parse message here and set self->flag_reinit_in_progres=1; IFF an apply would be triggered
        self->schedule->schedule_work(self->schedule->handle, lv2_atom_total_size(&ev->body), &ev->body);
      }
    }
  }

  /* send current setting to UI */
  if (self->flag_notify_ui && self->notify_port) {
    self->flag_notify_ui = 0;
    inform_ui(instance);
  }

  if (silent) {
    return;
  }
  clv_convolve(self->clv_online, input, output,
               self->chn_in,
               self->chn_out,
               n_samples, self->output_gain);
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
#ifdef HAVE_LV2_FREE_PATH
  LV2_State_Free_Path* free_path = NULL;
#endif

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
#ifdef HAVE_LV2_FREE_PATH
    if (!strcmp(features[i]->URI, LV2_STATE__freePath)) {
      free_path = (LV2_State_Free_Path*)features[i]->data;
    }
#endif
  }

  if (!map_path) {
    return LV2_STATE_ERR_NO_FEATURE;
  } else {
    char fn[1024]; // PATH_MAX
    if (clv_query_setting(self->clv_online, "convolution.ir.file", fn, 1024) > 0 ) {
      char* apath = map_path->abstract_path(map_path->handle, fn);
      store(handle, self->uris.clv2_impulse,
            apath, strlen(apath) + 1,
            self->uris.atom_Path,
            LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);

#ifdef HAVE_LV2_FREE_PATH
      if (free_path) {
	free_path->free_path (free_path->handle, apath);
      } else {
#elif !defined _WIN32
	free(apath);
#endif
      }
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

  /* Get the work scheduler provided to restore() (state:threadSafeRestore
     support), but fall back to instantiate() schedules (spec-violating
     workaround for broken hosts). */
  LV2_Worker_Schedule* schedule = self->schedule;
  LV2_State_Map_Path* map_path = NULL;
  for (int i = 0; features[i]; ++i) {
    if (!strcmp(features[i]->URI, LV2_WORKER__schedule)) {
      DEBUG_printf("State: using thread-safe restore scheduler\n");
      schedule = (LV2_Worker_Schedule*)features[i]->data;
    }
    if (!strcmp(features[i]->URI, LV2_STATE__mapPath)) {
      map_path = (LV2_State_Map_Path*)features[i]->data;
    }
  }
  if (!map_path) {
    return LV2_STATE_ERR_NO_FEATURE;
  }
  if (schedule == self->schedule) {
    DEBUG_printf("State: warning: using run() scheduler to restore\n");
  }

  if (self->clv_offline) {
    // TODO use lv2_log_error()
    VERBOSE_printf("State: offline instance in-use, state ignored.\n");
    return LV2_STATE_ERR_UNKNOWN; // OOM
  }

  /* prepare new engine instance */
  if (!self->clv_offline) {
    DEBUG_printf("State: allocate offline instance\n");
    self->clv_offline = clv_alloc();

    if (!self->clv_offline) {
      return LV2_STATE_ERR_UNKNOWN; // OOM
    }
  }

#if 1 // prevent concurrent run() mess
  // should use atomic -- but really hosts need to be fixed instead
  self->flag_reinit_in_progress = 1;
#endif

  const void* value = retrieve(handle, self->uris.clv2_state, &size, &type, &valflags);

  bool ok = true;
  if (value) {
    const char* cfg = (const char*)value;
    const char *te,*ts = cfg;
    while (ts && *ts && (te=strchr(ts, '\n'))) {
      char *val;
      char kv[1024];
      memcpy(kv, ts, te-ts);
      kv[te-ts]=0;
      DEBUG_printf("CFG: %s\n", kv);
      if((val=strchr(kv,'='))) {
        *val=0;
        clv_configure(self->clv_offline, kv, val+1);
      }
      ts=te+1;
    }
  } else {
    ok = false;
  }

  value = retrieve(handle, self->uris.clv2_impulse, &size, &type, &valflags);


  if (value) {
    char* path = map_path->absolute_path (map_path->handle, (const char*) value);
    DEBUG_printf("PTH: convolution.ir.file=%s\n", path);
    clv_configure(self->clv_offline, "convolution.ir.file", path);
#ifndef _WIN32 // https://github.com/drobilla/lilv/issues/14
    free (path);
#endif
  } else  {
    ok = false;
  }

  if (!ok) {
    DEBUG_printf("State: incomplete state. Free offline instance\n");
    clv_free(self->clv_offline);
    self->clv_offline = 0;
    self->flag_reinit_in_progress = 0;
    return LV2_STATE_ERR_NO_PROPERTY;
  }

#if 0
  /* run() and restore() must not be called concurrently.
   * except some hosts do..
   *
   * in any case re-initialize in a background thread to not
   * block run() for a long time.
   */
  clv_initialize(self->clv_offline, self->rate, self->chn_in, self->chn_out,
                 /*64 <= buffer-size <=4096*/ self->bufsize);

  DEBUG_printf("State: applied offline instance\n");
  LV2convolv *old   = self->clv_online;
  self->clv_online  = self->clv_offline;
  self->clv_offline = old;

  self->flag_reinit_in_progress = 0;
  self->flag_notify_ui = 1;

  DEBUG_printf("State: free offline instance\n");
  clv_free(self->clv_offline);
  self->clv_offline=NULL;
#else
  int d = CMD_APPLY;
  schedule->schedule_work(self->schedule->handle, sizeof(int), &d);
#endif
  return LV2_STATE_SUCCESS;
}

static const void*
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
  CONVOLV2_URI "#Mono",
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


#undef LV2_SYMBOL_EXPORT
#ifdef _WIN32
#    define LV2_SYMBOL_EXPORT __declspec(dllexport)
#else
#    define LV2_SYMBOL_EXPORT  __attribute__ ((visibility ("default")))
#endif
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
