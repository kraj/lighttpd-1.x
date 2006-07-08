%token_prefix TK_
%token_type {buffer *}
%extra_argument {http_resp_ctx_t *ctx}
%name http_resp_parser

%include {
#include <assert.h>
#include <string.h>
#include "http_resp.h"
#include "keyvalue.h"
#include "array.h"
}

%parse_failure {
  ctx->ok = 0;
}

%type protocol { int }
%type response_hdr { http_resp * }
%type number { int }
%type headers { array * }
%type header { data_string * }
%token_destructor { buffer_free($$); }

response_hdr ::= protocol(B) number(C) reason(D) CRLF headers(HDR) CRLF . {
    http_resp *resp = ctx->resp;
    
    resp->status = C;
    resp->protocol = B;
    buffer_copy_string_buffer(resp->reason, D);
    
    array_free(resp->headers);
    
    resp->headers = HDR;
    
    HDR = NULL;
}

protocol(A) ::= STRING(B). {
    if (buffer_is_equal_string(B, CONST_STR_LEN("HTTP/1.0"))) {
        A = HTTP_VERSION_1_0;
    } else if (buffer_is_equal_string(B, CONST_STR_LEN("HTTP/1.1"))) {
        A = HTTP_VERSION_1_1;
    } else {
        buffer_copy_string(ctx->errmsg, "unknown protocol: ");
        buffer_append_string_buffer(ctx->errmsg, B);
        
        ctx->ok = 0;
    }
}

number(A) ::= STRING(B). {
    char *err;
    A = strtol(B->ptr, &err, 10);
    
    if (*err != '\0') {
        buffer_copy_string(ctx->errmsg, "expected a number: ");
        buffer_append_string_buffer(ctx->errmsg, B);
        
        ctx->ok = 0;
    }
}

reason(A) ::= STRING(B). {
    buffer_copy_string_buffer(A, B);
}

reason(A) ::= reason(C) STRING(B). {
    A = C;
    
    buffer_append_string(A, " ");
    buffer_append_string_buffer(A, B);
    
    C = NULL;
}

headers(HDRS) ::= headers(SRC) header(HDR). {
    HDRS = SRC;
    
    array_insert_unique(HDRS, (data_unset *)HDR);
    
    SRC = NULL;
}

headers(HDRS) ::= header(HDR). {
    HDRS = array_init();

    array_insert_unique(HDRS, (data_unset *)HDR);
}
header(HDR) ::= STRING(A) COLON STRING(B) CRLF. {
    HDR = data_string_init();
    
    buffer_copy_string_buffer(HDR->key, A);
    buffer_copy_string_buffer(HDR->value, B);    
}
