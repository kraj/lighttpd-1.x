#ifndef _BUFFER_H_
#define _BUFFER_H_

#include <stdlib.h>
#include <sys/types.h>

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "settings.h"

typedef struct {
	char *ptr;
	
	size_t used;
	size_t size;
} buffer;

typedef struct {
	buffer **ptr;
	
	size_t used;
	size_t size;
} buffer_array;

typedef struct {
	char *ptr;
	
	size_t offset; /* input-pointer */
	
	size_t used;   /* output-pointer */
	size_t size;
} read_buffer;

buffer_array* buffer_array_init(void);
void buffer_array_free(buffer_array *b);
buffer *buffer_array_append_get_buffer(buffer_array *b);

buffer* buffer_init(void);
buffer* buffer_init_string(const char *str);
void buffer_free(buffer *b);
void buffer_reset(buffer *b);
	
int buffer_prepare_copy(buffer *b, size_t size);
int buffer_prepare_append(buffer *b, size_t size);

int buffer_copy_string(buffer *b, const char *s);
int buffer_copy_string_len(buffer *b, const char *s, size_t s_len);
int buffer_copy_string_buffer(buffer *b, const buffer *src);
int buffer_copy_string_hex(buffer *b, const char *in, size_t in_len);

int buffer_copy_long(buffer *b, long val);

int buffer_copy_memory(buffer *b, const char *s, size_t s_len);

int buffer_append_string(buffer *b, const char *s);
int buffer_append_string_len(buffer *b, const char *s, size_t s_len);
int buffer_append_string_buffer(buffer *b, const buffer *src);
int buffer_append_string_lfill(buffer *b, const char *s, size_t maxlen);
int buffer_append_string_rfill(buffer *b, const char *s, size_t maxlen);

int buffer_append_hex(buffer *b, unsigned long len);
int buffer_append_long(buffer *b, long val);

#if defined(SIZEOF_LONG) && (SIZEOF_LONG == SIZEOF_OFF_T)
#define buffer_copy_off_t(x, y)		buffer_copy_long(x, y)
#define buffer_append_off_t(x, y)	buffer_append_long(x, y)
#else
int buffer_copy_off_t(buffer *b, off_t val);
int buffer_append_off_t(buffer *b, off_t val);
#endif

int buffer_append_memory(buffer *b, const char *s, size_t s_len);

char * buffer_search_string_len(buffer *b, const char *needle, size_t len);

int buffer_is_empty(buffer *b);
int buffer_is_equal(buffer *a, buffer *b);
int buffer_is_equal_right_len(buffer *a, buffer *b, size_t len);
int buffer_is_equal_string(buffer *a, const char *s, size_t b_len);
int buffer_caseless_compare(const char *a, size_t a_len, const char *b, size_t b_len);

int buffer_append_string_hex(buffer *b, const char *in, size_t in_len);
int buffer_append_string_url_encoded(buffer *b, const char *s);
int buffer_append_string_html_encoded(buffer *b, const char *s);

int buffer_urldecode(buffer *url);
int buffer_path_simplify(buffer *dest, buffer *src);

/** deprecated */
int ltostr(char *buf, long val);
char hex2int(unsigned char c);
char int2hex(char i);

int light_isdigit(int c);
int light_isxdigit(int c);
int light_isalpha(int c);
int light_isalnum(int c);

#define BUFFER_APPEND_STRING_CONST(x, y) \
	buffer_append_string_len(x, y, sizeof(y) - 1)

#define BUFFER_COPY_STRING_CONST(x, y) \
	buffer_copy_string_len(x, y, sizeof(y) - 1)

#define BUFFER_APPEND_SLASH(x) \
	if (x->used > 1 && x->ptr[x->used - 2] != '/') { BUFFER_APPEND_STRING_CONST(x, "/"); }

#define CONST_STR_LEN(x) x, sizeof(x) - 1
#define CONST_BUF_LEN(x) x->ptr, x->used - 1


#define SEGFAULT() do { fprintf(stderr, "%s.%d: unexpected event, aborting\n", __FILE__, __LINE__); abort(); } while(0)
#define UNUSED(x) ( (void)(x) )

#endif
