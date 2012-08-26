/* setBfree - DSP tonewheel organ
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

#ifndef CONVOLUTION_H
#define CONVOLUTION_H


#define MAX_OUTPUT_CHANNELS (4)

/* zita-convolver lib is C++ so we need extern "C" in order to link
 * functions using it. */
#ifdef __cplusplus
extern "C" {
#endif

typedef struct LV2convolv LV2convolv;

extern LV2convolv *clv_alloc();
extern void clv_free (LV2convolv *clv);

int clv_configure (LV2convolv *clv, const char *key, const char *value);
extern int clv_initialize (LV2convolv *clv, const unsigned int sample_rate, const unsigned int in_channel_cnt, const unsigned int out_channel_cnt, const unsigned int buffersize);
extern void clv_release (LV2convolv *clv);
void clv_clone_settings(LV2convolv *clv_new, LV2convolv *clv);

extern int clv_convolve (LV2convolv *clv, const float * const * inbuf, float * const* outbuf, const unsigned int n_channels, const unsigned int n_samples);

int clv_query_setting (LV2convolv *clv, const char *key, char *value, size_t val_max_len);
char *clv_dump_settings (LV2convolv *clv);

#ifdef __cplusplus
}
#endif

#endif
