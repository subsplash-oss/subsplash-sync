/*
 * Minimal fake <obs-module.h> for unit tests.
 *
 * Provides LOG_* constants and obs_data_* types/functions so that
 * production source files can be compiled without the real OBS SDK.
 * The obs_data layer is backed by cJSON in obs-stubs.c.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define LOG_ERROR   100
#define LOG_WARNING 200
#define LOG_INFO    300
#define LOG_DEBUG   400

typedef struct obs_data obs_data_t;
typedef struct obs_data_array obs_data_array_t;

obs_data_t *obs_data_create_from_json(const char *json_string);
const char *obs_data_get_string(obs_data_t *data, const char *name);
long long obs_data_get_int(obs_data_t *data, const char *name);
bool obs_data_get_bool(obs_data_t *data, const char *name);
obs_data_t *obs_data_get_obj(obs_data_t *data, const char *name);
obs_data_array_t *obs_data_get_array(obs_data_t *data, const char *name);
size_t obs_data_array_count(obs_data_array_t *array);
obs_data_t *obs_data_array_item(obs_data_array_t *array, size_t idx);
void obs_data_release(obs_data_t *data);
void obs_data_array_release(obs_data_array_t *array);
