#ifndef CONVOLV2_URIS_H
#define CONVOLV2_URIS_H

#include "lv2/lv2plug.in/ns/ext/atom/atom.h"
#include "lv2/lv2plug.in/ns/ext/atom/forge.h"
#include "lv2/lv2plug.in/ns/ext/urid/urid.h"
#include "lv2/lv2plug.in/ns/ext/patch/patch.h"

#define CONVOLV2_URI          "http://gareus.org/oss/lv2/convoLV2"
#define CONVOLV2__file        CONVOLV2_URI "#file"

typedef struct {
	LV2_URID atom_Blank;
	LV2_URID atom_Path;
	LV2_URID atom_Resource;
	LV2_URID atom_Sequence;
	LV2_URID atom_eventTransfer;
	LV2_URID clv_file;
	LV2_URID irfile_load;
} ConvoLV2URIs;

static inline void
map_convolv2_uris(LV2_URID_Map* map, ConvoLV2URIs* uris)
{
	uris->atom_Blank         = map->map(map->handle, LV2_ATOM__Blank);
	uris->atom_Path          = map->map(map->handle, LV2_ATOM__Path);
	uris->atom_Resource      = map->map(map->handle, LV2_ATOM__Resource);
	uris->atom_Sequence      = map->map(map->handle, LV2_ATOM__Sequence);
	uris->atom_eventTransfer = map->map(map->handle, LV2_ATOM__eventTransfer);

	uris->clv_file           = map->map(map->handle, CONVOLV2__file);
	uris->irfile_load        = map->map(map->handle, LV2_PATCH__Set);
}


static inline bool
is_object_type(const ConvoLV2URIs* uris, LV2_URID type)
{
	return type == uris->atom_Resource
		|| type == uris->atom_Blank;
}

/**
 * Write a message like the following to @p forge:
 * [
 *     a patch:Set ;
 *     patch:body [
 *         eg-sampler:file </home/me/foo.wav> ;
 *     ] ;
 * ]
 */
static inline LV2_Atom*
write_set_file(LV2_Atom_Forge*    forge,
               const ConvoLV2URIs* uris,
               const char*        filename)
{
	LV2_Atom_Forge_Frame set_frame;
	LV2_Atom* set = (LV2_Atom*)lv2_atom_forge_blank(
		forge, &set_frame, 1, uris->irfile_load);
	lv2_atom_forge_property_head(forge, uris->clv_file, 0);
	lv2_atom_forge_path(forge, filename, strlen(filename));
	lv2_atom_forge_pop(forge, &set_frame);

	return set;
}

/**
 * Get the file path from a message like:
 * [
 *     a patch:Set ;
 *     patch:body [
 *         eg-sampler:file </home/me/foo.wav> ;
 *     ] ;
 * ]
 */
static inline const LV2_Atom*
read_set_file(const ConvoLV2URIs*     uris,
              const LV2_Atom_Object* obj)
{
	if (obj->body.otype != uris->irfile_load) {
		fprintf(stderr, "Ignoring unknown message type %d\n", obj->body.otype);
		return NULL;
	}
	const LV2_Atom* file_path = NULL;
	lv2_atom_object_get(obj, uris->clv_file, &file_path, 0);
	if (!file_path) {
		fprintf(stderr, "Ignored set message with no file path.\n");
		return NULL;
	}

	return file_path;
}

#endif
