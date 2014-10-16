#include "base.h"
#include "log.h"
#include "buffer.h"
#include "response.h"
#include "stat_cache.h"

#include "plugin.h"

#include "crc32.h"
#include "etag.h"
#include "file.h"

#include <sys/types.h>
#include <sys/stat.h>

#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#if defined HAVE_ZLIB_H && defined HAVE_LIBZ
# define USE_ZLIB
# include <zlib.h>
#endif

#if defined HAVE_BZLIB_H && defined HAVE_LIBBZ2
# define USE_BZ2LIB
/* we don't need stdio interface */
# define BZ_NO_STDIO
# include <bzlib.h>
#endif

/* request: accept-encoding */
#define HTTP_ACCEPT_ENCODING_IDENTITY BV(0)
#define HTTP_ACCEPT_ENCODING_GZIP     BV(1)
#define HTTP_ACCEPT_ENCODING_DEFLATE  BV(2)
#define HTTP_ACCEPT_ENCODING_COMPRESS BV(3)
#define HTTP_ACCEPT_ENCODING_BZIP2    BV(4)
#define HTTP_ACCEPT_ENCODING_X_GZIP   BV(5)
#define HTTP_ACCEPT_ENCODING_X_BZIP2  BV(6)

/* maximum allowed "compress.max-filesize" */
/* (must ensure that 1.1*COMPRESS_MAX_FILESIZE still fits into memory) */
#define COMPRESS_MAX_FILESIZE (128 * 1024 * 1024)

#ifdef __WIN32
# define mkdir(x,y) mkdir(x)
#endif

typedef struct {
	buffer *compress_cache_dir;
	array  *compress;
	off_t   compress_max_filesize; /** max filesize in kb */
	int     allowed_encodings;
} plugin_config;

typedef struct {
	PLUGIN_DATA;
	buffer *ofn;
	buffer *b;

	plugin_config **config_storage;
	plugin_config conf;
} plugin_data;

INIT_FUNC(mod_compress_init) {
	plugin_data *p;

	p = calloc(1, sizeof(*p));

	p->ofn = buffer_init();
	p->b = buffer_init();

	return p;
}

FREE_FUNC(mod_compress_free) {
	plugin_data *p = p_d;

	UNUSED(srv);

	if (!p) return HANDLER_GO_ON;

	buffer_free(p->ofn);
	buffer_free(p->b);

	if (p->config_storage) {
		size_t i;
		for (i = 0; i < srv->config_context->used; i++) {
			plugin_config *s = p->config_storage[i];

			if (NULL == s) continue;

			array_free(s->compress);
			buffer_free(s->compress_cache_dir);

			free(s);
		}
		free(p->config_storage);
	}


	free(p);

	return HANDLER_GO_ON;
}

/* 0 on success, -1 for error */
static int mkdir_recursive(char *dir) {
	char *p = dir;

	if (!dir || !dir[0])
		return 0;

	while ((p = strchr(p + 1, '/')) != NULL) {

		*p = '\0';
		if ((mkdir(dir, 0700) != 0) && (errno != EEXIST)) {
			*p = '/';
			return -1;
		}

		*p++ = '/';
		if (!*p) return 0; /* Ignore trailing slash */
	}

	return (mkdir(dir, 0700) != 0) && (errno != EEXIST) ? -1 : 0;
}

/* 0 on success, -1 for error */
static int mkdir_for_file(char *filename) {
	char *p = filename;

	if (!filename || !filename[0])
		return -1;

	while ((p = strchr(p + 1, '/')) != NULL) {

		*p = '\0';
		if ((mkdir(filename, 0700) != 0) && (errno != EEXIST)) {
			*p = '/';
			return -1;
		}

		*p++ = '/';
		if (!*p) return -1; /* Unexpected trailing slash in filename */
	}

	return 0;
}

SETDEFAULTS_FUNC(mod_compress_setdefaults) {
	plugin_data *p = p_d;
	size_t i = 0, j;

	config_values_t cv[] = {
		{ "compress.cache-dir",             NULL, T_CONFIG_STRING, T_CONFIG_SCOPE_CONNECTION },
		{ "compress.filetype",              NULL, T_CONFIG_ARRAY, T_CONFIG_SCOPE_CONNECTION },
		{ "compress.max-filesize",          NULL, T_CONFIG_SHORT, T_CONFIG_SCOPE_CONNECTION },
		{ "compress.allowed-encodings",     NULL, T_CONFIG_ARRAY, T_CONFIG_SCOPE_CONNECTION },
		{ NULL,                             NULL, T_CONFIG_UNSET, T_CONFIG_SCOPE_UNSET }
	};

	p->config_storage = calloc(1, srv->config_context->used * sizeof(plugin_config *));

	for (i = 0; i < srv->config_context->used; i++) {
		plugin_config *s;
		array  *encodings_arr = array_init();
		unsigned short max_filesize = 0;

		s = calloc(1, sizeof(plugin_config));
		s->compress_cache_dir = buffer_init();
		s->compress = array_init();
		s->compress_max_filesize = 0;
		s->allowed_encodings = 0;

		cv[0].destination = s->compress_cache_dir;
		cv[1].destination = s->compress;
		cv[2].destination = &max_filesize;
		cv[3].destination = encodings_arr; /* temp array for allowed encodings list */

		p->config_storage[i] = s;

		if (0 != config_insert_values_global(srv, ((data_config *)srv->config_context->data[i])->value, cv)) {
			return HANDLER_ERROR;
		}

		if (encodings_arr->used) {
			for (j = 0; j < encodings_arr->used; j++) {
				data_string *ds = (data_string *)encodings_arr->data[j];
#ifdef USE_ZLIB
				if (NULL != strstr(ds->value->ptr, "gzip"))
					s->allowed_encodings |= HTTP_ACCEPT_ENCODING_GZIP | HTTP_ACCEPT_ENCODING_X_GZIP;
				if (NULL != strstr(ds->value->ptr, "x-gzip"))
					s->allowed_encodings |= HTTP_ACCEPT_ENCODING_X_GZIP;
				if (NULL != strstr(ds->value->ptr, "deflate"))
					s->allowed_encodings |= HTTP_ACCEPT_ENCODING_DEFLATE;
				/*
				if (NULL != strstr(ds->value->ptr, "compress"))
					s->allowed_encodings |= HTTP_ACCEPT_ENCODING_COMPRESS;
				*/
#endif
#ifdef USE_BZ2LIB
				if (NULL != strstr(ds->value->ptr, "bzip2"))
					s->allowed_encodings |= HTTP_ACCEPT_ENCODING_BZIP2 | HTTP_ACCEPT_ENCODING_X_BZIP2;
				if (NULL != strstr(ds->value->ptr, "x-bzip2"))
					s->allowed_encodings |= HTTP_ACCEPT_ENCODING_X_BZIP2;
#endif
			}
		} else {
			/* default encodings */
			s->allowed_encodings = 0
#ifdef USE_ZLIB
				| HTTP_ACCEPT_ENCODING_GZIP | HTTP_ACCEPT_ENCODING_X_GZIP | HTTP_ACCEPT_ENCODING_DEFLATE
#endif
#ifdef USE_BZ2LIB
				| HTTP_ACCEPT_ENCODING_BZIP2 | HTTP_ACCEPT_ENCODING_X_BZIP2
#endif
				;
		}

		array_free(encodings_arr);

		for (j = 0; j < s->compress->used; j++) {
			data_unset *du = s->compress->data[j];
			force_assert(NULL != du);

			if (TYPE_STRING != du->type) {
				log_error_write(srv, __FILE__, __LINE__, "s", "compress.filetype must be a list of strings");
				return HANDLER_ERROR;
			}
		}

		s->compress_max_filesize = max_filesize << 10;
		if (s->compress_max_filesize > COMPRESS_MAX_FILESIZE) {
			s->compress_max_filesize = COMPRESS_MAX_FILESIZE;
			log_error_write(srv, __FILE__, __LINE__, "sO", "compress.max-filesize too large, capping to", s->compress_max_filesize);
		}

		if (!buffer_string_is_empty(s->compress_cache_dir)) {
			struct stat st;
			mkdir_recursive(s->compress_cache_dir->ptr);

			if (0 != stat(s->compress_cache_dir->ptr, &st)) {
				log_error_write(srv, __FILE__, __LINE__, "sbs", "can't stat compress.cache-dir",
						s->compress_cache_dir, strerror(errno));

				return HANDLER_ERROR;
			}
		}
	}

	return HANDLER_GO_ON;

}

#ifdef USE_ZLIB
static int deflate_file_to_buffer_gzip(server *srv, connection *con, plugin_data *p, const unsigned char *start, off_t st_size, time_t mtime) {
	unsigned char *c;
	unsigned long crc;
	z_stream z;
	size_t outlen;

	UNUSED(srv);
	UNUSED(con);

	z.zalloc = Z_NULL;
	z.zfree = Z_NULL;
	z.opaque = Z_NULL;

	if (Z_OK != deflateInit2(&z,
				 Z_DEFAULT_COMPRESSION,
				 Z_DEFLATED,
				 -MAX_WBITS,  /* supress zlib-header */
				 8,
				 Z_DEFAULT_STRATEGY)) {
		return -1;
	}

	z.next_in = (unsigned char *)start;
	z.avail_in = st_size;
	z.total_in = 0;


	buffer_string_prepare_copy(p->b, (z.avail_in * 1.1) + 12 + 18);

	/* write gzip header */

	c = (unsigned char *)p->b->ptr;
	c[0] = 0x1f;
	c[1] = 0x8b;
	c[2] = Z_DEFLATED;
	c[3] = 0; /* options */
	c[4] = (mtime >>  0) & 0xff;
	c[5] = (mtime >>  8) & 0xff;
	c[6] = (mtime >> 16) & 0xff;
	c[7] = (mtime >> 24) & 0xff;
	c[8] = 0x00; /* extra flags */
	c[9] = 0x03; /* UNIX */

	outlen = 10;
	z.next_out = (unsigned char *)p->b->ptr + outlen;
	z.avail_out = p->b->size - outlen - 9;
	z.total_out = 0;

	if (Z_STREAM_END != deflate(&z, Z_FINISH)) {
		deflateEnd(&z);
		return -1;
	}

	/* trailer */
	outlen += z.total_out;

	crc = generate_crc32c((const char*) start, st_size);

	c = (unsigned char *)p->b->ptr + outlen;

	c[0] = (crc >>  0) & 0xff;
	c[1] = (crc >>  8) & 0xff;
	c[2] = (crc >> 16) & 0xff;
	c[3] = (crc >> 24) & 0xff;
	c[4] = (z.total_in >>  0) & 0xff;
	c[5] = (z.total_in >>  8) & 0xff;
	c[6] = (z.total_in >> 16) & 0xff;
	c[7] = (z.total_in >> 24) & 0xff;
	outlen += 8;
	buffer_commit(p->b, outlen);

	if (Z_OK != deflateEnd(&z)) {
		return -1;
	}

	return 0;
}

static int deflate_file_to_buffer_deflate(server *srv, connection *con, plugin_data *p, const unsigned char *start, off_t st_size) {
	z_stream z;

	UNUSED(srv);
	UNUSED(con);

	z.zalloc = Z_NULL;
	z.zfree = Z_NULL;
	z.opaque = Z_NULL;

	if (Z_OK != deflateInit2(&z,
				 Z_DEFAULT_COMPRESSION,
				 Z_DEFLATED,
				 -MAX_WBITS,  /* supress zlib-header */
				 8,
				 Z_DEFAULT_STRATEGY)) {
		return -1;
	}

	z.next_in = (unsigned char*) start;
	z.avail_in = st_size;
	z.total_in = 0;

	buffer_string_prepare_copy(p->b, (z.avail_in * 1.1) + 12);

	z.next_out = (unsigned char *)p->b->ptr;
	z.avail_out = p->b->size - 1;
	z.total_out = 0;

	if (Z_STREAM_END != deflate(&z, Z_FINISH)) {
		deflateEnd(&z);
		return -1;
	}

	if (Z_OK != deflateEnd(&z)) {
		return -1;
	}

	/* trailer */
	buffer_commit(p->b, z.total_out);

	return 0;
}

#endif

#ifdef USE_BZ2LIB
static int deflate_file_to_buffer_bzip2(server *srv, connection *con, plugin_data *p, const unsigned char *start, off_t st_size) {
	bz_stream bz;

	UNUSED(srv);
	UNUSED(con);

	bz.bzalloc = NULL;
	bz.bzfree = NULL;
	bz.opaque = NULL;

	if (BZ_OK != BZ2_bzCompressInit(&bz,
					9, /* blocksize = 900k */
					0, /* no output */
					0)) { /* workFactor: default */
		return -1;
	}

	bz.next_in = (char *)start;
	bz.avail_in = st_size;
	bz.total_in_lo32 = 0;
	bz.total_in_hi32 = 0;

	buffer_string_prepare_copy(p->b, (bz.avail_in * 1.1) + 12);

	bz.next_out = p->b->ptr;
	bz.avail_out = p->b->size - 1;
	bz.total_out_lo32 = 0;
	bz.total_out_hi32 = 0;

	if (BZ_STREAM_END != BZ2_bzCompress(&bz, BZ_FINISH)) {
		BZ2_bzCompressEnd(&bz);
		return -1;
	}

	if (BZ_OK != BZ2_bzCompressEnd(&bz)) {
		return -1;
	}

	/* file is too large for now */
	if (bz.total_out_hi32) return -1;

	/* trailer */
	buffer_commit(p->b, bz.total_out_lo32);

	return 0;
}
#endif

static int deflate_file(server *srv, connection *con, plugin_data *p, buffer *fn, int ifd, struct stat *st, int type) {
	file_iterator it;
	int ret;

	file_iterator_init(&it, ifd);
	if (0 > file_iterator_load(&it, st->st_size)) {
		log_error_write(srv, __FILE__, __LINE__, "sbss", "reading", fn, "failed", strerror(errno));
		file_iterator_clear(&it);
		return -1;
	}
	if (st->st_size != (off_t) it.length) {
		log_error_write(srv, __FILE__, __LINE__, "sbs", "reading", fn, "failed: didn't get enough data");
		file_iterator_clear(&it);
		return -1;
	}

	ret = -1;
	switch(type) {
#ifdef USE_ZLIB
	case HTTP_ACCEPT_ENCODING_GZIP:
	case HTTP_ACCEPT_ENCODING_X_GZIP:
		ret = deflate_file_to_buffer_gzip(srv, con, p, it.buf, it.length, st->st_mtime);
		break;
	case HTTP_ACCEPT_ENCODING_DEFLATE:
		ret = deflate_file_to_buffer_deflate(srv, con, p, it.buf, it.length);
		break;
#endif
#ifdef USE_BZ2LIB
	case HTTP_ACCEPT_ENCODING_BZIP2:
	case HTTP_ACCEPT_ENCODING_X_BZIP2:
		ret = deflate_file_to_buffer_bzip2(srv, con, p, it.buf, it.length);
		break;
#endif
	}

	file_iterator_clear(&it);
	return ret;
}

static int deflate_file_to_file(server *srv, connection *con, plugin_data *p, buffer *fn, int ifd, struct stat *st, int type) {
	int ofd;
	int ret;
	ssize_t r;

	force_assert(st->st_size <= COMPRESS_MAX_FILESIZE);

	buffer_reset(p->ofn);
	buffer_copy_buffer(p->ofn, p->conf.compress_cache_dir);
	buffer_append_slash(p->ofn);

	if (0 == strncmp(con->physical.path->ptr, con->physical.doc_root->ptr, buffer_string_length(con->physical.doc_root))) {
		buffer_append_string(p->ofn, con->physical.path->ptr + buffer_string_length(con->physical.doc_root));
	} else {
		buffer_append_string_buffer(p->ofn, con->uri.path);
	}

	switch(type) {
	case HTTP_ACCEPT_ENCODING_GZIP:
	case HTTP_ACCEPT_ENCODING_X_GZIP:
		buffer_append_string_len(p->ofn, CONST_STR_LEN("-gzip-"));
		break;
	case HTTP_ACCEPT_ENCODING_DEFLATE:
		buffer_append_string_len(p->ofn, CONST_STR_LEN("-deflate-"));
		break;
	case HTTP_ACCEPT_ENCODING_BZIP2:
	case HTTP_ACCEPT_ENCODING_X_BZIP2:
		buffer_append_string_len(p->ofn, CONST_STR_LEN("-bzip2-"));
		break;
	default:
		log_error_write(srv, __FILE__, __LINE__, "sd", "unknown compression type", type);
		return -1;
	}

	/* append "readable" etag data (before mutate) */
	etag_create(srv->tmp_buf, st, con->etag_flags);
	buffer_append_string_buffer(p->ofn, srv->tmp_buf);

	if (-1 == mkdir_for_file(p->ofn->ptr)) {
		log_error_write(srv, __FILE__, __LINE__, "sb", "couldn't create directory for file", p->ofn);
		return -1;
	}

	if (-1 == (ofd = open(p->ofn->ptr, O_WRONLY | O_CREAT | O_EXCL | O_BINARY, 0600))) {
		if (errno == EEXIST) {
			/* cache-entry exists */
#if 0
			log_error_write(srv, __FILE__, __LINE__, "bs", p->ofn, "compress-cache hit");
#endif
			buffer_copy_buffer(con->physical.path, p->ofn);

			return 0;
		}

		log_error_write(srv, __FILE__, __LINE__, "sbss", "creating cachefile", p->ofn, "failed", strerror(errno));

		return -1;
	}
#if 0
	log_error_write(srv, __FILE__, __LINE__, "bs", p->ofn, "compress-cache miss");
#endif

	ret = deflate_file(srv, con, p, fn, ifd, st, type);

	if (ret == 0) {
		r = write(ofd, CONST_BUF_LEN(p->b));
		if (-1 == r) {
			log_error_write(srv, __FILE__, __LINE__, "sbss", "writing cachefile", p->ofn, "failed:", strerror(errno));
			ret = -1;
		} else if ((size_t)r != buffer_string_length(p->b)) {
			log_error_write(srv, __FILE__, __LINE__, "sbs", "writing cachefile", p->ofn, "failed: not enough bytes written");
			ret = -1;
		}
	}

	close(ofd);

	if (ret != 0) {
		/* Remove the incomplete cache file, so that later hits aren't served from it */
		if (-1 == unlink(p->ofn->ptr)) {
			log_error_write(srv, __FILE__, __LINE__, "sbss", "unlinking incomplete cachefile", p->ofn, "failed:", strerror(errno));
		}

		return -1;
	}

	buffer_copy_buffer(con->physical.path, p->ofn);

	return 0;
}

static int deflate_file_to_buffer(server *srv, connection *con, plugin_data *p, buffer *fn, int ifd, struct stat *st, int type) {
	force_assert(st->st_size <= COMPRESS_MAX_FILESIZE);

	if (0 != deflate_file(srv, con, p, fn, ifd, st, type)) return -1;

	chunkqueue_reset(con->write_queue);
	chunkqueue_append_buffer(con->write_queue, p->b);

	buffer_reset(con->physical.path);

	con->file_finished = 1;
	con->file_started  = 1;

	return 0;
}


#define PATCH(x) \
	p->conf.x = s->x;
static int mod_compress_patch_connection(server *srv, connection *con, plugin_data *p) {
	size_t i, j;
	plugin_config *s = p->config_storage[0];

	PATCH(compress_cache_dir);
	PATCH(compress);
	PATCH(compress_max_filesize);
	PATCH(allowed_encodings);

	/* skip the first, the global context */
	for (i = 1; i < srv->config_context->used; i++) {
		data_config *dc = (data_config *)srv->config_context->data[i];
		s = p->config_storage[i];

		/* condition didn't match */
		if (!config_check_cond(srv, con, dc)) continue;

		/* merge config */
		for (j = 0; j < dc->value->used; j++) {
			data_unset *du = dc->value->data[j];

			if (buffer_is_equal_string(du->key, CONST_STR_LEN("compress.cache-dir"))) {
				PATCH(compress_cache_dir);
			} else if (buffer_is_equal_string(du->key, CONST_STR_LEN("compress.filetype"))) {
				PATCH(compress);
			} else if (buffer_is_equal_string(du->key, CONST_STR_LEN("compress.max-filesize"))) {
				PATCH(compress_max_filesize);
			} else if (buffer_is_equal_string(du->key, CONST_STR_LEN("compress.allowed-encodings"))) {
				PATCH(allowed_encodings);
			}
		}
	}

	return 0;
}
#undef PATCH

static int mod_compress_contains_encoding(const char *headervalue, const char *encoding) {
	const char *m;
	for ( ;; ) {
		m = strstr(headervalue, encoding);
		if (NULL == m) return 0;
		if (m == headervalue || m[-1] == ' ' || m[-1] == ',') return 1;

		/* only partial match, search for next value */
		m = strchr(m, ',');
		if (NULL == m) return 0;
		headervalue = m + 1;
	}
}

/* check if mimetype is in compress-config */
static int mod_compress_want_mimetype(server *srv, plugin_data *p, buffer *mimetype) {
	size_t i;
	buffer *content_type = NULL; /* mimetype without ';...' */

	if (!buffer_string_is_empty(mimetype)) {
		char *c;
		if (NULL != (c = strchr(mimetype->ptr, ';'))) {
			content_type = srv->tmp_buf;
			buffer_copy_string_len(content_type, mimetype->ptr, c - mimetype->ptr);
		}
	}

	for (i = 0; i < p->conf.compress->used; i++) {
		data_string *ds = (data_string *)p->conf.compress->data[i];

		if (buffer_is_equal(ds->value, mimetype)
		    || (content_type && buffer_is_equal(ds->value, content_type))) {
			return 1;
		}
	}

	return 0;
}

static int mod_compress_match_encoding(server *srv, connection *con, plugin_data *p, const char **compression_name) {
	data_string *ds;
	int compression_type = 0;
	int accept_encoding = 0;
	char *value;
	int matched_encodings = 0;

	if (NULL == (ds = (data_string *)array_get_element(con->request.headers, "Accept-Encoding"))) {
		if (con->conf.log_request_handling) {
			log_error_write(srv, __FILE__, __LINE__, "s", "-- client didn't send 'Accept-Encoding'");
		}
		return 0;
	}
	value = ds->value->ptr;

	/* get client side support encodings */
#ifdef USE_ZLIB
	if (mod_compress_contains_encoding(value, "gzip")) accept_encoding |= HTTP_ACCEPT_ENCODING_GZIP;
	if (mod_compress_contains_encoding(value, "x-gzip")) accept_encoding |= HTTP_ACCEPT_ENCODING_X_GZIP;
	if (mod_compress_contains_encoding(value, "deflate")) accept_encoding |= HTTP_ACCEPT_ENCODING_DEFLATE;
	if (mod_compress_contains_encoding(value, "compress")) accept_encoding |= HTTP_ACCEPT_ENCODING_COMPRESS;
#endif
#ifdef USE_BZ2LIB
	if (mod_compress_contains_encoding(value, "bzip2")) accept_encoding |= HTTP_ACCEPT_ENCODING_BZIP2;
	if (mod_compress_contains_encoding(value, "x-bzip2")) accept_encoding |= HTTP_ACCEPT_ENCODING_X_BZIP2;
#endif
	/* identity not supported below - not needed anyway.
	if (mod_compress_contains_encoding(value, "identity")) accept_encoding |= HTTP_ACCEPT_ENCODING_IDENTITY;
	*/

	/* find matching entries */
	matched_encodings = accept_encoding & p->conf.allowed_encodings;

	if (matched_encodings) {
		static const char dflt_gzip[] = "gzip";
		static const char dflt_x_gzip[] = "x-gzip";
		static const char dflt_deflate[] = "deflate";
		static const char dflt_bzip2[] = "bzip2";
		static const char dflt_x_bzip2[] = "x-bzip2";

		/* select best matching encoding */
		if (matched_encodings & HTTP_ACCEPT_ENCODING_BZIP2) {
			compression_type = HTTP_ACCEPT_ENCODING_BZIP2;
			*compression_name = dflt_bzip2;
		} else if (matched_encodings & HTTP_ACCEPT_ENCODING_X_BZIP2) {
			compression_type = HTTP_ACCEPT_ENCODING_X_BZIP2;
			*compression_name = dflt_x_bzip2;
		} else if (matched_encodings & HTTP_ACCEPT_ENCODING_GZIP) {
			compression_type = HTTP_ACCEPT_ENCODING_GZIP;
			*compression_name = dflt_gzip;
		} else if (matched_encodings & HTTP_ACCEPT_ENCODING_X_GZIP) {
			compression_type = HTTP_ACCEPT_ENCODING_X_GZIP;
			*compression_name = dflt_x_gzip;
		} else {
			force_assert(matched_encodings & HTTP_ACCEPT_ENCODING_DEFLATE);
			compression_type = HTTP_ACCEPT_ENCODING_DEFLATE;
			*compression_name = dflt_deflate;
		}
	} else {
		if (con->conf.log_request_handling) {
			log_error_write(srv, __FILE__, __LINE__, "s", "-- couldn't find common encoding");
		}
	}

	return compression_type;
}

PHYSICALPATH_FUNC(mod_compress_physical) {
	plugin_data *p = p_d;
	int fd = -1;
	struct stat st;
	buffer *mtime = NULL;
	buffer *mimetype = NULL;
	int compression_type;
	const char *compression_name = NULL;
	int result = HANDLER_GO_ON;
	int use_cache;

	if (con->mode != DIRECT || con->http_status) goto cleanup;

	/* only GET and POST can get compressed */
	switch (con->request.http_method) {
	case HTTP_METHOD_GET:
	case HTTP_METHOD_POST:
		break;
	default:
		goto cleanup;
	}

	if (buffer_string_is_empty(con->physical.path)) goto cleanup;

	mod_compress_patch_connection(srv, con, p);

	if (-1 == (fd = file_open(srv, con, con->physical.path, &st, 0))) {
		if (EISDIR == errno || ENOENT == errno) {
			if (con->conf.log_request_handling) {
				log_error_write(srv, __FILE__, __LINE__, "s", "-- directory or couldn't find file");
			}
			goto cleanup;
		}

		con->http_status = 403;
		if (con->conf.log_request_handling) {
			log_error_write(srv, __FILE__, __LINE__, "s", "-- couldn't open file", errno, EISDIR, EACCES);
		}
		result = HANDLER_FINISHED;
		goto cleanup;
	}

	/* don't compress files that are too large as we need to much time to handle them */
	if (p->conf.compress_max_filesize && (st.st_size >> 10) > p->conf.compress_max_filesize) {
		if (con->conf.log_request_handling) {
			log_error_write(srv, __FILE__, __LINE__, "s", "-- file too large");
		}
		goto cleanup;
	}

	/* don't try to compress files less than 128 bytes
	 *
	 * - extra overhead for compression
	 * - mmap() fails for st_size = 0 :)
	 */
	if (st.st_size < 128) {
		if (con->conf.log_request_handling) {
			log_error_write(srv, __FILE__, __LINE__, "s", "-- file too small");
		}
		goto cleanup;
	}

	if (con->conf.log_request_handling) {
		log_error_write(srv, __FILE__, __LINE__, "s", "-- handling file as static file");
	}

	mimetype = buffer_init();
	file_get_mimetype(mimetype, con, con->physical.path, fd);

	if (!mod_compress_want_mimetype(srv, p, mimetype)) {
		if (con->conf.log_request_handling) {
			log_error_write(srv, __FILE__, __LINE__, "SBS", "-- mimetype'", mimetype, "' not in compress.filetype");
		}
		goto cleanup;
	}


	/* the response might change according to Accept-Encoding */
	response_header_insert(srv, con, CONST_STR_LEN("Vary"), CONST_STR_LEN("Accept-Encoding"));

	etag_build(con->physical.etag, &st, con->etag_flags);

	/* try matching original etag of uncompressed version */
	mtime = strftime_cache_get(srv, st.st_mtime);
	if (HANDLER_FINISHED == http_response_handle_cachable(srv, con, mtime)) {
		response_header_overwrite(srv, con, CONST_STR_LEN("Content-Type"), CONST_BUF_LEN(mimetype));
		response_header_overwrite(srv, con, CONST_STR_LEN("Last-Modified"), CONST_BUF_LEN(mtime));
		if (con->etag_flags) {
			response_header_overwrite(srv, con, CONST_STR_LEN("ETag"), CONST_BUF_LEN(con->physical.etag));
		}
		result = HANDLER_FINISHED;
		goto cleanup;
	}

	if (0 == (compression_type = mod_compress_match_encoding(srv, con, p, &compression_name))) {
		/* mod_compress_match_encoding() did the logging */
		goto cleanup;
	}

	if (con->etag_flags) {
		/* try matching etag of compressed version */
		buffer_append_string_len(con->physical.etag, CONST_STR_LEN("-"));
		buffer_append_string(con->physical.etag, compression_name);
		etag_mutate(con->physical.etag, con->physical.etag);
	}

	if (HANDLER_FINISHED == http_response_handle_cachable(srv, con, mtime)) {
		response_header_overwrite(srv, con, CONST_STR_LEN("Content-Encoding"), compression_name, strlen(compression_name));
		response_header_overwrite(srv, con, CONST_STR_LEN("Content-Type"), CONST_BUF_LEN(mimetype));
		response_header_overwrite(srv, con, CONST_STR_LEN("Last-Modified"), CONST_BUF_LEN(mtime));
		if (con->etag_flags) {
			response_header_overwrite(srv, con, CONST_STR_LEN("ETag"), CONST_BUF_LEN(con->physical.etag));
		}
		result = HANDLER_FINISHED;
		goto cleanup;
	}

	use_cache = con->etag_flags && !buffer_string_is_empty(p->conf.compress_cache_dir);

	/* deflate it */
	if (use_cache) {
		if (0 != deflate_file_to_file(srv, con, p, con->physical.path, fd, &st, compression_type)) goto cleanup;
	} else {
		if (0 != deflate_file_to_buffer(srv, con, p, con->physical.path, fd, &st, compression_type)) goto cleanup;
	}
	response_header_overwrite(srv, con, CONST_STR_LEN("Content-Encoding"), compression_name, strlen(compression_name));
	response_header_overwrite(srv, con, CONST_STR_LEN("Last-Modified"), CONST_BUF_LEN(mtime));
	if (con->etag_flags) {
		response_header_overwrite(srv, con, CONST_STR_LEN("ETag"), CONST_BUF_LEN(con->physical.etag));
	}
	response_header_overwrite(srv, con, CONST_STR_LEN("Content-Type"), CONST_BUF_LEN(mimetype));

	/* if result wasn't cached (on disk) the response is finished */
	if (!use_cache) result = HANDLER_FINISHED;
	/* otherwise let mod_staticfile handle the cached compressed files, physical path was modified */
	/* else result = HANDLER_GO_ON; -- HANDLER_GO_ON is the default value */

cleanup:
	if (-1 != fd) close(fd);
	if (NULL != mimetype) buffer_free(mimetype);

	return result;
}

int mod_compress_plugin_init(plugin *p);
int mod_compress_plugin_init(plugin *p) {
	p->version     = LIGHTTPD_VERSION_ID;
	p->name        = buffer_init_string("compress");

	p->init        = mod_compress_init;
	p->set_defaults = mod_compress_setdefaults;
	p->handle_subrequest_start  = mod_compress_physical;
	p->cleanup     = mod_compress_free;

	p->data        = NULL;

	return 0;
}
