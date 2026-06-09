/*
 * libcurl stubs for unit tests.
 *
 * Every function is a no-op or returns a safe default.  Tests that
 * exercise code which calls libcurl (e.g. subsplash_client_authenticate)
 * will get CURLE_OK from curl_easy_perform without making real HTTP
 * requests.
 */
#include "curl/curl.h"

#include <stdlib.h>
#include <string.h>

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
	(void)option;
	return CURLE_OK;
}

CURLcode curl_easy_perform(CURL *curl)
{
	(void)curl;
	return CURLE_OK;
}

char *curl_easy_escape(CURL *curl, const char *string, int length)
{
	(void)curl;
	(void)length;
	return string ? strdup(string) : NULL;
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

struct curl_slist *curl_slist_append(struct curl_slist *list,
				     const char *string)
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
