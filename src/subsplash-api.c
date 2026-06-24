#include "subsplash-api.h"

#include "compat-atomics.h"
#include "plugin-support.h"

#include <curl/curl.h>

#include <obs-module.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/* curl write-callback: appends received data to a growable buffer    */
/* ------------------------------------------------------------------ */

struct curl_buf {
	char *data;
	size_t size;
};

static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *userdata)
{
	size_t total_bytes = size * nmemb;
	struct curl_buf *buf = (struct curl_buf *)userdata;

	char *tmp = realloc(buf->data, buf->size + total_bytes + 1);
	if (!tmp)
		return 0;

	buf->data = tmp;
	memcpy(buf->data + buf->size, ptr, total_bytes);
	buf->size += total_bytes;
	buf->data[buf->size] = '\0';
	return total_bytes;
}

/* ------------------------------------------------------------------ */
/* curl progress callback: aborts the transfer when the owning client */
/* has been asked to abort (e.g. during shutdown), so a joining thread */
/* never waits out a full network timeout.                            */
/* ------------------------------------------------------------------ */

static int curl_progress_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
	(void)dltotal;
	(void)dlnow;
	(void)ultotal;
	(void)ulnow;

	subsplash_client_t *client = (subsplash_client_t *)clientp;
	if (client && sched_atomic_load(&client->abort_requested))
		return 1; /* non-zero aborts the transfer */
	return 0;
}

/* ------------------------------------------------------------------ */
/* Shared timeout/safety options applied to every curl handle.        */
/*                                                                    */
/* NOSIGNAL is required for use off the main thread; CONNECTTIMEOUT   */
/* bounds the connect/DNS phase that CURLOPT_TIMEOUT alone can miss;   */
/* the progress callback lets shutdown abort an in-flight request.    */
/* ------------------------------------------------------------------ */

static void apply_common_curl_opts(CURL *curl, subsplash_client_t *client)
{
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curl_progress_cb);
	curl_easy_setopt(curl, CURLOPT_XFERINFODATA, client);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
}

/* ------------------------------------------------------------------ */
/* ISO-8601 helper: "2026-03-29T10:00:00Z" -> time_t (UTC)           */
/* ------------------------------------------------------------------ */

static time_t parse_iso8601(const char *str)
{
	int year = 0;
	int month = 0;
	int day = 0;
	int hour = 0;
	int minute = 0;
	int second = 0;

	if (sscanf(str, "%d-%d-%dT%d:%d:%dZ", &year, &month, &day, &hour, &minute, &second) != 6)
		return (time_t)-1;

	struct tm time_components;
	memset(&time_components, 0, sizeof(time_components));
	time_components.tm_year = year - 1900;
	time_components.tm_mon = month - 1;
	time_components.tm_mday = day;
	time_components.tm_hour = hour;
	time_components.tm_min = minute;
	time_components.tm_sec = second;
	time_components.tm_isdst = 0;

#if defined(_WIN32)
	time_t result = _mkgmtime(&time_components);
#elif defined(__APPLE__) || defined(__linux__)
	time_t result = timegm(&time_components);
#else
	char *old_tz = getenv("TZ");
	char *saved_tz = old_tz ? strdup(old_tz) : NULL;
	setenv("TZ", "UTC", 1);
	tzset();
	time_t result = mktime(&time_components);
	if (saved_tz) {
		setenv("TZ", saved_tz, 1);
		free(saved_tz);
	} else {
		unsetenv("TZ");
	}
	tzset();
#endif
	return result;
}

/* ------------------------------------------------------------------ */
/* Parse a single broadcast object from an obs_data_t JSON node.      */
/* ------------------------------------------------------------------ */

static void parse_broadcast_json(obs_data_t *broadcast_obj, subsplash_broadcast_t *out)
{
	const char *id = obs_data_get_string(broadcast_obj, "id");
	const char *start_at = obs_data_get_string(broadcast_obj, "start_at");
	const char *end_at = obs_data_get_string(broadcast_obj, "end_at");
	const char *status = obs_data_get_string(broadcast_obj, "status");

	if (id)
		snprintf(out->id, sizeof(out->id), "%s", id);
	if (start_at)
		snprintf(out->start_at, sizeof(out->start_at), "%s", start_at);
	if (end_at)
		snprintf(out->end_at, sizeof(out->end_at), "%s", end_at);
	if (status)
		snprintf(out->status, sizeof(out->status), "%s", status);

	out->start_epoch = parse_iso8601(out->start_at);
	out->end_epoch = parse_iso8601(out->end_at);
	out->simulated_live = obs_data_get_bool(broadcast_obj, "simulated_live");
	out->valid = true;
}

/* ------------------------------------------------------------------ */
/* Perform an authenticated GET request and return the response body. */
/* ------------------------------------------------------------------ */

static CURLcode perform_authenticated_get(subsplash_client_t *client, const char *url, struct curl_buf *response,
					  long *out_http_code)
{
	CURL *curl = curl_easy_init();
	if (!curl)
		return CURLE_FAILED_INIT;

	char auth_header[SUBSPLASH_MAX_TOKEN + 32];
	snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s", client->access_token);

	struct curl_slist *headers = NULL;
	headers = curl_slist_append(headers, auth_header);

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, response);
	apply_common_curl_opts(curl, client);

	CURLcode result = curl_easy_perform(curl);

	if (result == CURLE_OK && out_http_code)
		curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, out_http_code);

	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	return result;
}

/* ------------------------------------------------------------------ */
/* Build the URL-encoded form body for the client_credentials token   */
/* request. client_id and client_secret are percent-encoded so values */
/* containing reserved characters (& = + %) don't corrupt the body.   */
/* ------------------------------------------------------------------ */

static bool build_token_post_fields(CURL *curl, char *out, size_t out_size, const char *client_id,
				    const char *client_secret)
{
	char *escaped_id = curl_easy_escape(curl, client_id, 0);
	char *escaped_secret = curl_easy_escape(curl, client_secret, 0);

	bool ok = false;
	if (escaped_id && escaped_secret) {
		int written = snprintf(out, out_size, "client_id=%s&client_secret=%s&grant_type=client_credentials",
				       escaped_id, escaped_secret);
		ok = written > 0 && (size_t)written < out_size;
	}

	curl_free(escaped_id);
	curl_free(escaped_secret);
	return ok;
}

/* ------------------------------------------------------------------ */
/* subsplash_client_init                                              */
/* ------------------------------------------------------------------ */

bool subsplash_client_init(subsplash_client_t *client, const char *base_url, const char *client_id,
			   const char *client_secret, const char *app_key)
{
	if (!client || !base_url || !client_id || !client_secret || !app_key)
		return false;

	memset(client, 0, sizeof(*client));

	snprintf(client->base_url, sizeof(client->base_url), "%s", base_url);
	snprintf(client->client_id, sizeof(client->client_id), "%s", client_id);
	snprintf(client->client_secret, sizeof(client->client_secret), "%s", client_secret);
	snprintf(client->app_key, sizeof(client->app_key), "%s", app_key);

	pthread_mutex_init(&client->token_lock, NULL);
	client->initialized = true;
	return true;
}

/* ------------------------------------------------------------------ */
/* subsplash_client_destroy                                           */
/* ------------------------------------------------------------------ */

void subsplash_client_abort(subsplash_client_t *client)
{
	if (!client)
		return;
	sched_atomic_store(&client->abort_requested, 1);
}

void subsplash_client_destroy(subsplash_client_t *client)
{
	if (!client)
		return;

	if (client->initialized)
		pthread_mutex_destroy(&client->token_lock);

	memset(client, 0, sizeof(*client));
}

/* ------------------------------------------------------------------ */
/* subsplash_client_authenticate                                      */
/* ------------------------------------------------------------------ */

bool subsplash_client_authenticate(subsplash_client_t *client)
{
	if (!client || !client->initialized)
		return false;

	pthread_mutex_lock(&client->token_lock);

	if (client->access_token[0] != '\0' && time(NULL) < client->token_expiry) {
		pthread_mutex_unlock(&client->token_lock);
		return true;
	}

	char url[SUBSPLASH_MAX_URL + 64];
	snprintf(url, sizeof(url), "%s/tokens/v1/token", client->base_url);

	CURL *curl = curl_easy_init();
	if (!curl) {
		obs_log(LOG_WARNING, "subsplash: curl_easy_init failed for auth");
		pthread_mutex_unlock(&client->token_lock);
		return false;
	}

	char post_fields[SUBSPLASH_MAX_FIELD * 3 + 128];
	if (!build_token_post_fields(curl, post_fields, sizeof(post_fields), client->client_id,
				     client->client_secret)) {
		obs_log(LOG_WARNING, "subsplash: failed to build auth request body");
		curl_easy_cleanup(curl);
		pthread_mutex_unlock(&client->token_lock);
		return false;
	}

	struct curl_buf buf = {NULL, 0};

	struct curl_slist *headers = NULL;
	headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_POST, 1L);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_fields);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
	apply_common_curl_opts(curl, client);

	CURLcode curl_result = curl_easy_perform(curl);

	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	if (curl_result != CURLE_OK || !buf.data) {
		obs_log(LOG_WARNING, "subsplash: auth request failed: %s", curl_easy_strerror(curl_result));
		free(buf.data);
		pthread_mutex_unlock(&client->token_lock);
		return false;
	}

	obs_data_t *json = obs_data_create_from_json(buf.data);
	free(buf.data);

	if (!json) {
		obs_log(LOG_WARNING, "subsplash: failed to parse auth JSON response");
		pthread_mutex_unlock(&client->token_lock);
		return false;
	}

	const char *token = obs_data_get_string(json, "access_token");
	int expires_in = (int)obs_data_get_int(json, "expires_in");

	if (!token || token[0] == '\0') {
		obs_log(LOG_WARNING, "subsplash: no access_token in auth response");
		obs_data_release(json);
		pthread_mutex_unlock(&client->token_lock);
		return false;
	}

	snprintf(client->access_token, sizeof(client->access_token), "%s", token);
	client->token_expiry = time(NULL) + expires_in - 60;

	obs_log(LOG_INFO, "subsplash: authenticated, token expires in %ds", expires_in);

	obs_data_release(json);
	pthread_mutex_unlock(&client->token_lock);
	return true;
}

/* ------------------------------------------------------------------ */
/* subsplash_client_fetch_broadcasts                                  */
/*                                                                    */
/* Queries upcoming broadcasts via the upcoming filter.        */
/* Terminal statuses (ended, on-demand, never-happened) are skipped   */
/* so a just-finished broadcast doesn't shadow the next scheduled     */
/* event.                                                             */
/* ------------------------------------------------------------------ */

int subsplash_client_fetch_broadcasts(subsplash_client_t *client, subsplash_broadcast_t *out)
{
	if (!client || !out)
		return SUBSPLASH_FETCH_API_ERROR;

	memset(out, 0, sizeof(*out));
	out->valid = false;

	if (!subsplash_client_authenticate(client))
		return SUBSPLASH_FETCH_API_ERROR;

	CURL *escape_handle = curl_easy_init();
	if (!escape_handle)
		return SUBSPLASH_FETCH_API_ERROR;

	char *encoded_app_key = curl_easy_escape(escape_handle, client->app_key, 0);

	/*
	 * The upcoming filter hits a cached code path in server,
	 * avoiding a round-trip to the upstream API on most polls.
	 */
	char url[SUBSPLASH_MAX_URL + 512];
	snprintf(url, sizeof(url),
		 "%s/live/v1/broadcasts?"
		 "filter%%5Bapp_key%%5D=%s&"
		 "filter%%5Bupcoming%%5D=true&"
		 "page%%5Bsize%%5D=1",
		 client->base_url, encoded_app_key ? encoded_app_key : client->app_key);

	curl_free(encoded_app_key);
	curl_easy_cleanup(escape_handle);

	struct curl_buf response = {NULL, 0};
	long http_code = 0;
	CURLcode curl_result = perform_authenticated_get(client, url, &response, &http_code);

	if (curl_result != CURLE_OK || !response.data) {
		obs_log(LOG_WARNING, "subsplash: fetch broadcasts failed: %s", curl_easy_strerror(curl_result));
		free(response.data);
		return SUBSPLASH_FETCH_API_ERROR;
	}

	if (http_code == 401) {
		obs_log(LOG_WARNING, "subsplash: broadcasts request returned HTTP 401 (token rejected)");
		pthread_mutex_lock(&client->token_lock);
		client->access_token[0] = '\0';
		client->token_expiry = 0;
		pthread_mutex_unlock(&client->token_lock);
		free(response.data);
		return SUBSPLASH_FETCH_AUTH_ERROR;
	}

	if (http_code == 403) {
		obs_log(LOG_WARNING, "subsplash: broadcasts request returned HTTP 403 (not authorized for app key)");
		free(response.data);
		return SUBSPLASH_FETCH_AUTH_ERROR;
	}

	if (http_code < 200 || http_code >= 300) {
		obs_log(LOG_WARNING, "subsplash: broadcasts request returned HTTP %ld", http_code);
		free(response.data);
		return SUBSPLASH_FETCH_API_ERROR;
	}

	obs_data_t *json = obs_data_create_from_json(response.data);
	free(response.data);

	if (!json) {
		obs_log(LOG_WARNING, "subsplash: failed to parse broadcasts JSON");
		return SUBSPLASH_FETCH_API_ERROR;
	}

	obs_data_t *embedded = obs_data_get_obj(json, "_embedded");
	if (!embedded) {
		obs_log(LOG_INFO, "subsplash: broadcasts response contained no results");
		obs_data_release(json);
		return SUBSPLASH_FETCH_NO_DATA;
	}

	obs_data_array_t *broadcasts = obs_data_get_array(embedded, "broadcasts");
	size_t broadcast_count = broadcasts ? obs_data_array_count(broadcasts) : 0;

	if (broadcast_count == 0) {
		obs_log(LOG_INFO, "subsplash: no active broadcasts found");
		if (broadcasts)
			obs_data_array_release(broadcasts);
		obs_data_release(embedded);
		obs_data_release(json);
		return SUBSPLASH_FETCH_NO_DATA;
	}

	/*
	 * Results arrive sorted by start time (ascending). Skip
	 * terminal broadcasts (ended/on-demand/never-happened) so we
	 * pick up the next actionable event even when a just-finished
	 * broadcast is still in the time window.
	 */
	obs_data_t *chosen = NULL;
	for (size_t i = 0; i < broadcast_count; i++) {
		obs_data_t *item = obs_data_array_item(broadcasts, i);
		const char *status = obs_data_get_string(item, "status");

		bool terminal = strcmp(status, "ended") == 0 || strcmp(status, "on-demand") == 0 ||
				strcmp(status, "never-happened") == 0;

		if (!terminal) {
			chosen = item;
			break;
		}
		obs_data_release(item);
	}

	obs_data_array_release(broadcasts);
	obs_data_release(embedded);

	if (!chosen) {
		obs_data_release(json);
		return SUBSPLASH_FETCH_NO_DATA;
	}

	parse_broadcast_json(chosen, out);
	obs_data_release(chosen);
	obs_data_release(json);

	obs_log(LOG_INFO, "subsplash: broadcast id=%s start=%s end=%s status=%s simulated_live=%s", out->id,
		out->start_at, out->end_at, out->status, out->simulated_live ? "true" : "false");

	return SUBSPLASH_FETCH_OK;
}

/* ------------------------------------------------------------------ */
/* subsplash_client_fetch_by_id                                       */
/* ------------------------------------------------------------------ */

int subsplash_client_fetch_by_id(subsplash_client_t *client, const char *id, subsplash_broadcast_t *out)
{
	if (!client || !id || !out)
		return SUBSPLASH_FETCH_API_ERROR;

	memset(out, 0, sizeof(*out));
	out->valid = false;

	if (!subsplash_client_authenticate(client))
		return SUBSPLASH_FETCH_API_ERROR;

	char url[SUBSPLASH_MAX_URL + SUBSPLASH_MAX_ID + 32];
	snprintf(url, sizeof(url), "%s/live/v1/broadcasts/%s", client->base_url, id);

	struct curl_buf response = {NULL, 0};
	long http_code = 0;
	CURLcode curl_result = perform_authenticated_get(client, url, &response, &http_code);

	if (curl_result != CURLE_OK || !response.data) {
		obs_log(LOG_WARNING, "subsplash: fetch broadcast %s failed: %s", id, curl_easy_strerror(curl_result));
		free(response.data);
		return SUBSPLASH_FETCH_API_ERROR;
	}

	if (http_code == 401) {
		obs_log(LOG_WARNING, "subsplash: broadcast %s returned HTTP 401 (token rejected)", id);
		pthread_mutex_lock(&client->token_lock);
		client->access_token[0] = '\0';
		client->token_expiry = 0;
		pthread_mutex_unlock(&client->token_lock);
		free(response.data);
		return SUBSPLASH_FETCH_AUTH_ERROR;
	}

	if (http_code == 403) {
		obs_log(LOG_WARNING, "subsplash: broadcast %s returned HTTP 403 (not authorized for app key)", id);
		free(response.data);
		return SUBSPLASH_FETCH_AUTH_ERROR;
	}

	if (http_code == 404) {
		obs_log(LOG_INFO, "subsplash: broadcast %s no longer exists (HTTP 404)", id);
		free(response.data);
		return SUBSPLASH_FETCH_NOT_FOUND;
	}

	if (http_code < 200 || http_code >= 300) {
		obs_log(LOG_WARNING, "subsplash: broadcast %s request returned HTTP %ld", id, http_code);
		free(response.data);
		return SUBSPLASH_FETCH_API_ERROR;
	}

	obs_data_t *json = obs_data_create_from_json(response.data);
	free(response.data);

	if (!json) {
		obs_log(LOG_WARNING, "subsplash: failed to parse broadcast %s JSON", id);
		return SUBSPLASH_FETCH_API_ERROR;
	}

	parse_broadcast_json(json, out);
	obs_data_release(json);

	if (!out->valid)
		return SUBSPLASH_FETCH_NO_DATA;

	obs_log(LOG_INFO, "subsplash: fetched broadcast id=%s status=%s end=%s", out->id, out->status, out->end_at);

	return SUBSPLASH_FETCH_OK;
}

/* ------------------------------------------------------------------ */
/* subsplash_client_test_connection                                   */
/* ------------------------------------------------------------------ */

bool subsplash_client_test_connection(subsplash_client_t *client, char *status_out, size_t status_len)
{
	if (!client || !status_out || status_len == 0)
		return false;

	if (!subsplash_client_authenticate(client)) {
		snprintf(status_out, status_len, "Sign-in failed — check your Client ID and Secret");
		return false;
	}

	subsplash_broadcast_t broadcast;
	int result = subsplash_client_fetch_broadcasts(client, &broadcast);

	if (result == SUBSPLASH_FETCH_AUTH_ERROR) {
		snprintf(status_out, status_len,
			 "Signed in, but not authorized for this App Key — double-check your App Key");
		return false;
	}

	/* Connectivity only: report that the creds reach the API. The upcoming
	 * broadcast has its own status row, so don't imply pending action here
	 * (especially while sync is stopped). */
	snprintf(status_out, status_len, "Connected");
	return true;
}
