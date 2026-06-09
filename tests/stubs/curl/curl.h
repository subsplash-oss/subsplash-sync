/*
 * Minimal fake <curl/curl.h> for unit tests.
 *
 * Provides the types and function signatures that subsplash-api.c
 * references so the file can compile without linking libcurl.
 */
#pragma once

#include <stddef.h>

typedef void CURL;

typedef enum {
	CURLE_OK = 0,
	CURLE_FAILED_INIT = 2,
	CURLE_COULDNT_CONNECT = 7,
} CURLcode;

typedef enum {
	CURLOPT_URL = 10002,
	CURLOPT_HTTPGET = 80,
	CURLOPT_HTTPHEADER = 10023,
	CURLOPT_WRITEFUNCTION = 20011,
	CURLOPT_WRITEDATA = 10001,
	CURLOPT_TIMEOUT = 13,
	CURLOPT_POST = 47,
	CURLOPT_POSTFIELDS = 10015,
} CURLoption;

struct curl_slist {
	char *data;
	struct curl_slist *next;
};

CURL *curl_easy_init(void);
void curl_easy_cleanup(CURL *curl);
CURLcode curl_easy_setopt(CURL *curl, CURLoption option, ...);
CURLcode curl_easy_perform(CURL *curl);
char *curl_easy_escape(CURL *curl, const char *string, int length);
const char *curl_easy_strerror(CURLcode code);
void curl_free(void *p);
struct curl_slist *curl_slist_append(struct curl_slist *list,
				     const char *string);
void curl_slist_free_all(struct curl_slist *list);
