#include <stdio.h>
#include <assert.h>
#include <string.h>

#include <tap.h>

#include "http_req.h"
#include "http_req_parser.h"
#include "log.h"
#include "chunk_helper.h"

/* declare prototypes for the parser */
void *http_req_parserAlloc(void *(*mallocProc)(size_t));
void http_req_parserFree(void *p,  void (*freeProc)(void*));
void http_req_parserTrace(FILE *TraceFILE, char *zTracePrompt);
void http_req_parser(void *, int, buffer *, http_req_ctx_t *);

void startParser(http_req_ctx_t *context, void **pParser, buffer **token) {
	context->ok = 1;
	context->errmsg = buffer_init();
	context->req = http_request_init();;
	context->unused_buffers = buffer_pool_init();

	*pParser = http_req_parserAlloc( malloc );
	*token = buffer_init();
}

void endParser(http_req_ctx_t *context, void *pParser, buffer *token) {
	http_req_parserFree(pParser, free);

	http_request_free(context->req);

	buffer_pool_free(context->unused_buffers);
	buffer_free(token);
	buffer_free(context->errmsg);
}

void check_parser_empty_input() {
	http_req_ctx_t context;
	void *pParser = NULL;
	buffer *token;

	startParser(&context, &pParser, &token);

	http_req_parser(pParser, 0, token, &context);

	ok(context.ok == 0, "Parser should fail on empty input");

	endParser(&context, pParser, token);
}

void check_parser_unfinished_input() {
	http_req_ctx_t context;
	void *pParser = NULL;
	buffer *token;

	startParser(&context, &pParser, &token);

	buffer_copy_string_len(token, CONST_STR_LEN("GET"));
	http_req_parser(pParser, TK_STRING, token, &context);
	token = buffer_pool_get(context.unused_buffers);
	http_req_parser(pParser, 0, token, &context);

	ok(context.ok == 0, "Parser should fail on unfinished input");

	endParser(&context, pParser, token);
}

int main(void) {
	http_req *req;
	chunkqueue *cq;
	buffer *content;

	cq = chunkqueue_init();
	content = buffer_init();

	log_init();
	plan_tests(12);



	/* Test 1.1: basic request header + CRLF */
	req = http_request_init();
	chunkqueue_append_mem(cq, CONST_STR_LEN(
		"GET / HTTP/1.0\r\n"
		"Location: foobar\r\n"
		"Content-Lenght: 24\r\n"
		"\r\nABC"
	));

	ok(PARSE_SUCCESS == http_request_parse_cq(cq, req), "basic GET header");

	/* Test 1.2 */
	buffer_reset(content);
	chunkqueue_to_buffer(cq, content);
	ok(0 == strcmp("ABC", BUF_STR(content)), "content is ABC, got %s", BUF_STR(content));

	http_request_free(req);



	/* Test 2.1: line-wrapping */

	chunkqueue_reset(cq);
	req = http_request_init();

	chunkqueue_append_mem(cq, CONST_STR_LEN(
		"GET /server-status HTTP/1.0\r\n"
		"User-Agent: Wget/1.9.1\r\n"
		"Authorization: Digest username=\"jan\", realm=\"jan\", nonce=\"9a5428ccc05b086a08d918e73b01fc6f\",\r\n"
		"                uri=\"/server-status\", response=\"ea5f7d9a30b8b762f9610ccb87dea74f\"\r\n"
		"\r\n"
	));

	ok(PARSE_SUCCESS == http_request_parse_cq(cq, req), "POST request with line-wrapping");
	http_request_free(req);



	/* Test 3.1 no request line */

	chunkqueue_reset(cq);
	req = http_request_init();

	chunkqueue_append_mem(cq, CONST_STR_LEN(
		"Location: foobar\r\n"
		"Content-Length: 24\r\n"
		"\r\nABC"
	));

	ok(PARSE_ERROR == http_request_parse_cq(cq, req), "missing request-line");
	http_request_free(req);



	/* Test 4.1: LF as line-ending */

	chunkqueue_reset(cq);
	req = http_request_init();

	chunkqueue_append_mem(cq, CONST_STR_LEN(
		"GET / HTTP/1.0\n"
		"\nABC"
	));
	ok(PARSE_SUCCESS == http_request_parse_cq(cq, req), "no request key-value pairs");

	/* Test 4.2 */

	chunkqueue_remove_finished_chunks(cq);
	buffer_reset(content);
	chunkqueue_to_buffer(cq, content);
	ok(0 == strcmp("ABC", BUF_STR(content)), "content is ABC, got %s", BUF_STR(content));
	http_request_free(req);



	/* 5.1 LF as line-ending */

	chunkqueue_reset(cq);
	req = http_request_init();

	chunkqueue_append_mem(cq, CONST_STR_LEN("GE"));
	ok(PARSE_NEED_MORE == http_request_parse_cq(cq, req), "Partial request 1");

	/* 5.2 */

	chunkqueue_append_mem(cq, CONST_STR_LEN("T "));
	chunkqueue_append_mem(cq, CONST_STR_LEN("/foo"));
	chunkqueue_append_mem(cq, CONST_STR_LEN("bar HTTP/1.0\r"));
	chunkqueue_append_mem(cq, CONST_STR_LEN(
		"\n"
		"Locati"
	));
	ok(PARSE_NEED_MORE == http_request_parse_cq(cq, req), "Partial request 2");

	/* 5.3 */

	chunkqueue_append_mem(cq, CONST_STR_LEN(
		"on: foobar\r\n"
		"Content-Lenght: 24\r\n"
		"\r\nABC"
	));

	ok(PARSE_SUCCESS == http_request_parse_cq(cq, req), "POST request with line-wrapping");

	/* 5.4 */

	chunkqueue_remove_finished_chunks(cq);
	buffer_reset(content);
	chunkqueue_to_buffer(cq, content);
	ok(0 == strcmp("ABC", BUF_STR(content)), "content is ABC, got %s", BUF_STR(content));
	http_request_free(req);



	/* Test: 6 */
	todo_start("Fix Parser, fail on empty input");
	check_parser_empty_input();
	todo_end();
	check_parser_unfinished_input();

	chunkqueue_free(cq);
	buffer_free(content);
	log_free();

	return exit_status();
}
