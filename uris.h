/* convoLV2 -- LV2 convolution plugin
 *
 * Copyright 2011-2012 David Robillard <d@drobilla.net>
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

#ifndef CLV2_URIS_H
#define CLV2_URIS_H

#ifdef HAVE_LV2_1_18_6
#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/urid/urid.h>
#include <lv2/patch/patch.h>
#else
#include <lv2/lv2plug.in/ns/ext/atom/atom.h>
#include <lv2/lv2plug.in/ns/ext/atom/forge.h>
#include <lv2/lv2plug.in/ns/ext/urid/urid.h>
#include <lv2/lv2plug.in/ns/ext/patch/patch.h>
#endif

#define CONVOLV2_URI "http://gareus.org/oss/lv2/convoLV2"

#define CLV2__impulse CONVOLV2_URI "#impulse"
#define CLV2__load    CONVOLV2_URI "#load"
#define CLV2__state   CONVOLV2_URI "#state"

#ifdef HAVE_LV2_1_8
#define x_forge_object lv2_atom_forge_object
#else
#define x_forge_object lv2_atom_forge_blank
#endif

typedef struct {
	LV2_URID atom_Blank;
	LV2_URID atom_Object;
	LV2_URID atom_Path;
	LV2_URID atom_String;
	LV2_URID atom_URID;
	LV2_URID atom_eventTransfer;
	LV2_URID clv2_impulse;
	LV2_URID clv2_state;
	LV2_URID patch_Get;
	LV2_URID patch_Set;
	LV2_URID patch_property;
	LV2_URID patch_value;
} ConvoLV2URIs;

static inline void
map_convolv2_uris(LV2_URID_Map* map, ConvoLV2URIs* uris)
{
	uris->atom_Blank         = map->map(map->handle, LV2_ATOM__Blank);
	uris->atom_Object        = map->map(map->handle, LV2_ATOM__Object);
	uris->atom_Path          = map->map(map->handle, LV2_ATOM__Path);
	uris->atom_String        = map->map(map->handle, LV2_ATOM__String);
	uris->atom_URID          = map->map(map->handle, LV2_ATOM__URID);
	uris->atom_eventTransfer = map->map(map->handle, LV2_ATOM__eventTransfer);
	uris->clv2_impulse       = map->map(map->handle, CLV2__impulse);
	uris->clv2_state         = map->map(map->handle, CLV2__state);
	uris->patch_Get          = map->map(map->handle, LV2_PATCH__Get);
	uris->patch_Set          = map->map(map->handle, LV2_PATCH__Set);
	uris->patch_property     = map->map(map->handle, LV2_PATCH__property);
	uris->patch_value        = map->map(map->handle, LV2_PATCH__value);
}

/**
 * Write a message like the following to @p forge:
 * []
 *     a patch:Set ;
 *     patch:property convolv2:impulse ;
 *     patch:value </home/me/foo.wav> .
 */
static inline LV2_Atom*
write_set_file(LV2_Atom_Forge*     forge,
               const ConvoLV2URIs* uris,
               const char*         filename)
{
	LV2_Atom_Forge_Frame frame;
	LV2_Atom* set = (LV2_Atom*)x_forge_object(
		forge, &frame, 1, uris->patch_Set);

	lv2_atom_forge_property_head(forge, uris->patch_property, 0);
	lv2_atom_forge_urid(forge, uris->clv2_impulse);
	lv2_atom_forge_property_head(forge, uris->patch_value, 0);
	lv2_atom_forge_path(forge, filename, strlen(filename));

	lv2_atom_forge_pop(forge, &frame);

	return set;
}

/**
 * Get the file path from a message like:
 * []
 *     a patch:Set ;
 *     patch:property convolv2:impulse ;
 *     patch:value </home/me/foo.wav> .
 */
static inline const LV2_Atom*
read_set_file(const ConvoLV2URIs*    uris,
              const LV2_Atom_Object* obj)
{
	if (obj->body.otype != uris->patch_Set) {
		fprintf(stderr, "Ignoring unknown message type %d\n", obj->body.otype);
		return NULL;
	}

	/* Get property URI. */
	const LV2_Atom* property = NULL;
	lv2_atom_object_get(obj, uris->patch_property, &property, 0);
	if (!property) {
		fprintf(stderr, "Malformed set message has no body.\n");
		return NULL;
	} else if (property->type != uris->atom_URID) {
		fprintf(stderr, "Malformed set message has non-URID property.\n");
		return NULL;
	} else if (((LV2_Atom_URID*)property)->body != uris->clv2_impulse) {
		fprintf(stderr, "Set message for unknown property.\n");
		return NULL;
	}

	/* Get value. */
	const LV2_Atom* file_path = NULL;
	lv2_atom_object_get(obj, uris->patch_value, &file_path, 0);
	if (!file_path) {
		fprintf(stderr, "Malformed set message has no value.\n");
		return NULL;
	} else if (file_path->type != uris->atom_Path) {
		fprintf(stderr, "Set message value is not a Path.\n");
		return NULL;
	}

	return file_path;
}

#endif
