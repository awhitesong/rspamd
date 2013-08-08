/* Copyright (c) 2013, Vsevolod Stakhov
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *       * Redistributions of source code must retain the above copyright
 *         notice, this list of conditions and the following disclaimer.
 *       * Redistributions in binary form must reproduce the above copyright
 *         notice, this list of conditions and the following disclaimer in the
 *         documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "rcl.h"
#include "rcl_internal.h"
#include "util.h"

/**
 * @file rcl_parser.c
 * The implementation of rcl parser
 */

/**
 * Create a new object
 * @return new object
 */
static inline rspamd_cl_object_t *
rspamd_cl_object_new (void)
{
	return g_slice_alloc0 (sizeof (rspamd_cl_object_t));
}

/**
 * Move up to len characters
 * @param parser
 * @param begin
 * @param len
 * @return new position in chunk
 */
static inline void
rspamd_cl_chunk_skipc (struct rspamd_cl_chunk *chunk, guchar c)
{
	if (c == '\n') {
		chunk->line ++;
		chunk->column = 0;
	}
	else {
		chunk->column ++;
	}

	chunk->pos ++;
	chunk->remain --;
}

static inline void
rspamd_cl_set_err (struct rspamd_cl_chunk *chunk, gint code, const char *str, GError **err)
{
	g_set_error (err, RCL_ERROR, code, "Error detected on line %d at pos %d: '%s'",
			chunk->line, chunk->column, str);
}

static gboolean
rspamd_cl_skip_comments (struct rspamd_cl_parser *parser, GError **err)
{
	struct rspamd_cl_chunk *chunk = parser->chunks;
	const guchar *p;
	gint comments_nested = 0;

	p = chunk->pos;

	if (*p == '#') {
		if (parser->state != RSPAMD_RCL_STATE_SCOMMENT &&
				parser->state != RSPAMD_RCL_STATE_MCOMMENT) {
			while (p < chunk->end) {
				if (*p == '\n') {
					rspamd_cl_chunk_skipc (chunk, *p);
					break;
				}
				rspamd_cl_chunk_skipc (chunk, *p);
				p ++;
			}
		}
	}
	else if (*p == '/' && chunk->remain >= 2) {
		p ++;
		if (*p == '/' && parser->state != RSPAMD_RCL_STATE_SCOMMENT &&
				parser->state != RSPAMD_RCL_STATE_MCOMMENT) {
			chunk->pos = p;
			while (p < chunk->end) {
				if (*p == '\n') {
					rspamd_cl_chunk_skipc (chunk, *p);
					break;
				}
				rspamd_cl_chunk_skipc (chunk, *p);
				p ++;
			}
		}
		else if (*p == '*') {
			comments_nested ++;
			chunk->pos = p;

			while (p < chunk->end) {
				if (*p == '*') {
					rspamd_cl_chunk_skipc (chunk, *p);
					p ++;
					rspamd_cl_chunk_skipc (chunk, *p);
					if (*p == '/') {
						comments_nested --;
						if (comments_nested == 0) {
							break;
						}
					}
					p ++;
					rspamd_cl_chunk_skipc (chunk, *p);
				}
				rspamd_cl_chunk_skipc (chunk, *p);
				p ++;
			}
			if (comments_nested != 0) {
				rspamd_cl_set_err (chunk, RSPAMD_CL_ENESTED, "comments nesting is invalid", err);
				return FALSE;
			}
		}
	}

	return TRUE;
}

/**
 * Handle include macro
 * @param data include data
 * @param len length of data
 * @param ud user data
 * @param err error ptr
 * @return
 */
static gboolean
rspamd_cl_include_handler (const guchar *data, gsize len, gpointer ud, GError **err)
{
	return TRUE;
}

/**
 * Handle includes macro
 * @param data include data
 * @param len length of data
 * @param ud user data
 * @param err error ptr
 * @return
 */
static gboolean
rspamd_cl_includes_handler (const guchar *data, gsize len, gpointer ud, GError **err)
{
	return TRUE;
}

/**
 * Parse quoted string with possible escapes
 * @param parser
 * @param chunk
 * @param err
 * @return TRUE if a string has been parsed
 */
static gboolean
rspamd_cl_lex_json_string (struct rspamd_cl_parser *parser,
		struct rspamd_cl_chunk *chunk, GError **err)
{
	const guchar *p = chunk->pos;
	guchar c;
	gint i;

	while (p < chunk->end) {
		c = *p;
		if (c < 0x1F) {
			/* Unmasked control character */
			if (c == '\n') {
				rspamd_cl_set_err (chunk, RSPAMD_CL_ESYNTAX, "unexpected newline", err);
			}
			else {
				rspamd_cl_set_err (chunk, RSPAMD_CL_ESYNTAX, "unexpected control character", err);
			}
			return FALSE;
		}
		if (c == '\\') {
			rspamd_cl_chunk_skipc (chunk, *p);
			p ++;
			c = *p;
			if (p >= chunk->end) {
				rspamd_cl_set_err (chunk, RSPAMD_CL_ESYNTAX, "unfinished escape character", err);
				return FALSE;
			}
			if (*p == 'u') {
				rspamd_cl_chunk_skipc (chunk, *p);
				p ++;
				for (i = 0; i < 4 && p < chunk->end; i ++) {
					if (!g_ascii_isxdigit (*p)) {
						rspamd_cl_set_err (chunk, RSPAMD_CL_ESYNTAX, "invalid utf escape", err);
						return FALSE;
					}
					rspamd_cl_chunk_skipc (chunk, *p);
					p ++;
				}
				if (p >= chunk->end) {
					rspamd_cl_set_err (chunk, RSPAMD_CL_ESYNTAX, "unfinished escape character", err);
					return FALSE;
				}
			}
			else if (c == '"' || c == '\\' || c == '/' || c == 'b' ||
					c == 'f' || c == 'n' || c == 'r' || c == 't') {
				rspamd_cl_chunk_skipc (chunk, *p);
				p ++;
			}
			else {
				rspamd_cl_set_err (chunk, RSPAMD_CL_ESYNTAX, "invalid escape character", err);
				return FALSE;
			}
			continue;
		}
		else if (c == '"') {
			return TRUE;
		}
	}

	return FALSE;
}

/**
 * Parse a key in an object
 * @param parser
 * @param chunk
 * @param err
 * @return TRUE if a key has been parsed
 */
static gboolean
rspamd_cl_parse_key (struct rspamd_cl_parser *parser,
		struct rspamd_cl_chunk *chunk, GError **err)
{
	const guchar *p, *c = NULL, *end;
	gboolean got_quote = FALSE, got_eq = FALSE, got_semicolon = FALSE;
	rspamd_cl_object_t *nobj, *tobj;

	p = chunk->pos;

	/* Skip any spaces */
	while (p < chunk->end && g_ascii_isspace (*p)) {
		rspamd_cl_chunk_skipc (chunk, *p);
		p ++;
	}

	while (p < chunk->end) {
		/*
		 * A key must start with alpha and end with space character
		 */
		if (*p == '.') {
			/* It is macro actually */
			rspamd_cl_chunk_skipc (chunk, *p);
			parser->state = RSPAMD_RCL_STATE_MACRO_NAME;
			return TRUE;
		}
		else if (c == NULL) {
			if (g_ascii_isalpha (*p)) {
				/* The first symbol */
				c = p;
				rspamd_cl_chunk_skipc (chunk, *p);
				p ++;
			}
			else if (*p == '"') {
				/* JSON style key */
				c = p + 1;
				got_quote = TRUE;
				rspamd_cl_chunk_skipc (chunk, *p);
				p ++;
			}
			else {
				/* Invalid identifier */
				parser->state = RSPAMD_RCL_STATE_ERROR;
				rspamd_cl_set_err (chunk, RSPAMD_CL_ESYNTAX, "key must begin with a letter", err);
				return FALSE;
			}
		}
		else {
			/* Parse the body of a key */
			if (!got_quote) {
				if (g_ascii_isalnum (*p)) {
					rspamd_cl_chunk_skipc (chunk, *p);
					p ++;
				}
				else if (*p == ' ' || *p == '\t' || *p == ':' || *p == '=') {
					end = p;
					break;
				}
				else {
					rspamd_cl_set_err (chunk, RSPAMD_CL_ESYNTAX, "invalid character in a key", err);
					return FALSE;
				}
			}
			else {
				/* We need to parse json like quoted string */
				if (!rspamd_cl_lex_json_string (parser, chunk, err)) {
					return FALSE;
				}
				end = chunk->pos;
				p = end;
				rspamd_cl_chunk_skipc (chunk, *p);
				p ++;
				break;
			}
		}
	}

	if (p >= chunk->end) {
		rspamd_cl_set_err (chunk, RSPAMD_CL_ESYNTAX, "unfinished key", err);
		return FALSE;
	}

	/* We are now at the end of the key, need to parse the rest */
	while (p < chunk->end) {
		if (g_ascii_isspace (*p)) {
			rspamd_cl_chunk_skipc (chunk, *p);
			p ++;
		}
		else if (*p == '=') {
			if (!got_eq && !got_semicolon) {
				rspamd_cl_chunk_skipc (chunk, *p);
				p ++;
				got_eq = TRUE;
			}
			else {
				rspamd_cl_set_err (chunk, RSPAMD_CL_ESYNTAX, "unexpected '=' character", err);
				return FALSE;
			}
		}
		else if (*p == ':') {
			if (!got_eq && !got_semicolon) {
				rspamd_cl_chunk_skipc (chunk, *p);
				p ++;
				got_semicolon = TRUE;
			}
			else {
				rspamd_cl_set_err (chunk, RSPAMD_CL_ESYNTAX, "unexpected ':' character", err);
				return FALSE;
			}
		}
		else if (*p == '/' || *p == '#') {
			/* Check for comment */
			if (!rspamd_cl_skip_comments (parser, err)) {
				return FALSE;
			}
			p = chunk->pos;
		}
		else {
			/* Start value */
			break;
		}
	}

	if (p >= chunk->end) {
		rspamd_cl_set_err (chunk, RSPAMD_CL_ESYNTAX, "unfinished key", err);
		return FALSE;
	}

	/* Create a new object */
	nobj = rspamd_cl_object_new ();
	nobj->key = g_malloc (end - c + 1);
	rspamd_strlcpy (nobj->key, c, end - c + 1);

	if (got_quote) {
		rspamd_cl_unescape_json_string (nobj->key);
	}

	HASH_FIND_STR (parser->cur_obj->value.ov, nobj->key, tobj);
	if (tobj != NULL) {
		/* We are going to replace old key with new one */
		HASH_DELETE (hh, parser->cur_obj->value.ov, tobj);
		rspamd_cl_obj_free (tobj);
	}

	HASH_ADD_KEYPTR (hh, parser->cur_obj->value.ov, nobj->key, strlen (nobj->key), nobj);

	return TRUE;
}

/**
 * Handle the main states of rcl parser
 * @param parser parser structure
 * @param data the pointer to the beginning of a chunk
 * @param len the length of a chunk
 * @param err if *err is NULL it is set to parser error
 * @return TRUE if chunk has been parsed and FALSE in case of error
 */
static gboolean
rspamd_cl_state_machine (struct rspamd_cl_parser *parser, GError **err)
{
	rspamd_cl_object_t *obj;
	struct rspamd_cl_chunk *chunk = parser->chunks;
	const guchar *p;

	p = chunk->pos;
	while (chunk->pos < chunk->end) {
		switch (parser->state) {
		case RSPAMD_RCL_STATE_INIT:
			/*
			 * At the init state we can either go to the parse array or object
			 * if we got [ or { correspondingly or can just treat new data as
			 * a key of newly created object
			 */
			if (!rspamd_cl_skip_comments (parser, err)) {
				parser->state = RSPAMD_RCL_STATE_ERROR;
				return FALSE;
			}
			else {
				obj = rspamd_cl_object_new ();
				if (*p == '[') {
					parser->state = RSPAMD_RCL_STATE_ARRAY;
					obj->type = RSPAMD_CL_ARRAY;
					rspamd_cl_chunk_skipc (chunk, *p);
					p ++;
				}
				else {
					parser->state = RSPAMD_RCL_STATE_KEY;
					obj->type = RSPAMD_CL_OBJECT;
					if (*p == '{') {
						rspamd_cl_chunk_skipc (chunk, *p);
						p ++;
					}
				}
				parser->cur_obj = obj;
				parser->top_obj = obj;
			}
			break;
		case RSPAMD_RCL_STATE_KEY:
			if (!rspamd_cl_parse_key (parser, chunk, err)) {
				parser->state = RSPAMD_RCL_STATE_ERROR;
				return FALSE;
			}
			break;
		default:
			/* TODO: add all states */
			return FALSE;
		}
	}

	return TRUE;
}

struct rspamd_cl_parser*
rspamd_cl_parser_new (void)
{
	struct rspamd_cl_parser *new;

	new = g_slice_alloc0 (sizeof (struct rspamd_cl_parser));

	rspamd_cl_parser_register_macro (new, "include", rspamd_cl_include_handler, new);
	rspamd_cl_parser_register_macro (new, "includes", rspamd_cl_includes_handler, new);

	return new;
}


void
rspamd_cl_parser_register_macro (struct rspamd_cl_parser *parser, const gchar *macro,
		rspamd_cl_macro_handler handler, gpointer ud)
{
	struct rspamd_cl_macro *new;

	new = g_slice_alloc0 (sizeof (struct rspamd_cl_macro));
	new->handler = handler;
	new->name = g_strdup (macro);
	new->ud = ud;
	HASH_ADD_KEYPTR (hh, parser->macroes, new->name, strlen (new->name), new);
}

gboolean
rspamd_cl_parser_add_chunk (struct rspamd_cl_parser *parser, const guchar *data,
		gsize len, GError **err)
{
	struct rspamd_cl_chunk *chunk;

	if (parser->state != RSPAMD_RCL_STATE_ERROR) {
		chunk = g_slice_alloc (sizeof (struct rspamd_cl_chunk));
		chunk->begin = data;
		chunk->remain = len;
		chunk->pos = chunk->begin;
		chunk->end = chunk->begin + len;
		chunk->line = 1;
		chunk->column = 0;
		LL_PREPEND (parser->chunks, chunk);
		return rspamd_cl_state_machine (parser, err);
	}

	g_set_error (err, RCL_ERROR, RSPAMD_CL_ESTATE, "a parser is in an invalid state");

	return FALSE;
}