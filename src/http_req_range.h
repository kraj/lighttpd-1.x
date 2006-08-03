#ifndef _HTTP_REQ_RANGE_H_
#define _HTTP_REQ_RANGE_H_

#include "array.h"
#include "chunk.h"
#include "http_parser.h"

typedef struct _http_req_range {
	off_t start;
	off_t end;
	struct _http_req_range *next;
} http_req_range;

typedef struct {
	int     ok;
	buffer *errmsg;

	http_req_range *ranges;
} http_req_range_ctx_t;

http_req_range *http_request_range_init(void);
void http_request_range_free(http_req_range *range);
void http_request_range_reset(http_req_range *range);

parse_status_t http_request_range_parse(buffer *range_hdr, http_req_range *ranges);

#endif
