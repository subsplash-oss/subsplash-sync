/*
 * Unit tests for subsplash-api.c
 *
 * Static functions are accessed by #include-ing the source file.
 * OBS and curl dependencies are replaced with stubs (obs-stubs.c,
 * curl-stubs.c) linked at compile time.
 */
#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * Include the source file directly to expose static functions.
 * Headers are intercepted by tests/stubs/ in the include path.
 */
#include "subsplash-api.c"

/* ================================================================== */
/* parse_iso8601 tests                                                */
/* ================================================================== */

static void test_parse_iso8601_standard(void **state)
{
	(void)state;
	time_t t = parse_iso8601("2026-06-08T15:30:00Z");
	assert_true(t != (time_t)-1);

	struct tm utc;
	gmtime_r(&t, &utc);
	assert_int_equal(utc.tm_year + 1900, 2026);
	assert_int_equal(utc.tm_mon + 1, 6);
	assert_int_equal(utc.tm_mday, 8);
	assert_int_equal(utc.tm_hour, 15);
	assert_int_equal(utc.tm_min, 30);
	assert_int_equal(utc.tm_sec, 0);
}

static void test_parse_iso8601_midnight(void **state)
{
	(void)state;
	time_t t = parse_iso8601("2026-01-01T00:00:00Z");
	assert_true(t != (time_t)-1);

	struct tm utc;
	gmtime_r(&t, &utc);
	assert_int_equal(utc.tm_year + 1900, 2026);
	assert_int_equal(utc.tm_mon + 1, 1);
	assert_int_equal(utc.tm_mday, 1);
	assert_int_equal(utc.tm_hour, 0);
	assert_int_equal(utc.tm_min, 0);
	assert_int_equal(utc.tm_sec, 0);
}

static void test_parse_iso8601_epoch(void **state)
{
	(void)state;
	time_t t = parse_iso8601("1970-01-01T00:00:00Z");
	assert_int_equal(t, 0);
}

static void test_parse_iso8601_malformed(void **state)
{
	(void)state;
	assert_int_equal(parse_iso8601("not-a-date"), (time_t)-1);
	assert_int_equal(parse_iso8601("2026-06-08"), (time_t)-1);
	assert_int_equal(parse_iso8601("2026-06-08T15:30"), (time_t)-1);
}

static void test_parse_iso8601_empty(void **state)
{
	(void)state;
	assert_int_equal(parse_iso8601(""), (time_t)-1);
}

/* ================================================================== */
/* curl_write_cb tests                                                */
/* ================================================================== */

static void test_curl_write_cb_basic(void **state)
{
	(void)state;
	struct curl_buf buf = {NULL, 0};

	const char *data = "hello";
	size_t written = curl_write_cb((void *)data, 1, 5, &buf);
	assert_int_equal(written, 5);
	assert_non_null(buf.data);
	assert_string_equal(buf.data, "hello");
	assert_int_equal(buf.size, 5);

	free(buf.data);
}

static void test_curl_write_cb_multiple(void **state)
{
	(void)state;
	struct curl_buf buf = {NULL, 0};

	curl_write_cb((void *)"abc", 1, 3, &buf);
	curl_write_cb((void *)"def", 1, 3, &buf);
	assert_int_equal(buf.size, 6);
	assert_string_equal(buf.data, "abcdef");

	free(buf.data);
}

static void test_curl_write_cb_empty(void **state)
{
	(void)state;
	struct curl_buf buf = {NULL, 0};

	size_t written = curl_write_cb((void *)"", 1, 0, &buf);
	assert_int_equal(written, 0);

	free(buf.data);
}

/* ================================================================== */
/* parse_broadcast_json tests                                         */
/* ================================================================== */

static void test_parse_broadcast_json_full(void **state)
{
	(void)state;
	const char *json =
		"{"
		"  \"id\": \"abc-123\","
		"  \"start_at\": \"2026-06-08T10:00:00Z\","
		"  \"end_at\": \"2026-06-08T11:00:00Z\","
		"  \"status\": \"scheduled\","
		"  \"simulated_live\": false"
		"}";

	obs_data_t *obj = obs_data_create_from_json(json);
	assert_non_null(obj);

	subsplash_broadcast_t b;
	memset(&b, 0, sizeof(b));
	parse_broadcast_json(obj, &b);

	assert_string_equal(b.id, "abc-123");
	assert_string_equal(b.start_at, "2026-06-08T10:00:00Z");
	assert_string_equal(b.end_at, "2026-06-08T11:00:00Z");
	assert_string_equal(b.status, "scheduled");
	assert_false(b.simulated_live);
	assert_true(b.valid);
	assert_true(b.start_epoch != (time_t)-1);
	assert_true(b.end_epoch != (time_t)-1);
	assert_true(b.end_epoch > b.start_epoch);

	obs_data_release(obj);
}

static void test_parse_broadcast_json_simulated(void **state)
{
	(void)state;
	const char *json =
		"{"
		"  \"id\": \"sim-1\","
		"  \"start_at\": \"2026-06-08T10:00:00Z\","
		"  \"end_at\": \"2026-06-08T11:00:00Z\","
		"  \"status\": \"live\","
		"  \"simulated_live\": true"
		"}";

	obs_data_t *obj = obs_data_create_from_json(json);
	assert_non_null(obj);

	subsplash_broadcast_t b;
	memset(&b, 0, sizeof(b));
	parse_broadcast_json(obj, &b);

	assert_true(b.simulated_live);
	assert_string_equal(b.status, "live");

	obs_data_release(obj);
}

static void test_parse_broadcast_json_missing_fields(void **state)
{
	(void)state;
	const char *json = "{ \"id\": \"partial\" }";

	obs_data_t *obj = obs_data_create_from_json(json);
	assert_non_null(obj);

	subsplash_broadcast_t b;
	memset(&b, 0, sizeof(b));
	parse_broadcast_json(obj, &b);

	assert_string_equal(b.id, "partial");
	assert_true(b.valid);
	/* Missing timestamps produce -1 from parse_iso8601. */
	assert_int_equal(b.start_epoch, (time_t)-1);
	assert_int_equal(b.end_epoch, (time_t)-1);

	obs_data_release(obj);
}

/* ================================================================== */
/* main                                                               */
/* ================================================================== */

int main(void)
{
	const struct CMUnitTest tests[] = {
		/* ISO-8601 parsing */
		cmocka_unit_test(test_parse_iso8601_standard),
		cmocka_unit_test(test_parse_iso8601_midnight),
		cmocka_unit_test(test_parse_iso8601_epoch),
		cmocka_unit_test(test_parse_iso8601_malformed),
		cmocka_unit_test(test_parse_iso8601_empty),
		/* curl write callback */
		cmocka_unit_test(test_curl_write_cb_basic),
		cmocka_unit_test(test_curl_write_cb_multiple),
		cmocka_unit_test(test_curl_write_cb_empty),
		/* broadcast JSON parsing */
		cmocka_unit_test(test_parse_broadcast_json_full),
		cmocka_unit_test(test_parse_broadcast_json_simulated),
		cmocka_unit_test(test_parse_broadcast_json_missing_fields),
	};

	return cmocka_run_group_tests(tests, NULL, NULL);
}
