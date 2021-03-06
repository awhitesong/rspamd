/*
 * Copyright (c) 2009-2015, Vsevolod Stakhov
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY AUTHOR ''AS IS'' AND ANY
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
#include "util.h"
#include "main.h"
#include "message.h"
#include "symbols_cache.h"
#include "cfg_file.h"
#include "blake2.h"

/* After which number of messages try to resort cache */
#define MAX_USES 100
static const guchar rspamd_symbols_cache_magic[8] = {'r', 's', 'c', 1, 0, 0, 0, 0 };

struct rspamd_symbols_cache_header {
	guchar magic[8];
	guint nitems;
	guchar checksum[BLAKE2B_OUTBYTES];
	guchar unused[128];
};

struct symbols_cache {
	/* Hash table for fast access */
	GHashTable *items_by_symbol;
	GPtrArray *items_by_order;
	rspamd_mempool_t *static_pool;
	guint cur_items;
	guint used_items;
	guint uses;
	struct rspamd_config *cfg;
};

struct counter_data {
	gdouble value;
	gint number;
};

struct cache_item {
	/* This block is likely shared */
	gdouble avg_time;
	gdouble weight;
	guint32 frequency;

	/* Per process counter */
	struct counter_data *cd;
	gchar *symbol;
	enum rspamd_symbol_type type;

	/* Callback data */
	symbol_func_t func;
	gpointer user_data;

	/* Parent symbol id for virtual symbols */
	gint parent;
	/* Priority */
	gint priority;
	gint id;
	gdouble metric_weight;
};

/* weight, frequency, time */
#define TIME_ALPHA (1.0 / 10000000.0)
#define SCORE_FUN(w, f, t) (((w) > 0 ? (w) : 1) * ((f) > 0 ? (f) : 1) / (t > TIME_ALPHA ? t : TIME_ALPHA))

gint
cache_logic_cmp (const void *p1, const void *p2)
{
	const struct cache_item *i1 = p1, *i2 = p2;
	double w1, w2;
	double weight1, weight2;
	double f1 = 0, f2 = 0, t1, t2;

	if (i1->priority == i2->priority) {
		f1 = (double)i1->frequency;
		f2 = (double)i2->frequency;
		weight1 = i1->metric_weight == 0 ? i1->weight : i1->metric_weight;
		weight2 = i2->metric_weight == 0 ? i2->weight : i2->metric_weight;
		t1 = i1->avg_time / 1000000.0;
		t2 = i2->avg_time / 1000000.0;
		w1 = SCORE_FUN (abs (weight1), f1, t1);
		w2 = SCORE_FUN (abs (weight2), f2, t2);
		msg_debug ("%s -> %.2f, %s -> %.2f", i1->symbol, w1, i2->symbol, w2);
	}
	else {
		/* Strict sorting */
		w1 = abs (i1->priority);
		w2 = abs (i2->priority);
	}

	return (gint)w2 - w1;
}

/**
 * Set counter for a symbol
 */
static double
rspamd_set_counter (struct cache_item *item, guint32 value)
{
	struct counter_data *cd;
	cd = item->cd;

	/* Cumulative moving average using per-process counter data */
	if (cd->number == 0) {
		cd->value = 0;
	}

	cd->value = cd->value + (value - cd->value) / (++cd->number);

	return cd->value;
}

/* Sort items in logical order */
static void
post_cache_init (struct symbols_cache *cache)
{
	g_ptr_array_sort (cache->items_by_order, cache_logic_cmp);
}

static gboolean
rspamd_symbols_cache_load_items (struct symbols_cache *cache, const gchar *name)
{
	struct rspamd_symbols_cache_header *hdr;
	struct stat st;
	struct ucl_parser *parser;
	ucl_object_t *top;
	const ucl_object_t *cur, *elt;
	ucl_object_iter_t it;
	struct cache_item *item;
	const guchar *p;
	gint fd;
	gpointer map;

	fd = open (name, O_RDONLY);

	if (fd == -1) {
		msg_info ("cannot open file %s, error %d, %s", name,
			errno, strerror (errno));
		return FALSE;
	}

	if (fstat (fd, &st) == -1) {
		close (fd);
		msg_info ("cannot stat file %s, error %d, %s", name,
				errno, strerror (errno));
		return FALSE;
	}

	if (st.st_size < (gint)sizeof (*hdr)) {
		close (fd);
		errno = EINVAL;
		msg_info ("cannot use file %s, error %d, %s", name,
				errno, strerror (errno));
		return FALSE;
	}

	map = mmap (NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);

	if (map == MAP_FAILED) {
		close (fd);
		msg_info ("cannot mmap file %s, error %d, %s", name,
				errno, strerror (errno));
		return FALSE;
	}

	close (fd);
	hdr = map;

	if (memcmp (hdr->magic, rspamd_symbols_cache_magic,
			sizeof (rspamd_symbols_cache_magic)) != 0) {
		msg_info ("cannot use file %s, bad magic", name);
		munmap (map, st.st_size);
		return FALSE;
	}

	parser = ucl_parser_new (0);
	p = (const guchar *)(hdr + 1);

	if (!ucl_parser_add_chunk (parser, p, st.st_size - sizeof (*hdr))) {
		msg_info ("cannot use file %s, cannot parse: %s", name,
				ucl_parser_get_error (parser));
		munmap (map, st.st_size);
		ucl_parser_free (parser);
		return FALSE;
	}

	top = ucl_parser_get_object (parser);
	munmap (map, st.st_size);
	ucl_parser_free (parser);

	if (top == NULL || ucl_object_type (top) != UCL_OBJECT) {
		msg_info ("cannot use file %s, bad object", name);
		ucl_object_unref (top);
		return FALSE;
	}

	it = ucl_object_iterate_new (top);

	while ((cur = ucl_object_iterate_safe (it, true))) {
		item = g_hash_table_lookup (cache->items_by_symbol, ucl_object_key (cur));

		if (item) {
			/* Copy saved info */
			elt = ucl_object_find_key (cur, "weight");

			if (elt) {
				item->weight = ucl_object_todouble (cur);
			}

			elt = ucl_object_find_key (cur, "time");

			if (elt) {
				item->avg_time = ucl_object_todouble (cur);
			}

			elt = ucl_object_find_key (cur, "frequency");

			if (elt) {
				item->frequency = ucl_object_toint (cur);
			}
		}
	}

	ucl_object_iterate_free (it);
	ucl_object_unref (top);

	return TRUE;
}

static gboolean
rspamd_symbols_cache_save_items (struct symbols_cache *cache, const gchar *name)
{
	struct rspamd_symbols_cache_header hdr;
	ucl_object_t *top, *elt;
	GHashTableIter it;
	struct cache_item *item;
	struct ucl_emitter_functions *efunc;
	gpointer k, v;
	gint fd;
	bool ret;

	fd = open (name, O_CREAT | O_TRUNC | O_WRONLY | O_EXCL, 00644);

	if (fd == -1) {
		msg_info ("cannot open file %s, error %d, %s", name,
				errno, strerror (errno));
		return FALSE;
	}

	memset (&hdr, 0, sizeof (hdr));
	memcpy (hdr.magic, rspamd_symbols_cache_magic,
			sizeof (rspamd_symbols_cache_magic));

	if (write (fd, &hdr, sizeof (hdr)) == -1) {
		msg_info ("cannot write to file %s, error %d, %s", name,
				errno, strerror (errno));
		close (fd);

		return FALSE;
	}

	top = ucl_object_typed_new (UCL_OBJECT);
	g_hash_table_iter_init (&it, cache->items_by_symbol);

	while (g_hash_table_iter_next (&it, &k, &v)) {
		item = v;
		elt = ucl_object_typed_new (UCL_OBJECT);
		ucl_object_insert_key (elt, ucl_object_fromdouble (item->weight),
				"weight", 0, false);
		ucl_object_insert_key (elt, ucl_object_fromdouble (item->avg_time),
				"time", 0, false);
		ucl_object_insert_key (elt, ucl_object_fromint (item->frequency),
				"frequency", 0, false);

		ucl_object_insert_key (top, elt, k, 0, false);
	}

	efunc = ucl_object_emit_fd_funcs (fd);
	ret = ucl_object_emit_full (top, UCL_EMIT_JSON_COMPACT, efunc);
	ucl_object_emit_funcs_free (efunc);
	close (fd);

	return ret;
}

void
register_symbol_common (struct symbols_cache *cache,
	const gchar *name,
	double weight,
	gint priority,
	symbol_func_t func,
	gpointer user_data,
	enum rspamd_symbol_type type)
{
	struct cache_item *item = NULL;
	GList *cur;
	struct metric *m;
	struct rspamd_symbol_def *s;
	gboolean skipped, ghost = (weight == 0.0);

	g_assert (cache != NULL);

	if (g_hash_table_lookup (cache->items_by_symbol, name) != NULL) {
		msg_err ("skip duplicate symbol registration for %s", name);
		return;
	}

	item = rspamd_mempool_alloc0_shared (cache->static_pool,
			sizeof (struct cache_item));
	/*
	 * We do not share cd to skip locking, instead we'll just calculate it on
	 * save or accumulate
	 */
	item->cd = rspamd_mempool_alloc0 (cache->static_pool,
			sizeof (struct counter_data));

	item->symbol = rspamd_mempool_strdup (cache->static_pool, name);
	item->func = func;
	item->user_data = user_data;
	item->priority = priority;
	item->type = type;

	/* Handle weight using default metric */
	if (cache->cfg && cache->cfg->default_metric &&
		(s =
		g_hash_table_lookup (cache->cfg->default_metric->symbols,
		name)) != NULL) {
		item->weight = weight * (*s->weight_ptr);
	}
	else {
		item->weight = weight;
	}

	if (item->weight < 0 && item->priority == 0) {
		/* Make priority for negative weighted symbols */
		item->priority = 1;
	}

	/* Check whether this item is skipped */
	skipped = !ghost;
	if (item->type == SYMBOL_TYPE_NORMAL &&
			g_hash_table_lookup (cache->cfg->metrics_symbols, name) == NULL) {
		cur = g_list_first (cache->cfg->metrics_list);
		while (cur) {
			m = cur->data;

			if (m->accept_unknown_symbols) {
				GList *mlist;

				skipped = FALSE;
				item->weight = weight * (m->unknown_weight);
				s = rspamd_mempool_alloc0 (cache->static_pool,
						sizeof (*s));
				s->name = item->symbol;
				s->weight_ptr = &item->weight;
				g_hash_table_insert (m->symbols, item->symbol, s);
				mlist = g_hash_table_lookup (cache->cfg->metrics_symbols, name);
				mlist = g_list_prepend (mlist, m);
				g_hash_table_insert (cache->cfg->metrics_symbols,
						item->symbol, mlist);

				msg_info ("adding unknown symbol %s to metric %s", name,
						m->name);
			}

			cur = g_list_next (cur);
		}
	}
	else {
		skipped = FALSE;
	}

	if (skipped) {
		item->type = SYMBOL_TYPE_SKIPPED;
		msg_warn ("symbol %s is not registered in any metric, so skip its check",
				name);
	}

	if (ghost) {
		msg_debug ("symbol %s is registered as ghost symbol, it won't be inserted "
				"to any metric", name);
	}

	item->id = cache->used_items;
	cache->used_items ++;
	msg_debug ("used items: %d, added symbol: %s", cache->used_items, name);
	rspamd_set_counter (item, 0);
	g_hash_table_insert (cache->items_by_symbol, item->symbol, item);
	g_ptr_array_add (cache->items_by_order, item);
}

void
register_symbol (struct symbols_cache *cache, const gchar *name, double weight,
	symbol_func_t func, gpointer user_data)
{
	register_symbol_common (cache,
		name,
		weight,
		0,
		func,
		user_data,
		SYMBOL_TYPE_NORMAL);
}

void
register_virtual_symbol (struct symbols_cache *cache,
	const gchar *name,
	double weight)
{
	register_symbol_common (cache,
		name,
		weight,
		0,
		NULL,
		NULL,
		SYMBOL_TYPE_VIRTUAL);
}

void
register_callback_symbol (struct symbols_cache *cache,
	const gchar *name,
	double weight,
	symbol_func_t func,
	gpointer user_data)
{
	register_symbol_common (cache,
		name,
		weight,
		0,
		func,
		user_data,
		SYMBOL_TYPE_CALLBACK);
}

void
register_callback_symbol_priority (struct symbols_cache *cache,
	const gchar *name,
	double weight,
	gint priority,
	symbol_func_t func,
	gpointer user_data)
{
	register_symbol_common (cache,
		name,
		weight,
		priority,
		func,
		user_data,
		SYMBOL_TYPE_CALLBACK);
}

void
rspamd_symbols_cache_destroy (struct symbols_cache *cache)
{
	if (cache != NULL) {

		if (cache->cfg->cache_filename) {
			/* Try to sync values to the disk */
			if (!rspamd_symbols_cache_save_items (cache,
					cache->cfg->cache_filename)) {
				msg_err ("cannot save cache data to %s",
						cache->cfg->cache_filename);
			}
		}

		g_hash_table_destroy (cache->items_by_symbol);
		rspamd_mempool_delete (cache->static_pool);

		g_slice_free1 (sizeof (*cache), cache);
	}
}

struct symbols_cache*
rspamd_symbols_cache_new (void)
{
	struct symbols_cache *cache;

	cache = g_slice_alloc0 (sizeof (struct symbols_cache));
	cache->static_pool =
			rspamd_mempool_new (rspamd_mempool_suggest_size ());
	cache->items_by_symbol = g_hash_table_new (rspamd_str_hash,
			rspamd_str_equal);

	return cache;
}

gboolean
init_symbols_cache (struct symbols_cache* cache,
		struct rspamd_config *cfg)
{
	gboolean res;

	g_assert (cache != NULL);
	cache->cfg = cfg;

	/* Just in-memory cache */
	if (cfg->cache_filename == NULL) {
		post_cache_init (cache);
		return TRUE;
	}

	/* Copy saved cache entries */
	res = rspamd_symbols_cache_load_items (cache, cfg->cache_filename);

	post_cache_init (cache);

	return res;
}

static gboolean
check_debug_symbol (struct rspamd_config *cfg, const gchar *symbol)
{
	GList *cur;

	cur = cfg->debug_symbols;
	while (cur) {
		if (strcmp (symbol, (const gchar *)cur->data) == 0) {
			return TRUE;
		}
		cur = g_list_next (cur);
	}

	return FALSE;
}

static void
rspamd_symbols_cache_metric_cb (gpointer k, gpointer v, gpointer ud)
{
	struct symbols_cache *cache = (struct symbols_cache *)ud;
	const gchar *sym = k;
	struct rspamd_symbol_def *s = (struct rspamd_symbol_def *)v;
	gdouble weight;
	struct cache_item *item;

	weight = *s->weight_ptr;
	item = g_hash_table_lookup (cache->items_by_symbol, sym);

	if (item) {
		item->metric_weight = weight;
	}
}

gboolean
validate_cache (struct symbols_cache *cache,
	struct rspamd_config *cfg,
	gboolean strict)
{
	struct cache_item *item;
	GList *cur, *metric_symbols;

	if (cache == NULL) {
		msg_err ("empty cache is invalid");
		return FALSE;
	}

	/* Now check each metric item and find corresponding symbol in a cache */
	metric_symbols = g_hash_table_get_keys (cfg->metrics_symbols);
	cur = metric_symbols;
	while (cur) {
		item = g_hash_table_lookup (cache->items_by_symbol, cur->data);

		if (item == NULL) {
			msg_warn (
				"symbol '%s' has its score defined but there is no "
				"corresponding rule registered",
				cur->data);
			if (strict) {
				g_list_free (metric_symbols);
				return FALSE;
			}
		}
		cur = g_list_next (cur);
	}
	g_list_free (metric_symbols);

	/* Now adjust symbol weights according to default metric */
	if (cfg->default_metric != NULL) {
		g_hash_table_foreach (cfg->default_metric->symbols,
			rspamd_symbols_cache_metric_cb,
			cache);
	}

	post_cache_init (cache);

	return TRUE;
}

gboolean
call_symbol_callback (struct rspamd_task * task,
	struct symbols_cache * cache,
	gpointer *save)
{
	double t1, t2;
	guint64 diff;
	struct cache_item *item = NULL;
	guint *s = *save;
	guint idx;

	g_assert (cache != NULL);

	if (s == NULL) {
		s =
			rspamd_mempool_alloc0 (task->task_pool,
				sizeof (gpointer));
		*save = s;
	}

	idx = GPOINTER_TO_INT (s);

	if (idx >= cache->used_items) {
		/* All symbols are processed */
		return FALSE;
	}

	item = g_ptr_array_index (cache->items_by_order, idx);

	if (!item) {
		return FALSE;
	}

	if (item->type == SYMBOL_TYPE_NORMAL || item->type == SYMBOL_TYPE_CALLBACK) {
		t1 = rspamd_get_ticks ();

		if (G_UNLIKELY (check_debug_symbol (task->cfg, item->symbol))) {
			rspamd_log_debug (rspamd_main->logger);
			item->func (task, item->user_data);
			rspamd_log_nodebug (rspamd_main->logger);
		}
		else {
			item->func (task, item->user_data);
		}

		t2 = rspamd_get_ticks ();

		diff = (t2 - t1) * 1000000;
		rspamd_set_counter (item, diff);
	}

	idx ++;
	s = GINT_TO_POINTER (idx);
	*save = s;

	return TRUE;

}
