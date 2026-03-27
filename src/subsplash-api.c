#include "subsplash-api.h"
#include <curl/curl.h>
#include <obs-module.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "plugin-support.h"

/* ------------------------------------------------------------------ */
/* curl write-callback: appends received data to a growable buffer    */
/* ------------------------------------------------------------------ */

struct curl_buf {
	char *data;
	size_t size;
};

static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb,
			    void *userdata)
{
	size_t bytes = size * nmemb;
	struct curl_buf *buf = (struct curl_buf *)userdata;

	char *tmp = realloc(buf->data, buf->size + bytes + 1);
	if (!tmp)
		return 0;

	buf->data = tmp;
	memcpy(buf->data + buf->size, ptr, bytes);
	buf->size += bytes;
	buf->data[buf->size] = '\0';
	return bytes;
}

/* ------------------------------------------------------------------ */
/* ISO-8601 helper: "2026-03-29T10:00:00Z" -> time_t (UTC)           */
/* ------------------------------------------------------------------ */

static time_t parse_iso8601(const char *str)
{
	int y, mo, d, h, mi, s;
	if (sscanf(str, "%d-%d-%dT%d:%d:%dZ", &y, &mo, &d, &h, &mi, &s) !=
	    6)
		return (time_t)-1;

	struct tm tm_val;
	memset(&tm_val, 0, sizeof(tm_val));
	tm_val.tm_year = y - 1900;
	tm_val.tm_mon = mo - 1;
	tm_val.tm_mday = d;
	tm_val.tm_hour = h;
	tm_val.tm_min = mi;
	tm_val.tm_sec = s;
	tm_val.tm_isdst = 0;

#if defined(_WIN32)
	time_t result = _mkgmtime(&tm_val);
#elif defined(__APPLE__) || defined(__linux__)
	time_t result = timegm(&tm_val);
#else
	/* Portable fallback: temporarily override TZ */
	char *old_tz = getenv("TZ");
	char *saved_tz = old_tz ? strdup(old_tz) : NULL;
	setenv("TZ", "UTC", 1);
	tzset();
	time_t result = mktime(&tm_val);
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
/* subsplash_client_init                                              */
/* ------------------------------------------------------------------ */

bool subsplash_client_init(subsplash_client_t *client, const char *base_url,
			   const char *client_id, const char *client_secret,
			   const char *app_key)
{
	if (!client || !base_url || !client_id || !client_secret || !app_key)
		return false;

	memset(client, 0, sizeof(*client));

	snprintf(client->base_url, sizeof(client->base_url), "%s", base_url);
	snprintf(client->client_id, sizeof(client->client_id), "%s",
		 client_id);
	snprintf(client->client_secret, sizeof(client->client_secret), "%s",
		 client_secret);
	snprintf(client->app_key, sizeof(client->app_key), "%s", app_key);

	pthread_mutex_init(&client->token_lock, NULL);
	client->initialized = true;
	return true;
}

/* ------------------------------------------------------------------ */
/* subsplash_client_destroy                                           */
/* ------------------------------------------------------------------ */

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

	if (client->access_token[0] != '\0' &&
	    time(NULL) < client->token_expiry) {
		pthread_mutex_unlock(&client->token_lock);
		return true;
	}

	char url[SUBSPLASH_MAX_URL + 64];
	snprintf(url, sizeof(url), "%s/tokens/v1/token", client->base_url);

	char post_fields[SUBSPLASH_MAX_FIELD * 3 + 128];
	snprintf(post_fields, sizeof(post_fields),
		 "client_id=%s&client_secret=%s&grant_type=client_credentials",
		 client->client_id, client->client_secret);

	CURL *curl = curl_easy_init();
	if (!curl) {
		obs_log(LOG_WARNING,
			"subsplash: curl_easy_init failed for auth");
		pthread_mutex_unlock(&client->token_lock);
		return false;
	}

	struct curl_buf buf = {NULL, 0};

	struct curl_slist *headers = NULL;
	headers = curl_slist_append(
		headers, "Content-Type: application/x-www-form-urlencoded");

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_POST, 1L);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_fields);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

	CURLcode res = curl_easy_perform(curl);

	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	if (res != CURLE_OK || !buf.data) {
		obs_log(LOG_WARNING, "subsplash: auth request failed: %s",
			curl_easy_strerror(res));
		free(buf.data);
		pthread_mutex_unlock(&client->token_lock);
		return false;
	}

	obs_data_t *json = obs_data_create_from_json(buf.data);
	free(buf.data);

	if (!json) {
		obs_log(LOG_WARNING,
			"subsplash: failed to parse auth JSON response");
		pthread_mutex_unlock(&client->token_lock);
		return false;
	}

	const char *token = obs_data_get_string(json, "access_token");
	int expires_in = (int)obs_data_get_int(json, "expires_in");

	if (!token || token[0] == '\0') {
		obs_log(LOG_WARNING,
			"subsplash: no access_token in auth response");
		obs_data_release(json);
		pthread_mutex_unlock(&client->token_lock);
		return false;
	}

	snprintf(client->access_token, sizeof(client->access_token), "%s",
		 token);
	client->token_expiry = time(NULL) + expires_in - 60;

	obs_log(LOG_INFO, "subsplash: authenticated, token expires in %ds",
		expires_in);

	obs_data_release(json);
	pthread_mutex_unlock(&client->token_lock);
	return true;
}

/* ------------------------------------------------------------------ */
/* subsplash_client_fetch_upcoming                                    */
/* ------------------------------------------------------------------ */

bool subsplash_client_fetch_upcoming(subsplash_client_t *client,
				     subsplash_broadcast_t *out)
{
	if (!client || !out)
		return false;

	memset(out, 0, sizeof(*out));
	out->valid = false;

	if (!subsplash_client_authenticate(client))
		return false;

	CURL *curl = curl_easy_init();
	if (!curl) {
		obs_log(LOG_WARNING,
			"subsplash: curl_easy_init failed for fetch");
		return false;
	}

	char *enc_app_key = curl_easy_escape(curl, client->app_key, 0);

	char url[SUBSPLASH_MAX_URL + 256];
	snprintf(url, sizeof(url),
		 "%s/live/v1/broadcasts?"
		 "filter%%5Bapp_key%%5D=%s&"
		 "filter%%5Bupcoming%%5D=true&"
		 "sort=start_at&limit=5",
		 client->base_url, enc_app_key ? enc_app_key : client->app_key);

	curl_free(enc_app_key);

	char auth_header[SUBSPLASH_MAX_TOKEN + 32];
	snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s",
		 client->access_token);

	struct curl_slist *headers = NULL;
	headers = curl_slist_append(headers, auth_header);

	struct curl_buf buf = {NULL, 0};

	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

	CURLcode res = curl_easy_perform(curl);

	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	if (res != CURLE_OK || !buf.data) {
		obs_log(LOG_WARNING, "subsplash: fetch broadcasts failed: %s",
			curl_easy_strerror(res));
		free(buf.data);
		return false;
	}

	obs_data_t *json = obs_data_create_from_json(buf.data);
	free(buf.data);

	if (!json) {
		obs_log(LOG_WARNING,
			"subsplash: failed to parse broadcasts JSON");
		return false;
	}

	obs_data_t *embedded = obs_data_get_obj(json, "_embedded");
	if (!embedded) {
		obs_log(LOG_INFO,
			"subsplash: no _embedded object in response");
		obs_data_release(json);
		return false;
	}

	obs_data_array_t *broadcasts =
		obs_data_get_array(embedded, "broadcasts");
	if (!broadcasts || obs_data_array_count(broadcasts) == 0) {
		obs_log(LOG_INFO, "subsplash: no upcoming broadcasts found");
		if (broadcasts)
			obs_data_array_release(broadcasts);
		obs_data_release(embedded);
		obs_data_release(json);
		return false;
	}

	obs_data_t *first = obs_data_array_item(broadcasts, 0);
	if (!first) {
		obs_data_array_release(broadcasts);
		obs_data_release(embedded);
		obs_data_release(json);
		return false;
	}

	const char *id = obs_data_get_string(first, "id");
	const char *start_at = obs_data_get_string(first, "start_at");
	const char *end_at = obs_data_get_string(first, "end_at");
	const char *status = obs_data_get_string(first, "status");

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
	out->simulated_live = obs_data_get_bool(first, "simulated_live");
	out->valid = true;

	obs_log(LOG_INFO,
		"subsplash: next broadcast id=%s start=%s status=%s simulated_live=%s",
		out->id, out->start_at, out->status,
		out->simulated_live ? "true" : "false");

	obs_data_release(first);
	obs_data_array_release(broadcasts);
	obs_data_release(embedded);
	obs_data_release(json);

	return true;
}

/* ------------------------------------------------------------------ */
/* subsplash_client_test_connection                                   */
/* ------------------------------------------------------------------ */

bool subsplash_client_test_connection(subsplash_client_t *client,
				      char *status_out, size_t status_len)
{
	if (!client || !status_out || status_len == 0)
		return false;

	if (!subsplash_client_authenticate(client)) {
		snprintf(status_out, status_len, "Authentication failed");
		return false;
	}

	subsplash_broadcast_t bc;
	if (subsplash_client_fetch_upcoming(client, &bc) && bc.valid) {
		snprintf(status_out, status_len,
			 "Connected. Next broadcast: %s", bc.start_at);
	} else {
		snprintf(status_out, status_len,
			 "Connected. No upcoming broadcasts.");
	}

	return true;
}
