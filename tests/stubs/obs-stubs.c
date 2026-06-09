/*
 * OBS API stubs for unit tests.
 *
 * obs_log is a silent no-op.  The obs_data_* family is backed by
 * cJSON so tests can feed real JSON strings through the production
 * parsing code.
 */
#include "obs-module.h"

#include <cJSON.h>

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* ---- plugin-support.h symbols ------------------------------------ */

const char *PLUGIN_NAME = "test";
const char *PLUGIN_VERSION = "0.0.0";

void obs_log(int log_level, const char *format, ...)
{
	(void)log_level;
	(void)format;
}

void blogva(int log_level, const char *format, va_list args)
{
	(void)log_level;
	(void)format;
	(void)args;
}

/* ---- obs_data backed by cJSON ------------------------------------ */

struct obs_data {
	cJSON *json;
	bool owned;
};

struct obs_data_array {
	cJSON *json;
};

obs_data_t *obs_data_create_from_json(const char *json_string)
{
	cJSON *json = cJSON_Parse(json_string);
	if (!json)
		return NULL;

	obs_data_t *data = calloc(1, sizeof(*data));
	data->json = json;
	data->owned = true;
	return data;
}

const char *obs_data_get_string(obs_data_t *data, const char *name)
{
	if (!data || !data->json)
		return "";
	cJSON *item = cJSON_GetObjectItemCaseSensitive(data->json, name);
	if (!item || !cJSON_IsString(item))
		return "";
	return item->valuestring;
}

long long obs_data_get_int(obs_data_t *data, const char *name)
{
	if (!data || !data->json)
		return 0;
	cJSON *item = cJSON_GetObjectItemCaseSensitive(data->json, name);
	if (!item || !cJSON_IsNumber(item))
		return 0;
	return (long long)item->valuedouble;
}

bool obs_data_get_bool(obs_data_t *data, const char *name)
{
	if (!data || !data->json)
		return false;
	cJSON *item = cJSON_GetObjectItemCaseSensitive(data->json, name);
	if (!item)
		return false;
	return cJSON_IsTrue(item);
}

obs_data_t *obs_data_get_obj(obs_data_t *data, const char *name)
{
	if (!data || !data->json)
		return NULL;
	cJSON *item = cJSON_GetObjectItemCaseSensitive(data->json, name);
	if (!item || !cJSON_IsObject(item))
		return NULL;

	obs_data_t *obj = calloc(1, sizeof(*obj));
	obj->json = item;
	obj->owned = false;
	return obj;
}

obs_data_array_t *obs_data_get_array(obs_data_t *data, const char *name)
{
	if (!data || !data->json)
		return NULL;
	cJSON *item = cJSON_GetObjectItemCaseSensitive(data->json, name);
	if (!item || !cJSON_IsArray(item))
		return NULL;

	obs_data_array_t *arr = calloc(1, sizeof(*arr));
	arr->json = item;
	return arr;
}

size_t obs_data_array_count(obs_data_array_t *array)
{
	if (!array || !array->json)
		return 0;
	return (size_t)cJSON_GetArraySize(array->json);
}

obs_data_t *obs_data_array_item(obs_data_array_t *array, size_t idx)
{
	if (!array || !array->json)
		return NULL;
	cJSON *item = cJSON_GetArrayItem(array->json, (int)idx);
	if (!item)
		return NULL;

	obs_data_t *data = calloc(1, sizeof(*data));
	data->json = item;
	data->owned = false;
	return data;
}

void obs_data_release(obs_data_t *data)
{
	if (!data)
		return;
	if (data->owned && data->json)
		cJSON_Delete(data->json);
	free(data);
}

void obs_data_array_release(obs_data_array_t *array)
{
	if (!array)
		return;
	free(array);
}
