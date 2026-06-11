#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <time.h>
#include <pthread.h>

#define SUBSPLASH_MAX_URL 512
#define SUBSPLASH_MAX_TOKEN 4096
#define SUBSPLASH_MAX_FIELD 256
#define SUBSPLASH_MAX_ID 64

typedef struct {
	char base_url[SUBSPLASH_MAX_URL];
	char client_id[SUBSPLASH_MAX_FIELD];
	char client_secret[SUBSPLASH_MAX_FIELD];
	char app_key[8];
	char access_token[SUBSPLASH_MAX_TOKEN];
	time_t token_expiry;
	pthread_mutex_t token_lock;
	bool initialized;
} subsplash_client_t;

typedef struct {
	char id[SUBSPLASH_MAX_ID];
	char start_at[32];
	char end_at[32];
	char status[24];
	time_t start_epoch;
	time_t end_epoch;
	bool simulated_live;
	bool valid;
} subsplash_broadcast_t;

/* Fetch result codes returned alongside the broadcast struct. */
#define SUBSPLASH_FETCH_OK         0
#define SUBSPLASH_FETCH_API_ERROR  1
#define SUBSPLASH_FETCH_NO_DATA    2
#define SUBSPLASH_FETCH_AUTH_ERROR 3

bool subsplash_client_init(subsplash_client_t *client, const char *base_url, const char *client_id,
			   const char *client_secret, const char *app_key);

void subsplash_client_destroy(subsplash_client_t *client);

bool subsplash_client_authenticate(subsplash_client_t *client);

/*
 * Fetch the earliest non-terminal broadcast whose scheduled end_at
 * is still in the future. Skips ended/on-demand/never-happened
 * results. Returns a SUBSPLASH_FETCH_* result code.
 */
int subsplash_client_fetch_broadcasts(subsplash_client_t *client, subsplash_broadcast_t *out);

/*
 * Fetch a single broadcast by ID. Used as a defensive fallback when the
 * tracked broadcast drops from list results.
 */
int subsplash_client_fetch_by_id(subsplash_client_t *client, const char *id, subsplash_broadcast_t *out);

bool subsplash_client_test_connection(subsplash_client_t *client, char *status_out, size_t status_len);

#ifdef __cplusplus
}
#endif
