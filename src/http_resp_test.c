#include <stdio.h>
#include <string.h>
#include <tap.h>

#include "http_resp.h"
#include "log.h"

int main(void) {
	http_resp *resp = http_response_init();
	chunkqueue *cq = chunkqueue_init();
	buffer *b, *content = buffer_init();

	log_init();
	plan_tests(11);

	/* basic response header + CRLF */
	b = chunkqueue_get_append_buffer(cq);

	buffer_copy_string(b,
		"HTTP/1.0 304 Not Modified\r\n"
// 		"HTTP/1.0 304 Not Modified\r\n"
//  		"Location: foobar\r\n"
// 		"Content-Lenght: 24\r\n"
// 		"\r\nABC"
		"\r\nABC"
	);

	ok(PARSE_SUCCESS == http_response_parse_cq(cq, resp), "good 304 response with CRLF");

	chunkqueue_remove_finished_chunks(cq);
	buffer_reset(content);
	chunkqueue_to_buffer(cq, content);
	ok(0 == strcmp("ABC", SAFE_BUF_STR(content)), "content is ABC, got %s", BUF_STR(content));

	http_response_free(resp);

	/* line-wrapping */

	chunkqueue_reset(cq);
	resp = http_response_init();

	b = chunkqueue_get_append_buffer(cq);
	buffer_copy_string(b,
		"HTTP/1.0 304 Not Modified\n"
		"Location: foobar\n"
		"Content-Lenght: 24\n"
		"\nABC"
	);

	ok(PARSE_SUCCESS == http_response_parse_cq(cq, resp), "good response with LF");

	chunkqueue_remove_finished_chunks(cq);
	buffer_reset(content);
	chunkqueue_to_buffer(cq, content);
	ok(0 == strcmp("ABC", BUF_STR(content)), "content is ABC, got %s", BUF_STR(content));

	http_response_free(resp);

	/* NPH */

	chunkqueue_reset(cq);
	resp = http_response_init();

	chunkqueue_append_mem(cq, CONST_STR_LEN(
		"HTTP/1.0 304 Not Modified\n"
		"\nABC"
	));

	ok(PARSE_SUCCESS == http_response_parse_cq(cq, resp), "good response with NPH");

	chunkqueue_remove_finished_chunks(cq);
	buffer_reset(content);
	chunkqueue_to_buffer(cq, content);
	ok(0 == strcmp("ABC", BUF_STR(content)), "content is ABC, got %s", BUF_STR(content));

	http_response_free(resp);

	/* no request line */

	chunkqueue_reset(cq);
	resp = http_response_init();

	b = chunkqueue_get_append_buffer(cq);

	buffer_copy_string(b,
		"Status: 200 Foobar\r\n"
		"Location: foobar\r\n"
		"Content-Lenght: 24\r\n"
		"\r\nABC"
	);

	ok(PARSE_SUCCESS == http_response_parse_cq(cq, resp), "Status: 200 ...");

	http_response_free(resp);

	/* LF as line-ending */

	chunkqueue_reset(cq);
	resp = http_response_init();

	b = chunkqueue_get_append_buffer(cq);

	buffer_copy_string(b,
		"Location: foobar\n"
		"\nABC"
	);
	ok(PARSE_SUCCESS == http_response_parse_cq(cq, resp), "no Status at all");

	chunkqueue_remove_finished_chunks(cq);
	buffer_reset(content);
	chunkqueue_to_buffer(cq, content);
	ok(0 == strcmp("ABC", BUF_STR(content)), "content is ABC, got %s", BUF_STR(content));

	/* LF as line-ending */

	chunkqueue_reset(cq);
	resp = http_response_init();

	b = chunkqueue_get_append_buffer(cq);

	buffer_copy_string(b,
		"HTTP");

	b = chunkqueue_get_append_buffer(cq);
	buffer_copy_string_len(b, CONST_STR_LEN("/1.0 "));
	b = chunkqueue_get_append_buffer(cq);
	buffer_copy_string_len(b, CONST_STR_LEN("30"));
	b = chunkqueue_get_append_buffer(cq);
	buffer_copy_string_len(b, CONST_STR_LEN("4 Not Modified\r"));

	b = chunkqueue_get_append_buffer(cq);
	buffer_copy_string(b, "\n"
		"Locati");

	b = chunkqueue_get_append_buffer(cq);
	buffer_copy_string(b, "on: foobar\r\n"
		"Content-Lenght: 24\r\n"
		"\r\nABC"
	);
	ok(PARSE_SUCCESS == http_response_parse_cq(cq, resp), "chunked response");

	chunkqueue_remove_finished_chunks(cq);
	buffer_reset(content);
	chunkqueue_to_buffer(cq, content);
	ok(0 == strcmp("ABC", BUF_STR(content)), "content is ABC, got %s", BUF_STR(content));

	http_response_free(resp);
	chunkqueue_free(cq);
	buffer_free(content);
	log_free();

	return exit_status();
}
