/*
 * libdivecomputer
 *
 * Copyright (C) 2014 Linus Torvalds
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301 USA
 */

#include <stdio.h>		// snprintf
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#include "suunto_eonsteel.h"
#include "context-private.h"
#include "parser-private.h"
#include "array.h"
#include "platform.h"
#include "field-cache.h"

enum eon_sample {
	ES_none = 0,
	ES_dtime,		// duint16,precision=3 (time delta in ms)
	ES_depth,		// uint16,precision=2,nillable=65535 (depth in cm)
	ES_temp,		// int16,precision=2,nillable=-3000 (temp in deci-Celsius)
	ES_ndl,			// int16,nillable=-1 (ndl in minutes)
	ES_ceiling,		// uint16,precision=2,nillable=65535 (ceiling in cm)
	ES_tts,			// uint16,nillable=65535 (time to surface)
	ES_heading,		// uint16,precision=4,nillable=65535 (heading in degrees)
	ES_abspressure,		// uint16,precision=0,nillable=65535 (abs presure in centibar)
	ES_gastime,		// int16,nillable=-1 (remaining gas time in minutes)
	ES_ventilation,		// uint16,precision=6,nillable=65535 ("x/6000000,x"? No idea)
	ES_gasnr,		// uint8
	ES_pressure,		// uint16,nillable=65535 (cylinder pressure in centibar)
	ES_state,		// enum:0=Wet Outside,1=Below Wet Activation Depth,2=Below Surface,3=Dive Active,4=Surface Calculation,5=Tank pressure available,6=Closed Circuit Mode
	ES_state_active,	// bool
	ES_notify,		// enum:0=NoFly Time,1=Depth,2=Surface Time,3=Tissue Level,4=Deco,5=Deco Window,6=Safety Stop Ahead,7=Safety Stop,8=Safety Stop Broken,9=Deep Stop Ahead,10=Deep Stop,11=Dive Time,12=Gas Available,13=SetPoint Switch,14=Diluent Hypoxia,15=Air Time,16=Tank Pressure
	ES_notify_active,	// bool
	ES_warning,		// enum:0=ICD Penalty,1=Deep Stop Penalty,2=Mandatory Safety Stop,3=OTU250,4=OTU300,5=CNS80%,6=CNS100%,7=Max.Depth,8=Air Time,9=Tank Pressure,10=Safety Stop Broken,11=Deep Stop Broken,12=Ceiling Broken,13=PO2 High
	ES_warning_active,	// bool
	ES_alarm,
	ES_alarm_active,
	ES_gasswitch,		// uint16
	ES_setpoint_type,	// enum:0=Low,1=High,2=Custom
	ES_setpoint_po2,	// uint32
	ES_setpoint_automatic,	// bool
	ES_bookmark,
	ES_insertgas,		// uint16
	ES_removegas,		// uint16
};

#define EON_MAX_GROUP 16

struct type_desc {
	char *desc, *format, *mod;
	unsigned int size;
	enum eon_sample type[EON_MAX_GROUP];
};

#define MAXTYPE 512

typedef struct suunto_eonsteel_parser_t {
	dc_parser_t base;
	struct type_desc type_desc[MAXTYPE];
	struct dc_field_cache cache;
} suunto_eonsteel_parser_t;

typedef int (*eon_data_cb_t)(unsigned short type, const struct type_desc *desc, const unsigned char *data, unsigned int len, void *user);

static const struct {
	const char *name;
	enum eon_sample type;
} type_translation[] = {
	{ "+Time",				ES_dtime },
	{ "Depth",				ES_depth },
	{ "Temperature",			ES_temp },
	{ "NoDecTime",				ES_ndl },
	{ "Ceiling",				ES_ceiling },
	{ "TimeToSurface",			ES_tts },
	{ "Heading",				ES_heading },
	{ "DeviceInternalAbsPressure",		ES_abspressure },
	{ "GasTime",				ES_gastime },
	{ "Ventilation",			ES_ventilation },
	{ "Cylinders+Cylinder.GasNumber",	ES_gasnr },
	{ "Cylinders.Cylinder.Pressure",	ES_pressure },
	{ "Events+State.Type",			ES_state },
	{ "Events.State.Active",		ES_state_active },
	{ "Events+Notify.Type",			ES_notify },
	{ "Events.Notify.Active",		ES_notify_active },
	{ "Events+Warning.Type",		ES_warning },
	{ "Events.Warning.Active",		ES_warning_active },
	{ "Events+Alarm.Type",			ES_alarm },
	{ "Events.Alarm.Active",		ES_alarm_active },
	{ "Events.Bookmark.Name",		ES_bookmark },
	{ "Events.GasSwitch.GasNumber",		ES_gasswitch },
	{ "Events.SetPoint.Type",		ES_setpoint_type },
	{ "Events.Events.SetPoint.PO2",		ES_setpoint_po2 },
	{ "Events.SetPoint.Automatic",		ES_setpoint_automatic },
	{ "Events.DiveTimer.Active",		ES_none },
	{ "Events.DiveTimer.Time",		ES_none },
	{ "Events+GasSwitch.GasNumber",		ES_gasswitch },
	{ "Events+GasEdit.InsertGasNumber",	ES_insertgas },
	{ "Events+GasEdit.RemoveGasNumber",	ES_removegas },
};

static enum eon_sample lookup_descriptor_type(suunto_eonsteel_parser_t *eon, struct type_desc *desc)
{
	const char *name = desc->desc;

	// Not a sample type? Skip it
	if (strncmp(name, "sml.DeviceLog.Samples", 21))
		return ES_none;

	// Skip the common base
	name += 21;

	// We have a "+Sample.Time", which starts a new
	// sample and contains the time delta
	if (!strcmp(name, "+Sample.Time"))
		return ES_dtime;

	// .. the rest should start with ".Sample."
	if (strncmp(name, ".Sample.", 8))
		return ES_none;

	// Skip the ".Sample."
	name += 8;

	// .. and look it up in the table of sample type strings
	for (size_t i = 0; i < C_ARRAY_SIZE(type_translation); i++) {
		if (!strcmp(name, type_translation[i].name))
			return type_translation[i].type;
	}
	return ES_none;
}

static const char *desc_type_name(enum eon_sample type)
{
	for (size_t i = 0; i < C_ARRAY_SIZE(type_translation); i++) {
		if (type == type_translation[i].type)
			return type_translation[i].name;
	}
	return "Unknown";
}

static int lookup_descriptor_size(suunto_eonsteel_parser_t *eon, struct type_desc *desc)
{
	const char *format = desc->format;
	unsigned char c;

	if (!format)
		return 0;

	if (!strncmp(format, "bool", 4))
		return 1;
	if (!strncmp(format, "enum", 4))
		return 1;
	if (!strncmp(format, "utf8", 4))
		return 0;

	// find the byte size (eg "float32" -> 4 bytes)
	while ((c = *format) != 0) {
		if (isdigit(c))
			return atoi(format)/8;
		format++;
	}
	return 0;
}

static int fill_in_group_details(suunto_eonsteel_parser_t *eon, struct type_desc *desc)
{
	int subtype = 0;
	const char *grp = desc->desc;

	for (;;) {
		struct type_desc *base;
		char *end;
		long index;

		index = strtol(grp, &end, 10);
		if (index < 0 || index >= MAXTYPE || end == grp) {
			ERROR(eon->base.context, "Group type descriptor '%s' does not parse", desc->desc);
			break;
		}
		base = eon->type_desc + index;
		if (!base->desc) {
			ERROR(eon->base.context, "Group type descriptor '%s' has undescribed index %ld", desc->desc, index);
			break;
		}
		if (!base->size) {
			ERROR(eon->base.context, "Group type descriptor '%s' uses unsized sub-entry '%s'", desc->desc, base->desc);
			break;
		}
		if (!base->type[0]) {
			ERROR(eon->base.context, "Group type descriptor '%s' has non-enumerated sub-entry '%s'", desc->desc, base->desc);
			break;
		}
		if (base->type[1]) {
			ERROR(eon->base.context, "Group type descriptor '%s' has a recursive group sub-entry '%s'", desc->desc, base->desc);
			break;
		}
		if (subtype >= EON_MAX_GROUP-1) {
			ERROR(eon->base.context, "Group type descriptor '%s' has too many sub-entries", desc->desc);
			break;
		}
		desc->size += base->size;
		desc->type[subtype++] = base->type[0];
		switch (*end) {
		case 0:
			return 0;
		case ',':
			grp = end+1;
			continue;
		default:
			ERROR(eon->base.context, "Group type descriptor '%s' has unparseable index %ld", desc->desc, index);
			return -1;
		}
	}
	return -1;
}

/*
 * Here we cache descriptor data so that we don't have
 * to re-parse the string all the time. That way we can
 * do it just once per type.
 *
 * Right now we only bother with the sample descriptors,
 * which all start with "sml.DeviceLog.Samples" (for the
 * base types) or are "GRP" types that are a group of said
 * types and are a set of numbers.
 */
static int fill_in_desc_details(suunto_eonsteel_parser_t *eon, struct type_desc *desc)
{
	if (!desc->desc)
		return 0;

	if (isdigit(desc->desc[0]))
		return fill_in_group_details(eon, desc);

	desc->size = lookup_descriptor_size(eon, desc);
	desc->type[0] = lookup_descriptor_type(eon, desc);
	return 0;
}

static void
desc_free (struct type_desc desc[], unsigned int count)
{
	for (unsigned int i = 0; i < count; ++i) {
		free(desc[i].desc);
		free(desc[i].format);
		free(desc[i].mod);
	}
}

static int record_type(suunto_eonsteel_parser_t *eon, unsigned short type, const char *name, int namelen)
{
	struct type_desc desc;
	const char *next;

	memset(&desc, 0, sizeof(desc));
	do {
		int len;
		char *p;

		next = strchr(name, '\n');
		if (next) {
			len = next - name;
			next++;
		} else {
			len = strlen(name);
			if (!len)
				break;
		}

		if (len < 5 || name[0] != '<' || name[4] != '>') {
			ERROR(eon->base.context, "Unexpected type description: %.*s", len, name);
			return -1;
		}
		p = (char *) malloc(len-4);
		if (!p) {
			ERROR(eon->base.context, "out of memory");
			desc_free(&desc, 1);
			return -1;
		}
		memcpy(p, name+5, len-5);
		p[len-5] = 0;

		// PTH, GRP, FRM, MOD
		switch (name[1]) {
		case 'P':
		case 'G':
			desc.desc = p;
			break;
		case 'F':
			desc.format = p;
			break;
		case 'M':
			desc.mod = p;
			break;
		default:
			ERROR(eon->base.context, "Unknown type descriptor: %.*s", len, name);
			desc_free(&desc, 1);
			free(p);
			return -1;
		}
	} while ((name = next) != NULL);

	if (type >= MAXTYPE) {
		ERROR(eon->base.context, "Type out of range (%04x: '%s' '%s' '%s')",
			type,
			desc.desc ? desc.desc : "",
			desc.format ? desc.format : "",
			desc.mod ? desc.mod : "");
		desc_free(&desc, 1);
		return -1;
	}

	fill_in_desc_details(eon, &desc);

	desc_free(eon->type_desc + type, 1);
	eon->type_desc[type] = desc;
	return 0;
}

static int traverse_entry(suunto_eonsteel_parser_t *eon, const unsigned char *p, int size, eon_data_cb_t callback, void *user)
{
	const unsigned char *name, *data, *end, *last, *one_past_end = p + size;
	int textlen, id;
	int rc;

	// First two bytes: zero and text length
	if (p[0]) {
		HEXDUMP(eon->base.context, DC_LOGLEVEL_DEBUG, "next", p, 8);
		ERROR(eon->base.context, "Bad dive entry (%02x)", p[0]);
		return -1;
	}
	textlen = p[1];

	name = p + 2;
	if (textlen == 0xff) {
		textlen = array_uint32_le(name);
		name += 4;
	}

	// Two bytes of 'type' followed by the name/descriptor, followed by the data
	data = name + textlen;
	id = array_uint16_le(name);
	name += 2;

	if (*name != '<') {
		HEXDUMP(eon->base.context, DC_LOGLEVEL_DEBUG, "bad", p, 16);
		return -1;
	}

	record_type(eon, id, (const char *) name, textlen-3);

	end = data;
	last = data;
	while (end < one_past_end && *end) {
		const unsigned char *begin = end;
		unsigned int type = *end++;
		unsigned int len;
		if (type == 0xff) {
			type = array_uint16_le(end);
			end += 2;
		}
		len = *end++;

		// I've never actually seen this case yet..
		// Just assuming from the other cases.
		if (len == 0xff) {
			HEXDUMP(eon->base.context, DC_LOGLEVEL_DEBUG, "len-ff", end, 8);
			len = array_uint32_le(end);
			end += 4;
		}

		if (type >= MAXTYPE || !eon->type_desc[type].desc) {
			HEXDUMP(eon->base.context, DC_LOGLEVEL_DEBUG, "last", last, 16);
			HEXDUMP(eon->base.context, DC_LOGLEVEL_DEBUG, "this", begin, 16);
		} else {
			rc = callback(type, eon->type_desc+type, end, len, user);
			if (rc < 0)
				return rc;
		}

		last = begin;
		end += len;
	}

	return end - p;
}

static int traverse_data(suunto_eonsteel_parser_t *eon, eon_data_cb_t callback, void *user)
{
	const unsigned char *data = eon->base.data;
	int len = eon->base.size;

	// Dive files start with "SBEM" and four NUL characters
	// Additionally, we've prepended the time as an extra
	// 4-byte pre-header
	if (len < 12 || memcmp(data+4, "SBEM", 4))
		return 0;

	data += 12;
	len -= 12;

	while (len > 4) {
		int i = traverse_entry(eon, data, len, callback, user);
		if (i < 0)
			return 1;
		len -= i;
		data += i;
	}
	return 0;
}

struct sample_data {
	suunto_eonsteel_parser_t *eon;
	dc_sample_callback_t callback;
	void *userdata;
	unsigned int time;
	char *state_type, *notify_type;
	char *warning_type, *alarm_type;

	/* We gather up deco and cylinder pressure information */
	int gasnr;
};

static void sample_time(struct sample_data *info, unsigned short time_delta)
{
	dc_sample_value_t sample = {0};

	info->time += time_delta;
	sample.time = info->time;
	if (info->callback) info->callback(DC_SAMPLE_TIME, &sample, info->userdata);
}

static void sample_depth(struct sample_data *info, unsigned short depth)
{
	dc_sample_value_t sample = {0};

	if (depth == 0xffff)
		return;

	sample.depth = depth / 100.0;
	if (info->callback) info->callback(DC_SAMPLE_DEPTH, &sample, info->userdata);
}

static void sample_temp(struct sample_data *info, short temp)
{
	dc_sample_value_t sample = {0};

	if (temp <= -3000)
		return;

	sample.temperature = temp / 10.0;
	if (info->callback) info->callback(DC_SAMPLE_TEMPERATURE, &sample, info->userdata);
}

static void sample_ndl(struct sample_data *info, short ndl)
{
	dc_sample_value_t sample = {0};

	if (ndl < 0)
		return;

	sample.deco.type = DC_DECO_NDL;
	sample.deco.time = ndl;
	sample.deco.tts = 0;
	if (info->callback) info->callback(DC_SAMPLE_DECO, &sample, info->userdata);
}

static void sample_tts(struct sample_data *info, unsigned short tts)
{
	if (tts != 0xffff) {
		dc_sample_value_t sample = {0};
		sample.time = tts;
		if (info->callback) info->callback(DC_SAMPLE_TTS, &sample, info->userdata);
	}
}

static void sample_ceiling(struct sample_data *info, unsigned short ceiling)
{
	if (ceiling != 0xffff) {
		dc_sample_value_t sample = {0};

		// We don't actually have a time for the
		// deco stop, we just have a ceiling.
		//
		// We'll just say it's one minute.
		sample.deco.type = DC_DECO_DECOSTOP;
		sample.deco.time = ceiling ? 60 : 0;
		sample.deco.depth = ceiling / 100.0;
		sample.deco.tts = 0;	// Fixme? DC_SAMPLE_TTS?
		if (info->callback) info->callback(DC_SAMPLE_DECO, &sample, info->userdata);
	}
}

static void sample_heading(struct sample_data *info, unsigned short heading)
{
	dc_sample_value_t sample = {0};

	if (heading == 0xffff)
		return;

	sample.event.type = SAMPLE_EVENT_HEADING;
	sample.event.value = heading;
	if (info->callback) info->callback(DC_SAMPLE_EVENT, &sample, info->userdata);
}

static void sample_abspressure(struct sample_data *info, unsigned short pressure)
{
}

static void sample_gastime(struct sample_data *info, short gastime)
{
	dc_sample_value_t sample = {0};

	if (gastime < 0)
		return;

	sample.rbt = gastime / 60;
	if (info->callback) info->callback (DC_SAMPLE_RBT, &sample, info->userdata);
}

/*
 * Per-sample "ventilation" data.
 *
 * It's described as:
 *   - "uint16,precision=6,nillable=65535"
 *   - "x/6000000,x"
 */
static void sample_ventilation(struct sample_data *info, unsigned short unk)
{
}

static void sample_gasnr(struct sample_data *info, unsigned char idx)
{
	info->gasnr = idx;
}

static void sample_pressure(struct sample_data *info, unsigned short pressure)
{
	dc_sample_value_t sample = {0};

	if (pressure == 0xffff)
		return;

	sample.pressure.tank = info->gasnr-1;
	sample.pressure.value = pressure / 100.0;
	if (info->callback) info->callback(DC_SAMPLE_PRESSURE, &sample, info->userdata);
}

static void sample_bookmark_event(struct sample_data *info, unsigned short idx)
{
	dc_sample_value_t sample = {0};

	sample.event.type = SAMPLE_EVENT_BOOKMARK;
	sample.event.value = idx;

	if (info->callback) info->callback(DC_SAMPLE_EVENT, &sample, info->userdata);
}

static void sample_gas_switch_event(struct sample_data *info, unsigned short idx)
{
	suunto_eonsteel_parser_t *eon = info->eon;
	dc_sample_value_t sample = {0};

	if (idx < 1 || idx > eon->cache.GASMIX_COUNT)
		return;

	sample.gasmix = idx - 1;
	if (info->callback) info->callback(DC_SAMPLE_GASMIX, &sample, info->userdata);
}

static const char *mixname(suunto_eonsteel_parser_t *eon, int idx)
{
	dc_gasmix_t *mix;
	static char name[32];
	int o2, he;

	if (idx < 1 || idx > MAXGASES)
		return "invalid";

	mix = &eon->cache.GASMIX[idx-1];
	o2 = lrint(mix->oxygen * 100);
	he = lrint(mix->helium * 100);
	if (he) {
		snprintf(name, sizeof(name), "%d/%d", o2, he);
		return name;
	}
	if (o2 && o2 != 21) {
		snprintf(name, sizeof(name), "NX%d", o2);
		return name;
	}
	return "air";
}

static void sample_insert_gas_event(struct sample_data *info, unsigned short idx)
{
	suunto_eonsteel_parser_t *eon = info->eon;
	dc_sample_value_t sample = {0};
	char event[32];

	if (!info->callback)
		return;

	snprintf(event, sizeof(event), "Create gas %d (%s)", idx, mixname(eon, idx));
	sample.event.type = SAMPLE_EVENT_STRING;
	sample.event.name = strdup(event);
	sample.event.flags = SAMPLE_FLAGS_SEVERITY_INFO;

	info->callback(DC_SAMPLE_EVENT, &sample, info->userdata);
}

static void sample_remove_gas_event(struct sample_data *info, unsigned short idx)
{
	suunto_eonsteel_parser_t *eon = info->eon;
	dc_sample_value_t sample = {0};
	char event[32];

	if (!info->callback)
		return;

	snprintf(event, sizeof(event), "Remove gas %d (%s)", idx, mixname(eon, idx));
	sample.event.type = SAMPLE_EVENT_STRING;
	sample.event.name = strdup(event);
	sample.event.flags = SAMPLE_FLAGS_SEVERITY_INFO;

	info->callback(DC_SAMPLE_EVENT, &sample, info->userdata);
}

/*
 * Look up the string from an enumeration.
 *
 * Enumerations have the enum values in the "format" string,
 * and all start with "enum:" followed by a comma-separated list
 * of enumeration values and strings. Example:
 *
 * "enum:0=NoFly Time,1=Depth,2=Surface Time,3=..."
 */
static char *lookup_enum(const struct type_desc *desc, unsigned char value)
{
	const char *str = desc->format;
	unsigned char c;

	if (!str)
		return NULL;
	if (strncmp(str, "enum:", 5))
		return NULL;
	str += 5;

	while ((c = *str) != 0) {
		unsigned char n;
		const char *begin, *end;
		char *ret;

		str++;
		if (!isdigit(c))
			continue;
		n = c - '0';

		// We only handle one or two digits
		if (isdigit(*str)) {
			n = n*10 + *str - '0';
			str++;
		}

		begin = end = str;
		while ((c = *str) != 0) {
			str++;
			if (c == ',')
				break;
			end = str;
		}

		// Verify that it has the 'n=string' format and skip the equals sign
		if (*begin != '=')
			continue;
		begin++;

		// Is it the value we're looking for?
		if (n != value)
			continue;

		ret = (char *)malloc(end - begin + 1);
		if (!ret)
			break;

		memcpy(ret, begin, end-begin);
		ret[end-begin] = 0;
		return ret;
	}
	return NULL;
}

/*
 * The EON Steel has four different sample events: "state", "notification",
 * "warning" and "alarm". All end up having two fields: type and a boolean value.
 */
static void sample_event_state_type(const struct type_desc *desc, struct sample_data *info, unsigned char type)
{
	free(info->state_type);
	info->state_type = lookup_enum(desc, type);
}

static void sample_event_state_value(const struct type_desc *desc, struct sample_data *info, unsigned char value)
{
	dc_sample_value_t sample = {0};
	const char *name;

	name = info->state_type;
	if (!name)
		return;

	sample.event.type = SAMPLE_EVENT_STRING;
	sample.event.name = name;
	sample.event.flags = value ? SAMPLE_FLAGS_BEGIN : SAMPLE_FLAGS_END;
	sample.event.flags |= 1 << SAMPLE_FLAGS_SEVERITY_SHIFT;

	if (info->callback) info->callback(DC_SAMPLE_EVENT, &sample, info->userdata);
}

static void sample_event_notify_type(const struct type_desc *desc, struct sample_data *info, unsigned char type)
{
	free(info->notify_type);
	info->notify_type = lookup_enum(desc, type);
}

static void sample_event_notify_value(const struct type_desc *desc, struct sample_data *info, unsigned char value)
{
	dc_sample_value_t sample = {0};
	const char *name;

	name = info->notify_type;
	if (!name)
		return;

	sample.event.type = SAMPLE_EVENT_STRING;
	sample.event.name = name;
	sample.event.flags = value ? SAMPLE_FLAGS_BEGIN : SAMPLE_FLAGS_END;
	sample.event.flags |= 2 << SAMPLE_FLAGS_SEVERITY_SHIFT;

	if (info->callback) info->callback(DC_SAMPLE_EVENT, &sample, info->userdata);
}


static void sample_event_warning_type(const struct type_desc *desc, struct sample_data *info, unsigned char type)
{
	free(info->warning_type);
	info->warning_type = lookup_enum(desc, type);
}

static void sample_event_warning_value(const struct type_desc *desc, struct sample_data *info, unsigned char value)
{
	dc_sample_value_t sample = {0};
	const char *name;

	name = info->warning_type;
	if (!name)
		return;

	sample.event.type = SAMPLE_EVENT_STRING;
	sample.event.name = name;
	sample.event.flags = value ? SAMPLE_FLAGS_BEGIN : SAMPLE_FLAGS_END;
	sample.event.flags |= 3 << SAMPLE_FLAGS_SEVERITY_SHIFT;

	if (info->callback) info->callback(DC_SAMPLE_EVENT, &sample, info->userdata);
}

static void sample_event_alarm_type(const struct type_desc *desc, struct sample_data *info, unsigned char type)
{
	free(info->alarm_type);
	info->alarm_type = lookup_enum(desc, type);
}


static void sample_event_alarm_value(const struct type_desc *desc, struct sample_data *info, unsigned char value)
{
	const char *name;
	dc_sample_value_t sample = {0};

	name = info->alarm_type;
	if (!name)
		return;

	sample.event.type = SAMPLE_EVENT_STRING;
	sample.event.name = name;
	sample.event.flags = value ? SAMPLE_FLAGS_BEGIN : SAMPLE_FLAGS_END;
	sample.event.flags |= 4 << SAMPLE_FLAGS_SEVERITY_SHIFT;

	if (info->callback) info->callback(DC_SAMPLE_EVENT, &sample, info->userdata);
}

// enum:0=Low,1=High,2=Custom
static void sample_setpoint_type(const struct type_desc *desc, struct sample_data *info, unsigned char value)
{
	dc_sample_value_t sample = {0};
	char *type = lookup_enum(desc, value);

	if (!type) {
		DEBUG(info->eon->base.context, "sample_setpoint_type(%u) did not match anything in %s", value, desc->format);
		return;
	}

	if (!strcasecmp(type, "Low"))
		sample.setpoint = info->eon->cache.lowsetpoint;
	else if (!strcasecmp(type, "High"))
		sample.setpoint = info->eon->cache.highsetpoint;
	else if (!strcasecmp(type, "Custom"))
		sample.setpoint = info->eon->cache.customsetpoint;
	else {
		DEBUG(info->eon->base.context, "sample_setpoint_type(%u) unknown type '%s'", value, type);
		free(type);
		return;
	}

	if (info->callback) info->callback(DC_SAMPLE_SETPOINT, &sample, info->userdata);
	free(type);
}

// uint32
static void sample_setpoint_po2(struct sample_data *info, unsigned int pressure)
{
	// I *think* this just sets the custom SP, and then
	// we'll get a setpoint_type(2) later.
	info->eon->cache.customsetpoint = pressure / 100000.0;	// Pascal to bar
}

static void sample_setpoint_automatic(struct sample_data *info, unsigned char value)
{
	DEBUG(info->eon->base.context, "sample_setpoint_automatic(%u)", value);
}

static unsigned int handle_sample_type(const struct type_desc *desc, struct sample_data *info, enum eon_sample type, const unsigned char *data)
{
	switch (type) {
	case ES_dtime:
		sample_time(info, array_uint16_le(data));
		return 2;

	case ES_depth:
		sample_depth(info, array_uint16_le(data));
		return 2;

	case ES_temp:
		sample_temp(info, array_uint16_le(data));
		return 2;

	case ES_ndl:
		sample_ndl(info, array_uint16_le(data));
		return 2;

	case ES_ceiling:
		sample_ceiling(info, array_uint16_le(data));
		return 2;

	case ES_tts:
		sample_tts(info, array_uint16_le(data));
		return 2;

	case ES_heading:
		sample_heading(info, array_uint16_le(data));
		return 2;

	case ES_abspressure:
		sample_abspressure(info, array_uint16_le(data));
		return 2;

	case ES_gastime:
		sample_gastime(info, array_uint16_le(data));
		return 2;

	case ES_ventilation:
		sample_ventilation(info, array_uint16_le(data));
		return 2;

	case ES_gasnr:
		sample_gasnr(info, *data);
		return 1;

	case ES_pressure:
		sample_pressure(info, array_uint16_le(data));
		return 2;

	case ES_state:
		sample_event_state_type(desc, info, data[0]);
		return 1;

	case ES_state_active:
		sample_event_state_value(desc, info, data[0]);
		return 1;

	case ES_notify:
		sample_event_notify_type(desc, info, data[0]);
		return 1;

	case ES_notify_active:
		sample_event_notify_value(desc, info, data[0]);
		return 1;

	case ES_warning:
		sample_event_warning_type(desc, info, data[0]);
		return 1;

	case ES_warning_active:
		sample_event_warning_value(desc, info, data[0]);
		return 1;

	case ES_alarm:
		sample_event_alarm_type(desc, info, data[0]);
		return 1;

	case ES_alarm_active:
		sample_event_alarm_value(desc, info, data[0]);
		return 1;

	case ES_bookmark:
		sample_bookmark_event(info, array_uint16_le(data));
		return 2;

	case ES_gasswitch:
		sample_gas_switch_event(info, array_uint16_le(data));
		return 2;

	case ES_setpoint_type:
		sample_setpoint_type(desc, info, data[0]);
		return 1;

	case ES_setpoint_po2:
		sample_setpoint_po2(info, array_uint32_le(data));
		return 4;

	case ES_setpoint_automatic:	// bool
		sample_setpoint_automatic(info, data[0]);
		return 1;

	case ES_insertgas:
		sample_insert_gas_event(info, array_uint16_le(data));
		return 2;

	case ES_removegas:
		sample_remove_gas_event(info, array_uint16_le(data));
		return 2;

	default:
		return 0;
	}
}

static int traverse_samples(unsigned short type, const struct type_desc *desc, const unsigned char *data, unsigned int len, void *user)
{
	struct sample_data *info = (struct sample_data *) user;
	suunto_eonsteel_parser_t *eon = info->eon;
	int i, used = 0;

	if (desc->size > len)
		ERROR(eon->base.context, "Got %d bytes of data for '%s' that wants %d bytes", len, desc->desc, desc->size);

	for (i = 0; i < EON_MAX_GROUP; i++) {
		unsigned int bytes = handle_sample_type(desc, info, desc->type[i], data);

		if (!bytes)
			break;
		if (bytes > len) {
			ERROR(eon->base.context, "Wanted %d bytes of data, only had %d bytes ('%s' idx %d)", bytes, len, desc->desc, i);
			break;
		}
		data += bytes;
		len -= bytes;
		used += bytes;
	}

	// Warn if there are left-over bytes for something we did use part of
	if (used && len)
		ERROR(eon->base.context, "Entry for '%s' had %d bytes, only used %d", desc->desc, len+used, used);
	return 0;
}

static dc_status_t
suunto_eonsteel_parser_samples_foreach(dc_parser_t *abstract, dc_sample_callback_t callback, void *userdata)
{
	suunto_eonsteel_parser_t *eon = (suunto_eonsteel_parser_t *) abstract;
	struct sample_data data = { eon, callback, userdata, 0 };

	traverse_data(eon, traverse_samples, &data);

	free(data.state_type);
	free(data.notify_type);
	free(data.warning_type);
	free(data.alarm_type);

	return DC_STATUS_SUCCESS;
}

static dc_status_t
suunto_eonsteel_parser_get_field(dc_parser_t *parser, dc_field_type_t type, unsigned int flags, void *value)
{
	suunto_eonsteel_parser_t *eon = (suunto_eonsteel_parser_t *)parser;

	if (!(eon->cache.initialized & (1 << type)))
		return DC_STATUS_UNSUPPORTED;

	/* Fix up the cylinder info before using dc_field_get() */
	if (type == DC_FIELD_TANK) {
		if (flags >= MAXGASES)
			return DC_STATUS_UNSUPPORTED;

		dc_tank_t *tank = (dc_tank_t *) value;

		/*
		 * Sadly it seems that the EON Steel doesn't tell us whether
		 * we get imperial or metric data - the only indication is
		 * that metric is (at least so far) always whole liters
		 */
		tank->volume = eon->cache.tanksize[flags];
		tank->gasmix = flags;

		/*
		 * The pressure reported is NOT the pressure the user enters.
		 *
		 * So 3000psi turns into 206.700 bar instead of 206.843 bar;
		 * We report it as we get it and let the application figure out
		 * what to do with that
		 */
		tank->workpressure = eon->cache.tankworkingpressure[flags];
		tank->type = eon->cache.tankinfo[flags];

		/*
		 * See if we should call this imperial instead.
		 *
		 * We need to have workpressure and a valid tank. In that case,
		 * a fractional tank size implies imperial.
		 */
		if (tank->workpressure && (tank->type & DC_TANKINFO_METRIC)) {
			if (fabs(tank->volume - rint(tank->volume)) > 0.001)
				tank->type += DC_TANKINFO_IMPERIAL - DC_TANKINFO_METRIC;
		}
		tank->usage = eon->cache.tankusage[flags];
	}

	return dc_field_get(&eon->cache, type, flags, value);
}

/*
 * The time of the dive is encoded in the filename,
 * and we've saved it off as the four first bytes
 * of the dive data (in little-endian format).
 */
static dc_status_t
suunto_eonsteel_parser_get_datetime(dc_parser_t *parser, dc_datetime_t *datetime)
{
	if (parser->size < 4)
		return DC_STATUS_UNSUPPORTED;

	if (!dc_datetime_gmtime(datetime, array_uint32_le(parser->data)))
		return DC_STATUS_DATAFORMAT;

	datetime->timezone = DC_TIMEZONE_NONE;

	return DC_STATUS_SUCCESS;
}

// time in ms
static void add_time_field(suunto_eonsteel_parser_t *eon, unsigned short time_delta_ms)
{
	eon->cache.DIVETIME += time_delta_ms;
}

// depth in cm
static void set_depth_field(suunto_eonsteel_parser_t *eon, unsigned short d)
{
	if (d != 0xffff) {
		double depth = d / 100.0;
		if (depth > eon->cache.MAXDEPTH)
			eon->cache.MAXDEPTH = depth;
		eon->cache.initialized |= 1 << DC_FIELD_MAXDEPTH;
	}
}

// new gas:
//  "sml.DeviceLog.Header.Diving.Gases+Gas.State"
//
// We eventually need to parse the descriptor for that 'enum type'.
// Two versions so far:
//   "enum:0=Off,1=Primary,2=?,3=Diluent"
//   "enum:0=Off,1=Primary,3=Diluent,4=Oxygen"
//
// We turn that into the DC_TANKINFO data here, but
// initially consider all non-off tanks to me METRIC.
//
// We may later turn the METRIC tank size into IMPERIAL if we
// get a working pressure and non-integral size
static dc_status_t add_gas_type(suunto_eonsteel_parser_t *eon, const struct type_desc *desc, unsigned char type)
{
	int idx = eon->cache.GASMIX_COUNT;
	dc_tankinfo_t tankinfo = DC_TANKINFO_METRIC;
	dc_usage_t usage = DC_USAGE_NONE;
	char *name;

	if (idx >= MAXGASES)
		return DC_STATUS_SUCCESS;

	eon->cache.GASMIX_COUNT = idx+1;
	name = lookup_enum(desc, type);
	if (!name)
		DEBUG(eon->base.context, "Unable to look up gas type %u in %s", type, desc->format);
	else if (!strcasecmp(name, "Diluent"))
		usage = DC_USAGE_DILUENT;
	else if (!strcasecmp(name, "Oxygen"))
		usage = DC_USAGE_OXYGEN;
	else if (!strcasecmp(name, "None"))
		tankinfo = DC_TANKVOLUME_NONE;
	else if (strcasecmp(name, "Primary"))
		DEBUG(eon->base.context, "Unknown gas type %u (%s)", type, name);

	eon->cache.tankinfo[idx] = tankinfo;
	eon->cache.tankusage[idx] = usage;
	eon->cache.GASMIX[idx].usage = usage;

	eon->cache.initialized |= 1 << DC_FIELD_GASMIX_COUNT;
	eon->cache.initialized |= 1 << DC_FIELD_TANK_COUNT;
	free(name);
	return DC_STATUS_SUCCESS;
}

// "sml.DeviceLog.Header.Diving.Gases.Gas.Oxygen"
// O2 percentage as a byte
static dc_status_t add_gas_o2(suunto_eonsteel_parser_t *eon, unsigned char o2)
{
	int idx = eon->cache.GASMIX_COUNT-1;
	if (idx >= 0)
		eon->cache.GASMIX[idx].oxygen = o2 / 100.0;
	eon->cache.initialized |= 1 << DC_FIELD_GASMIX;
	return DC_STATUS_SUCCESS;
}

// "sml.DeviceLog.Header.Diving.Gases.Gas.Helium"
// He percentage as a byte
static dc_status_t add_gas_he(suunto_eonsteel_parser_t *eon, unsigned char he)
{
	int idx = eon->cache.GASMIX_COUNT-1;
	if (idx >= 0)
		eon->cache.GASMIX[idx].helium = he / 100.0;
	eon->cache.initialized |= 1 << DC_FIELD_GASMIX;
	return DC_STATUS_SUCCESS;
}

static dc_status_t add_gas_size(suunto_eonsteel_parser_t *eon, float l)
{
	int idx = eon->cache.GASMIX_COUNT-1;
	if (idx >= 0)
		eon->cache.tanksize[idx] = l;
	eon->cache.initialized |= 1 << DC_FIELD_TANK;
	return DC_STATUS_SUCCESS;
}

static dc_status_t add_gas_workpressure(suunto_eonsteel_parser_t *eon, float wp)
{
	int idx = eon->cache.GASMIX_COUNT-1;
	if (idx >= 0)
		eon->cache.tankworkingpressure[idx] = wp;
	return DC_STATUS_SUCCESS;
}

static float get_le32_float(const unsigned char *src)
{
	union {
		unsigned int val;
		float result;
	} u;

	u.val = array_uint32_le(src);
	return u.result;
}

// "Device" fields are all utf8:
//   Info.BatteryAtEnd
//   Info.BatteryAtStart
//   Info.BSL
//   Info.HW
//   Info.SW
//   Name
//   SerialNumber
static dc_status_t traverse_device_fields(suunto_eonsteel_parser_t *eon,
                                          const struct type_desc *desc,
                                          const unsigned char *data, int len)
{
	const char *name = desc->desc + strlen("sml.DeviceLog.Device.");
	if (!strcmp(name, "SerialNumber"))
		return dc_field_add_string(&eon->cache, "Serial", data);
	if (!strcmp(name, "Info.HW"))
		return dc_field_add_string(&eon->cache, "HW Version", data);
	if (!strcmp(name, "Info.SW"))
		return dc_field_add_string(&eon->cache, "FW Version", data);
	if (!strcmp(name, "Info.BatteryAtStart"))
		return dc_field_add_string(&eon->cache, "Battery at start", data);
	if (!strcmp(name, "Info.BatteryAtEnd"))
		return dc_field_add_string(&eon->cache, "Battery at end", data);
	return DC_STATUS_SUCCESS;
}

// "sml.DeviceLog.Header.Diving.Gases"
//
//   +Gas.State (enum:0=Off,1=Primary,3=Diluent,4=Oxygen)
//   .Gas.Oxygen (uint8,precision=2)
//   .Gas.Helium (uint8,precision=2)
//   .Gas.PO2 (uint32)
//   .Gas.TransmitterID (utf8)
//   .Gas.TankSize (float32,precision=5)
//   .Gas.TankFillPressure (float32,precision=0)
//   .Gas.StartPressure (float32,precision=0)
//   .Gas.EndPressure (float32,precision=0)
//   .Gas.TransmitterStartBatteryCharge (int8,precision=2)
//   .Gas.TransmitterEndBatteryCharge (int8,precision=2)
static dc_status_t traverse_gas_fields(suunto_eonsteel_parser_t *eon,
                                       const struct type_desc *desc,
                                       const unsigned char *data, int len)
{
	const char *name = desc->desc + strlen("sml.DeviceLog.Header.Diving.Gases");

	if (!strcmp(name, "+Gas.State"))
		return add_gas_type(eon, desc, data[0]);

	if (!strcmp(name, ".Gas.Oxygen"))
		return add_gas_o2(eon, data[0]);

	if (!strcmp(name, ".Gas.Helium"))
		return add_gas_he(eon, data[0]);

	if (!strcmp(name, ".Gas.TransmitterID"))
		return dc_field_add_string(&eon->cache, "Transmitter ID", data);

	if (!strcmp(name, ".Gas.TankSize"))
		return add_gas_size(eon, get_le32_float(data));

	if (!strcmp(name, ".Gas.TankFillPressure"))
		return add_gas_workpressure(eon, get_le32_float(data));

	// There is a bug with older transmitters, where the transmitter
	// battery charge returns zero. Rather than returning that bogus
	// data, just don't return any battery charge information at all.
	//
	// Make sure to add all non-battery-charge field checks above this
	// test, so that it doesn't trigger for anything else.
	if (!data[0])
		return 0;

	if (!strcmp(name, ".Gas.TransmitterStartBatteryCharge"))
		return dc_field_add_string_fmt(&eon->cache, "Transmitter Battery at start", "%d %%", data[0]);

	if (!strcmp(name, ".Gas.TransmitterEndBatteryCharge"))
		return dc_field_add_string_fmt(&eon->cache, "Transmitter Battery at end", "%d %%", data[0]);

	return DC_STATUS_SUCCESS;
}


// "sml.DeviceLog.Header.Diving."
//
//   SurfaceTime (uint32)
//   NumberInSeries (uint32)
//   Algorithm (utf8)
//   SurfacePressure (uint32)
//   Conservatism (int8)
//   Altitude (uint16)
//   AlgorithmTransitionDepth (uint8)
//   DaysInSeries (uint32)
//   PreviousDiveDepth (float32,precision=2)
//   LowSetPoint (uint32)
//   HighSetPoint (uint32)
//   SwitchHighSetPoint.Enabled (bool)
//   SwitchHighSetPoint.Depth (float32,precision=1)
//   SwitchLowSetPoint.Enabled (bool)
//   SwitchLowSetPoint.Depth (float32,precision=1)
//   StartTissue.CNS (float32,precision=3)
//   StartTissue.OTU (float32)
//   StartTissue.OLF (float32,precision=3)
//   StartTissue.Nitrogen+Pressure (uint32)
//   StartTissue.Helium+Pressure (uint32)
//   StartTissue.RgbmNitrogen (float32,precision=3)
//   StartTissue.RgbmHelium (float32,precision=3)
//   DiveMode (utf8)
//   AlgorithmBottomTime (uint32)
//   AlgorithmAscentTime (uint32)
//   AlgorithmBottomMixture.Oxygen (uint8,precision=2)
//   AlgorithmBottomMixture.Helium (uint8,precision=2)
//   DesaturationTime (uint32)
//   EndTissue.CNS (float32,precision=3)
//   EndTissue.OTU (float32)
//   EndTissue.OLF (float32,precision=3)
//   EndTissue.Nitrogen+Pressure (uint32)
//   EndTissue.Helium+Pressure (uint32)
//   EndTissue.RgbmNitrogen (float32,precision=3)
//   EndTissue.RgbmHelium (float32,precision=3)
static dc_status_t traverse_diving_fields(suunto_eonsteel_parser_t *eon,
                                          const struct type_desc *desc,
                                          const unsigned char *data, int len)
{
	const char *name = desc->desc + strlen("sml.DeviceLog.Header.Diving.");

	if (!strncmp(name, "Gases", 5))
		return traverse_gas_fields(eon, desc, data, len);

	if (!strcmp(name, "SurfacePressure")) {
		unsigned int pressure = array_uint32_le(data); // in SI units - Pascal
		DC_ASSIGN_FIELD(eon->cache, ATMOSPHERIC, pressure / 100000.0); // bar
		return DC_STATUS_SUCCESS;
	}

	if (!strcmp(name, "Algorithm"))
		return dc_field_add_string(&eon->cache, "Deco algorithm", data);

	if (!strcmp(name, "DiveMode")) {
		if (!strncmp((const char *)data, "CCR", 3)) {
			DC_ASSIGN_FIELD(eon->cache, DIVEMODE, DC_DIVEMODE_CCR);
		}
		return dc_field_add_string(&eon->cache, "Dive Mode", data);
	}

	/* Signed byte of conservatism (-2 .. +2) */
	if (!strcmp(name, "Conservatism")) {
		int val = *(signed char *)data;

		return dc_field_add_string_fmt(&eon->cache, "Personal Adjustment", "P%d", val);
	}

	if (!strcmp(name, "LowSetPoint")) {
		unsigned int pressure = array_uint32_le(data); // in SI units - Pascal
		eon->cache.lowsetpoint = pressure / 100000.0; // bar
		return 0;
	}

	if (!strcmp(name, "HighSetPoint")) {
		unsigned int pressure = array_uint32_le(data); // in SI units - Pascal
		eon->cache.highsetpoint = pressure / 100000.0; // bar
		return 0;
	}

	// Time recoded in seconds.
	// Let's just agree to ignore seconds
	if (!strcmp(name, "DesaturationTime")) {
		unsigned int time = array_uint32_le(data) / 60;
		return dc_field_add_string_fmt(&eon->cache, "Desaturation Time", "%d:%02d", time / 60, time % 60);
	}

	if (!strcmp(name, "SurfaceTime")) {
		unsigned int time = array_uint32_le(data) / 60;
		return dc_field_add_string_fmt(&eon->cache, "Surface Time", "%d:%02d", time / 60, time % 60);
	}

	return DC_STATUS_SUCCESS;
}

// "Header" fields are:
//   Activity (utf8)
//   DateTime (utf8)
//   Depth.Avg (float32,precision=2)
//   Depth.Max (float32,precision=2)
//   Diving.*
//   Duration (uint32)
//   PauseDuration (uint32)
//   SampleInterval (uint8)
static dc_status_t traverse_header_fields(suunto_eonsteel_parser_t *eon,
                                          const struct type_desc *desc,
                                          const unsigned char *data, int len)
{
	const char *name = desc->desc + strlen("sml.DeviceLog.Header.");

	if (!strncmp(name, "Diving.", 7))
		return traverse_diving_fields(eon, desc, data, len);

	if (!strcmp(name, "Depth.Max")) {
		double d = get_le32_float(data);
		if (d > eon->cache.MAXDEPTH)
			DC_ASSIGN_FIELD(eon->cache, MAXDEPTH, d);
		return DC_STATUS_SUCCESS;
	}
	if (!strcmp(name, "DateTime"))
		return dc_field_add_string(&eon->cache, "Dive ID", data);

	return DC_STATUS_SUCCESS;
}

static dc_status_t traverse_dynamic_fields(suunto_eonsteel_parser_t *eon, const struct type_desc *desc, const unsigned char *data, int len)
{
	const char *name = desc->desc;

	if (!strncmp(name, "sml.", 4)) {
		name += 4;
		if (!strncmp(name, "DeviceLog.", 10)) {
			name += 10;
			if (!strncmp(name, "Device.", 7))
				return traverse_device_fields(eon, desc, data, len);
			if (!strncmp(name, "Header.", 7)) {
				return traverse_header_fields(eon, desc, data, len);
			}
		}
	}
	return DC_STATUS_SUCCESS;
}

/*
 * This is a simplified sample parser that only parses the depth and time
 * samples. It also depends on the GRP entries always starting with time/depth,
 * and just stops on anything else.
 */
static int traverse_sample_fields(suunto_eonsteel_parser_t *eon, const struct type_desc *desc, const unsigned char *data, int len)
{
	int i;

	for (i = 0; i < EON_MAX_GROUP; i++) {
		enum eon_sample type = desc->type[i];

		switch (type) {
		case ES_dtime:
			add_time_field(eon, array_uint16_le(data));
			data += 2;
			continue;
		case ES_depth:
			set_depth_field(eon, array_uint16_le(data));
			data += 2;
			continue;
		default:
			break;
		}
		break;
	}
	return 0;
}

static int traverse_fields(unsigned short type, const struct type_desc *desc, const unsigned char *data, unsigned int len, void *user)
{
	suunto_eonsteel_parser_t *eon = (suunto_eonsteel_parser_t *) user;

	// Sample type? Do basic maxdepth and time parsing
	if (desc->type[0])
		traverse_sample_fields(eon, desc, data, len);
	else
		traverse_dynamic_fields(eon, desc, data, len);

	return 0;
}


static void initialize_field_caches(suunto_eonsteel_parser_t *eon)
{
	memset(&eon->cache, 0, sizeof(eon->cache));
	eon->cache.initialized = 1 << DC_FIELD_DIVETIME;

	traverse_data(eon, traverse_fields, eon);

	// The internal time fields are in ms and have to be added up
	// like that. At the end, we translate it back to seconds.
	eon->cache.DIVETIME /= 1000;
}

static void show_descriptor(suunto_eonsteel_parser_t *eon, int nr, struct type_desc *desc)
{
	int i;

	if (!desc->desc)
		return;
	DEBUG(eon->base.context, "Descriptor %d: '%s', size %d bytes", nr, desc->desc, desc->size);
	if (desc->format)
		DEBUG(eon->base.context, "    format '%s'", desc->format);
	if (desc->mod)
		DEBUG(eon->base.context, "    mod '%s'", desc->mod);
	for (i = 0; i < EON_MAX_GROUP; i++) {
		enum eon_sample type = desc->type[i];
		if (!type)
			continue;
		DEBUG(eon->base.context, "    %d: %d (%s)", i, type, desc_type_name(type));
	}
}

static void show_all_descriptors(suunto_eonsteel_parser_t *eon)
{
	for (unsigned int i = 0; i < MAXTYPE; ++i)
		show_descriptor(eon, i, eon->type_desc+i);
}

static dc_status_t
suunto_eonsteel_parser_destroy(dc_parser_t *parser)
{
	suunto_eonsteel_parser_t *eon = (suunto_eonsteel_parser_t *) parser;

	desc_free(eon->type_desc, MAXTYPE);

	return DC_STATUS_SUCCESS;
}

static const dc_parser_vtable_t suunto_eonsteel_parser_vtable = {
	sizeof(suunto_eonsteel_parser_t),
	DC_FAMILY_SUUNTO_EONSTEEL,
	NULL, /* set_clock */
	NULL, /* set_atmospheric */
	NULL, /* set_density */
	suunto_eonsteel_parser_get_datetime, /* datetime */
	suunto_eonsteel_parser_get_field, /* fields */
	suunto_eonsteel_parser_samples_foreach, /* samples_foreach */
	suunto_eonsteel_parser_destroy /* destroy */
};

dc_status_t
suunto_eonsteel_parser_create(dc_parser_t **out, dc_context_t *context, const unsigned char data[], size_t size, unsigned int model)
{
	suunto_eonsteel_parser_t *parser = NULL;

	if (out == NULL)
		return DC_STATUS_INVALIDARGS;

	parser = (suunto_eonsteel_parser_t *) dc_parser_allocate (context, &suunto_eonsteel_parser_vtable, data, size);
	if (parser == NULL) {
		ERROR (context, "Failed to allocate memory.");
		return DC_STATUS_NOMEMORY;
	}

	memset(&parser->type_desc, 0, sizeof(parser->type_desc));
	memset(&parser->cache, 0, sizeof(parser->cache));

	initialize_field_caches(parser);
	show_all_descriptors(parser);

	*out = (dc_parser_t *) parser;

	return DC_STATUS_SUCCESS;
}
