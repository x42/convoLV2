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

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "convolution.h"

#include "lv2/lv2plug.in/ns/lv2core/lv2.h"

typedef enum {
  P_INPUT      = 0,
  P_OUTPUT     = 1,
} PortIndex;

typedef struct {
  float* input;
  float* output;

  struct LV2convolv *instance;
} convoLV2;

static LV2_Handle
instantiate(const LV2_Descriptor*     descriptor,
            double                    rate,
            const char*               bundle_path,
            const LV2_Feature* const* features)
{
  int sched_pol= 0;
  int sched_pri =0;

  convoLV2* clv = (convoLV2*)calloc(1, sizeof(convoLV2));
  if(!clv) { return NULL ;}

  if (!(clv->instance = allocConvolution())) {
    free(clv);
    return NULL;
  }

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

#if 0
  /* scheduler priority of zita-convolution thread */
  struct sched_param s_param;
  pthread_getschedparam(jack_client_thread_id(j_client), &s_policy, &s_param); // XXX TODO get LV2 thread priority
  sched_pri = s_param.sched_priority;
#endif

  if (initConvolution(clv->instance, rate,
	/*num channels*/ 1,
	/*64 <= buffer-size <=4096*/ 1024,
	sched_pri, sched_pol))
  {
    freeConvolution(clv->instance);
    free(clv);
    return NULL;
  }

  return (LV2_Handle)clv;
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
  }
}

static void
activate(LV2_Handle instance)
{
}

static void
run(LV2_Handle instance, uint32_t n_samples)
{
  convoLV2* clv = (convoLV2*)instance;

  const float *input[MAX_AUDIO_CHANNELS];
  float *output[MAX_AUDIO_CHANNELS];
  input[0] = clv->input;
  output[0] = clv->output;

  convolve(clv->instance, input, output, /*num channels*/1, n_samples);
}

static void
deactivate(LV2_Handle instance)
{
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
  return NULL;
}

static const LV2_Descriptor descriptor = {
  "http://gareus.org/oss/lv2/convoLV2",
  instantiate,
  connect_port,
  activate,
  run,
  deactivate,
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
