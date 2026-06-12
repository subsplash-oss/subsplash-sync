/*
 * libcurl stubs for unit tests.
 *
 * Every function is a no-op or returns a safe default.  Tests call
 * stub_enqueue_response() to queue up canned HTTP replies; each
 * curl_easy_perform() pops the next one and feeds it to the write
 * callback that subsplash-api.c registered via CURLOPT_WRITEFUNCTION.
 */
#include "curl/curl.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

typedef size_t (*curl_write_callback)(void *, size_t, size_t, void *);

/* Captured write callback set via CURLOPT_WRITEFUNCTION / WRITEDATA. */
static curl_write_callback g_write_cb = NULL;
static void *g_write_data = NULL;

/* Canned response queue. */
static struct {
	long http_code;
	const char *body;
} g_responses[STUB_MAX_RESPONSES];

static int g_response_count = 0;
static int g_response_index = 0;
static long g_last_http_code = 200;

void stub_reset(void)
{
	g_response_count = 0;
	g_response_index = 0;
	g_write_cb = NULL;
	g_write_data = NULL;
	g_last_http_code = 200;
}

void stub_enqueue_response(long http_code, const char *body)
{
	if (g_response_count < STUB_MAX_RESPONSES) {
		g_responses[g_response_count].http_code = http_code;
		g_responses[g_response_count].body = body;
		g_response_count++;
	}
}

/* ------------------------------------------------------------------ */
/* Standard curl_easy_* stubs                                         */
/* ------------------------------------------------------------------ */

static char dummy_curl;

CURL *curl_easy_init(void)
{
	return &dummy_curl;
}

void curl_easy_cleanup(CURL *curl)
{
	(void)curl;
}

CURLcode curl_easy_setopt(CURL *curl, CURLoption option, ...)
{
	(void)curl;
	va_list ap;
	va_start(ap, option);
	if (option == CURLOPT_WRITEFUNCTION)
		g_write_cb = va_arg(ap, curl_write_callback);
	else if (option == CURLOPT_WRITEDATA)
		g_write_data = va_arg(ap, void *);
	va_end(ap);
	return CURLE_OK;
}

CURLcode curl_easy_perform(CURL *curl)
{
	(void)curl;
	if (g_response_index < g_response_count) {
		long code = g_responses[g_response_index].http_code;
		const char *body = g_responses[g_response_index].body;
		g_response_index++;
		g_last_http_code = code;

		if (g_write_cb && body && body[0] != '\0')
			g_write_cb((void *)body, 1, strlen(body), g_write_data);
	}
	return CURLE_OK;
}

CURLcode curl_easy_getinfo(CURL *curl, CURLINFO info, ...)
{
	(void)curl;
	if (info == CURLINFO_RESPONSE_CODE) {
		va_list ap;
		va_start(ap, info);
		long *out = va_arg(ap, long *);
		if (out)
			*out = g_last_http_code;
		va_end(ap);
	}
	return CURLE_OK;
}

/*
 * Real RFC 3986 percent-encoding so tests can verify that reserved
 * characters in credentials are escaped. Mirrors libcurl: only the
 * unreserved set (ALPHA / DIGIT / - _ . ~) is left untouched.
 */
char *curl_easy_escape(CURL *curl, const char *string, int length)
{
	(void)curl;
	if (!string)
		return NULL;

	size_t len = length > 0 ? (size_t)length : strlen(string);
	char *out = malloc(len * 3 + 1);
	if (!out)
		return NULL;

	static const char hex[] = "0123456789ABCDEF";
	size_t o = 0;
	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)string[i];
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
		    c == '_' || c == '.' || c == '~') {
			out[o++] = (char)c;
		} else {
			out[o++] = '%';
			out[o++] = hex[c >> 4];
			out[o++] = hex[c & 0x0F];
		}
	}
	out[o] = '\0';
	return out;
}

const char *curl_easy_strerror(CURLcode code)
{
	(void)code;
	return "stub error";
}

void curl_free(void *p)
{
	free(p);
}

struct curl_slist *curl_slist_append(struct curl_slist *list, const char *string)
{
	struct curl_slist *node = calloc(1, sizeof(*node));
	node->data = strdup(string);
	node->next = list;
	return node;
}

void curl_slist_free_all(struct curl_slist *list)
{
	while (list) {
		struct curl_slist *next = list->next;
		free(list->data);
		free(list);
		list = next;
	}
}
