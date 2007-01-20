#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdarg.h>
#include <malloc.h>
#include <string.h>
#include <limits.h>
#include <sys/time.h>
#include <time.h>

#include <gmain.h>

struct timeout {
	guint id;
	guint interval;
	struct timeval expiration;
	gpointer data;
	GSourceFunc function;
};

struct _GIOChannel {
	int fd;
	gboolean closed;
	gboolean close_on_unref;
};

struct _GMainContext {
	guint next_id;
	glong next_timeout;

	GSList *timeouts;
	GSList *proc_timeouts;
	gboolean timeout_lock;

	GSList *watches;
	GSList *proc_watches;
	gboolean watch_lock;
};

struct _GMainLoop {
	gboolean is_running;
	GMainContext *context;
};

GIOError g_io_channel_read(GIOChannel *channel, gchar *buf, gsize count, gsize *bytes_read)
{
	int fd = channel->fd;
	gssize result;

	if (channel->closed)
		return G_IO_STATUS_ERROR;

	/* At least according to the Debian manpage for read */
	if (count > SSIZE_MAX)
		count = SSIZE_MAX;

retry:
	result = read (fd, buf, count);

	if (result < 0) {
		*bytes_read = 0;

		switch (errno) {
#ifdef EINTR
		case EINTR:
			goto retry;
#endif
#ifdef EAGAIN
		case EAGAIN:
			return G_IO_STATUS_AGAIN;
#endif
		default:
			return G_IO_STATUS_ERROR;
		}
	}

	*bytes_read = result;

	return (result > 0) ? G_IO_STATUS_NORMAL : G_IO_STATUS_EOF;
}

GIOError g_io_channel_write(GIOChannel *channel, const gchar *buf, gsize count,
				gsize *bytes_written)
{
	/* Not implemented */
	return G_IO_STATUS_ERROR;
}

void g_io_channel_close(GIOChannel *channel)
{
	if (!channel || channel->closed)
		return;

	close(channel->fd);

	channel->closed = TRUE;
}

void g_io_channel_unref(GIOChannel *channel)
{
	if (!channel)
		return;

	if (channel->close_on_unref && channel->fd >= 0)
		g_io_channel_close(channel);

	free(channel);
}

GIOChannel *g_io_channel_unix_new(int fd)
{
	GIOChannel *channel;

	channel = malloc(sizeof(GIOChannel));
	if (!channel)
		return NULL;

	memset(channel, 0, sizeof(GIOChannel));

	channel->fd = fd;

	return channel;
}

void g_io_channel_set_close_on_unref(GIOChannel *channel, gboolean do_close)
{
	channel->close_on_unref = do_close;
}

gint g_io_channel_unix_get_fd(GIOChannel *channel)
{
	if (channel->closed)
		return -1;

	return channel->fd;
}

struct watch {
	guint id;
	GIOChannel *channel;
	gint priority;
	GIOCondition condition;
	short *revents;
	GIOFunc func;
	gpointer user_data;
	GDestroyNotify destroy;
};

static GMainContext *default_context = NULL;

static void watch_free(struct watch *watch)
{
	if (watch->destroy)
		watch->destroy(watch->user_data);
	free(watch);
}

static GMainContext *g_main_context_default()
{
	if (default_context)
		return default_context;

	default_context = malloc(sizeof(GMainContext));
	if (!default_context)
		return NULL;

	memset(default_context, 0, sizeof(GMainContext));

	default_context->next_timeout = -1;
	default_context->next_id = 1;

	return default_context;
}

gboolean g_source_remove(guint tag)
{
	/* Not implemented yet */
	return FALSE;
}

void g_io_remove_watch(guint id)
{
	GMainContext *context = g_main_context_default();
	GSList *l;
	struct watch *w;

	if (!context)
		return;

	for (l = context->watches; l != NULL; l = l->next) {
		w = l->data;	

		if (w->id != id)
			continue;

		context->watches = g_slist_remove(context->watches, w);
		watch_free(w);

		return;
	}

	for (l = context->proc_watches; l != NULL; l = l->next) {
		w = l->data;	

		if (w->id != id)
			continue;

		context->proc_watches = g_slist_remove(context->proc_watches, w);
		watch_free(w);

		return;
	}
}

int watch_prio_cmp(struct watch *w1, struct watch *w2)
{
	return w1->priority - w2->priority;
}

#define watch_list_add(l, w) g_slist_insert_sorted((l), (w), (GCompareFunc) watch_prio_cmp)

guint g_io_add_watch_full(GIOChannel *channel, gint priority,
				GIOCondition condition, GIOFunc func,
				gpointer user_data, GDestroyNotify notify)
{
	struct watch *watch;
	GMainContext *context = g_main_context_default();

	if (!context)
		return 0;
       
	watch = malloc(sizeof(struct watch));
	if (!watch)
		return 0;

	watch->id = context->next_id++;
	watch->channel = channel;
	watch->priority = priority;
	watch->condition = condition;
	watch->func = func;
	watch->user_data = user_data;
	watch->destroy = notify;

	if (context->watch_lock)
		context->proc_watches = watch_list_add(context->proc_watches, watch);
	else
		context->watches = watch_list_add(context->watches, watch);

	return watch->id;
}

guint g_io_add_watch(GIOChannel *channel, GIOCondition condition,
					GIOFunc func, gpointer user_data)
{
	return g_io_add_watch_full(channel, 0, condition,
						func, user_data, NULL);
}

GMainLoop *g_main_loop_new(GMainContext *context, gboolean is_running)
{
	GMainLoop *ml;

	if (!context)
		context = g_main_context_default();

	if (!context)
		return NULL;

	ml = malloc(sizeof(GMainLoop));
	if (!ml)
		return NULL;

	memset(ml, 0, sizeof(GMainLoop));

	ml->context = context;
	ml->is_running = is_running;

	return ml;
}

static void timeout_handlers_prepare(GMainContext *context)
{
	GSList *l;
	struct timeval tv;
	glong msec, timeout = LONG_MAX;

	gettimeofday(&tv, NULL);

	for (l = context->timeouts; l != NULL; l = l->next) {
		struct timeout *t = l->data;

		/* calculate the remainning time */
		msec = (t->expiration.tv_sec - tv.tv_sec) * 1000 +
				(t->expiration.tv_usec - tv.tv_usec) / 1000;
		if (msec < 0)
			msec = 0;

		timeout = MIN_TIMEOUT(timeout, msec);
	}

	/* set to min value found or NO timeout */
	context->next_timeout = (timeout != LONG_MAX ? timeout : -1);
}

static int ptr_cmp(const void *t1, const void *t2)
{
	return t1 - t2;
}

static void timeout_handlers_check(GMainContext *context)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);

	context->timeout_lock = TRUE;

	while (context->timeouts) {
		struct timeout *t = context->timeouts->data;
		glong secs, msecs;
		gboolean ret;

		if (timercmp(&tv, &t->expiration, <)) {
			context->timeouts = g_slist_remove(context->timeouts, t);
			context->proc_timeouts = g_slist_append(context->proc_timeouts, t);
			continue;
		}

		ret = t->function(t->data);

		/* Check if the handler was removed/freed by the callback
		 * function */
		if (!g_slist_find_custom(context->timeouts, t, ptr_cmp))
			continue;

		context->timeouts = g_slist_remove(context->timeouts, t);

		if (!ret) {
			free(t);
			continue;
		}

		/* update the next expiration time */
		secs = t->interval / 1000;
		msecs = t->interval - secs * 1000;

		t->expiration.tv_sec = tv.tv_sec + secs;
		t->expiration.tv_usec = tv.tv_usec + msecs * 1000;
		if (t->expiration.tv_usec >= 1000000) {
			t->expiration.tv_usec -= 1000000;
			t->expiration.tv_sec++;
		}

		context->proc_timeouts = g_slist_append(context->proc_timeouts, t);
	}

	context->timeouts = context->proc_timeouts;
	context->proc_timeouts = NULL;
	context->timeout_lock = FALSE;
}

void g_main_loop_run(GMainLoop *loop)
{
	int open_max = sysconf(_SC_OPEN_MAX);
	struct pollfd *ufds;
	GMainContext *context = loop->context;

	ufds = malloc(open_max * sizeof(struct pollfd));
	if (!ufds)
		return;

	loop->is_running = TRUE;

	while (loop->is_running) {
		int nfds;
		GSList *l;
		struct watch *w;

		for (nfds = 0, l = context->watches; l != NULL; l = l->next, nfds++) {
			w = l->data;
			ufds[nfds].fd = w->channel->fd;
			ufds[nfds].events = w->condition;
			ufds[nfds].revents = 0;
			w->revents = &ufds[nfds].revents;
		}

		/* calculate the next timeout */
		timeout_handlers_prepare(context);

		if (poll(ufds, nfds, context->next_timeout) < 0)
			continue;

		context->watch_lock = TRUE;

		while (context->watches) {
			gboolean ret;

			w = context->watches->data;

			if (!*w->revents) {
				context->watches = g_slist_remove(context->watches, w);
				context->proc_watches = watch_list_add(context->proc_watches, w);
				continue;
			}

			ret = w->func(w->channel, *w->revents, w->user_data);

			/* Check if the watch was removed/freed by the callback
			 * function */
			if (!g_slist_find_custom(context->watches, w, ptr_cmp))
				continue;

			context->watches = g_slist_remove(context->watches, w);

			if (!ret) {
				watch_free(w);
				continue;
			}

			context->proc_watches = watch_list_add(context->proc_watches, w);
		}

		context->watches = context->proc_watches;
		context->proc_watches = NULL;
		context->watch_lock = FALSE;

		/* check expired timers */
		timeout_handlers_check(loop->context);
	}

	free(ufds);
}

void g_main_loop_quit(GMainLoop *loop)
{
	loop->is_running = FALSE;
}

void g_main_loop_unref(GMainLoop *loop)
{
	if (!loop->context)
		return;

	g_slist_foreach(loop->context->watches, (GFunc)watch_free, NULL);
	g_slist_free(loop->context->watches);

	g_slist_foreach(loop->context->timeouts, (GFunc)free, NULL);
	g_slist_free(loop->context->timeouts);

	free(loop->context);
	loop->context = NULL;
}

guint g_timeout_add(guint interval, GSourceFunc function, gpointer data)
{
	GMainContext *context = g_main_context_default();
	struct timeval tv;
	guint secs;
	guint msecs;
	struct timeout *t;

	if (!context || !function)
		return 0;

	t = malloc(sizeof(*t));

	if (!t)
		return 0;

	memset(t, 0, sizeof(*t));
	t->interval = interval;
	t->function = function;
	t->data = data;

	gettimeofday(&tv, NULL);

	secs = interval /1000;
	msecs = interval - secs * 1000;

	t->expiration.tv_sec = tv.tv_sec + secs;
	t->expiration.tv_usec = tv.tv_usec + msecs * 1000;

	if (t->expiration.tv_usec >= 1000000) {
		t->expiration.tv_usec -= 1000000;
		t->expiration.tv_sec++;
	}

	/* attach the timeout the default context */
	t->id = context->next_id++;

	if (context->timeout_lock)
		context->proc_timeouts = g_slist_prepend(context->proc_timeouts, t);
	else
		context->timeouts = g_slist_prepend(context->timeouts, t);

	return t->id;
}

gint g_timeout_remove(const guint id)
{
	GMainContext *context = g_main_context_default();
	GSList *l;
	struct timeout *t;

	if (!context)
		return -1;

	l = context->timeouts;

	while (l) {
		t = l->data;
		l = l->next;

		if (t->id != id)
			continue;

		context->timeouts = g_slist_remove(context->timeouts, t);
		free(t);

		return 0;
	}

	l = context->proc_timeouts;

	while (l) {
		t = l->data;
		l = l->next;

		if (t->id != id)
			continue;

		context->proc_timeouts = g_slist_remove(context->proc_timeouts, t);
		free(t);

		return 0;
	}

	return -1;
}

guint g_idle_add(GSourceFunc func, gpointer user_data)
{
	/* Not implemented */
	return 0;
}

/* GError */
void g_error_free(GError *err)
{
	g_free(err->message);
	g_free(err);
}

/* Spawning related functions */

gboolean g_spawn_async(const gchar *working_directory,
			gchar **argv, gchar **envp,
			GSpawnFlags flags,
			GSpawnChildSetupFunc child_setup,
			gpointer user_data,
			GPid *child_pid,
			GError **error)
{
	/* Not implemented */
	return FALSE;
}

void g_spawn_close_pid(GPid pid)
{
	return;
}

guint g_child_watch_add(GPid pid, GChildWatchFunc func, gpointer user_data)
{
	/* Not implemented */
	return 0;
}

/* UTF-8 Validation: approximate copy/paste from glib2. */

#define UNICODE_VALID(c)			\
	((c) < 0x110000 &&			\
	(((c) & 0xFFFFF800) != 0xD800) &&	\
	((c) < 0xFDD0 || (c) > 0xFDEF) &&	\
	((c) & 0xFFFE) != 0xFFFE)

#define CONTINUATION_CHAR(c, val)				\
	do {							\
  		if (((c) & 0xc0) != 0x80) /* 10xxxxxx */	\
  			goto failed;				\
  		(val) <<= 6;					\
  		(val) |= (c) & 0x3f;				\
	} while (0)

#define INCREMENT_AND_CHECK_MAX(p, i, max_len)					\
	do {									\
		(i)++;								\
		if ((p)[(i)] == '\0' || ((max_len) >= 0 && (i) >= (max_len)))	\
			goto failed;						\
	} while (0)
				

gboolean g_utf8_validate(const gchar *str, gssize max_len, const gchar **end)
{
	unsigned long val, min, i;
	const unsigned char *p, *last;

	min = val = 0;

	for (p = (unsigned char *) str, i = 0; p[i]; i++) {
		if (max_len >= 0 && i >= max_len)
			break;

		if (p[i] < 128)
			continue;

		last = &p[i];

		if ((p[i] & 0xe0) == 0xc0) { /* 110xxxxx */
			if ((p[i] & 0x1e) == 0)
				goto failed;
			INCREMENT_AND_CHECK_MAX(p, i, max_len);
			if ((p[i] & 0xc0) != 0x80)
				goto failed; /* 10xxxxxx */
		} else {
			if ((p[i] & 0xf0) == 0xe0) {
				/* 1110xxxx */
				min = (1 << 11);
				val = p[i] & 0x0f;
				goto two_remaining;
			} else if ((p[i] & 0xf8) == 0xf0) {
				/* 11110xxx */
				min = (1 << 16);
				val = p[i] & 0x07;
			} else
				goto failed;

			INCREMENT_AND_CHECK_MAX(p, i, max_len);
			CONTINUATION_CHAR(p[i], val);
two_remaining:
			INCREMENT_AND_CHECK_MAX(p, i, max_len);
			CONTINUATION_CHAR(p[i], val);

			INCREMENT_AND_CHECK_MAX(p, i, max_len);
			CONTINUATION_CHAR(p[i], val);

			if (val < min || !UNICODE_VALID(val))
				goto failed;
		} 
	}

	if (end)
		*end = (const gchar *) &p[i];

	return TRUE;

failed:
	if (end)
		*end = (const gchar *) last;

	return FALSE;
}

/* GSList functions */

GSList *g_slist_append(GSList *list, void *data)
{
	GSList *entry, *tail;

	entry = malloc(sizeof(GSList));
	/* FIXME: this currently just silently fails */
	if (!entry)
		return list;

	entry->data = data;
	entry->next = NULL;

	if (!list)
		return entry;

	/* Find the end of the list */
	for (tail = list; tail->next; tail = tail->next);

	tail->next = entry;

	return list;
}

GSList *g_slist_prepend(GSList *list, void *data)
{
	GSList *entry;

	entry = malloc(sizeof(GSList));
	/* FIXME: this currently just silently fails */
	if (!entry)
		return list;

	entry->data = data;
	entry->next = list;

	return entry;
}

GSList *g_slist_insert_sorted(GSList *list, void *data, GCompareFunc cmp_func)
{
	GSList *tmp, *prev, *entry;
	int cmp;

	entry = malloc(sizeof(GSList));
	if (!entry)
		return list;

	entry->data = data;
	entry->next = NULL;

	if (!list)
		return entry;

	prev = NULL;
	tmp = list;

	cmp = cmp_func(data, tmp->data);

	while (tmp->next && cmp > 0) {
		prev = tmp;
		tmp = tmp->next;

		cmp = cmp_func(data, tmp->data);
	}

	if (!tmp->next && cmp > 0) {
		tmp->next = entry;
		return list;
	}

	if (prev) {
		prev->next = entry;
		entry->next = tmp;
		return list;
	} else {
		entry->next = list;
		return entry;
	}
}

GSList *g_slist_remove(GSList *list, void *data)
{
	GSList *l, *next, *prev = NULL, *match = NULL;

	if (!list)
		return NULL;

	for (l = list; l != NULL; l = l->next) {
		if (l->data == data) {
			match = l;
			break;
		}
		prev = l;
	}

	if (!match)
		return list;

	next = match->next;

	free(match);

	/* If the head was removed, return the next element */
	if (!prev)
		return next;

	prev->next = next;

	return list;
}

GSList *g_slist_find(GSList *list, gconstpointer data)
{
	GSList *l;

	for (l = list; l != NULL; l = l->next) {
		if (l->data == data)
			return l;
	}

	return NULL;
}

GSList *g_slist_find_custom(GSList *list, const void *data,
			GCompareFunc cmp_func)
{
	GSList *l;

	for (l = list; l != NULL; l = l->next) {
		if (!cmp_func(l->data, data))
			return l;
	}

	return NULL;
}

static GSList *g_slist_sort_merge(GSList *l1, GSList *l2,
					GCompareFunc cmp_func)
{
	GSList list, *l;
	int cmp;

	l = &list;

	while (l1 && l2) {
		cmp = cmp_func(l1->data, l2->data);

		if (cmp <= 0) {
			l = l->next = l1;
			l1 = l1->next;
		} else {
			l = l->next = l2;
			l2 = l2->next;
		}
	}

	l->next = l1 ? l1 : l2;

	return list.next;
}

GSList *g_slist_sort(GSList *list, GCompareFunc cmp_func)
{
	GSList *l1, *l2;

	if (!list || !list->next) 
		return list;

	l1 = list; 
	l2 = list->next;

	while ((l2 = l2->next) != NULL) {
		if ((l2 = l2->next) == NULL) 
			break;
		l1 = l1->next;
	}

	l2 = l1->next; 
	l1->next = NULL;

	return g_slist_sort_merge(g_slist_sort(list, cmp_func),
				g_slist_sort(l2, cmp_func), cmp_func);
}

int g_slist_length(GSList *list)
{
	int len;

	for (len = 0; list != NULL; list = list->next)
		len++;

	return len;
}

void g_slist_foreach(GSList *list, GFunc func, void *user_data)
{
	while (list) {
		GSList *next = list->next;
		func(list->data, user_data);
		list = next;
	}
}

void g_slist_free(GSList *list)
{
	GSList *l, *next;

	for (l = list; l != NULL; l = next) {
		next = l->next;
		free(l);
	}
}

/* Memory allocation functions */

gpointer g_malloc(gulong n_bytes)
{
	gpointer mem;

	if (!n_bytes)
		return NULL;

	mem = malloc((size_t) n_bytes);
	if (!mem) {
		fprintf(stderr, "g_malloc: failed to allocate %lu bytes",
				n_bytes);
		abort();
	}

	return mem;
}

gpointer g_malloc0(gulong n_bytes)
{
	gpointer mem;

	if (!n_bytes)
		return NULL;

	mem = g_malloc(n_bytes);

	memset(mem, 0, (size_t) n_bytes);

	return mem;
}

gpointer g_try_malloc(gulong n_bytes)
{
	if (!n_bytes)
		return NULL;

	return malloc((size_t) n_bytes);
}

gpointer g_try_malloc0(gulong n_bytes)
{
	gpointer mem;

	mem = g_try_malloc(n_bytes);
	if (mem)
		memset(mem, 0, (size_t) n_bytes);

	return mem;
}

void g_free(gpointer mem)
{
	if (mem)
		free(mem);
}

gchar *g_strdup(const gchar *str)
{
	gchar *s;

	if (!str)
		return NULL;

	s = strdup(str);
	if (!s) {
		fprintf(stderr, "strdup: failed to allocate new string");
		abort();
	}

	return s;
}

gchar *g_strdup_printf(const gchar *format, ...)
{
	gchar str[1024];
	va_list ap;

	memset(str, 0, sizeof(str));

	va_start(ap, format);

	vsnprintf(str, sizeof(str) - 1, format, ap);

	va_end(ap);

	return g_strdup(str);
}

void g_strfreev(gchar **str_array)
{
	int i;

	if (!str_array)
		return;
	
	for (i = 0; str_array[i] != NULL; i++)
		g_free(str_array[i]);

	g_free(str_array);
}

/* g_shell_* */

gboolean g_shell_parse_argv(const gchar *command_line,
				gint *argcp,
				gchar ***argvp,
				GError **error)
{
	/* Not implemented */
	return FALSE;
}

/* GKeyFile */

typedef gpointer GHashTable;

typedef struct _GKeyFileGroup GKeyFileGroup;

struct _GKeyFile {
	GSList *groups;

	GKeyFileGroup *start_group;
	GKeyFileGroup *current_group;

	/* Holds up to one line of not-yet-parsed data */
	gchar *parse_buffer;

	/* Used for sizing the output buffer during serialization */
	gsize approximate_size;

	gchar list_separator;

	GKeyFileFlags flags;
};

typedef struct _GKeyFileKeyValuePair GKeyFileKeyValuePair;

struct _GKeyFileGroup {
	/* NULL for above first group (which will be comments) */
	const gchar *name;

	/* Special comment that is stuck to the top of a group */
	GKeyFileKeyValuePair *comment;

	GSList *key_value_pairs;

	/* Used in parallel with key_value_pairs for
	 * increased lookup performance
	 */
	GHashTable *lookup_map;
};

struct _GKeyFileKeyValuePair {
	gchar *key;  /* NULL for comments */
	gchar *value;
};

GKeyFile *g_key_file_new(void)
{
	/* Not implemented */
	return NULL;
}

void g_key_file_free(GKeyFile *key_file)
{
	/* Not implemented fully */
	g_free(key_file);
}

gboolean g_key_file_load_from_file(GKeyFile *key_file,
				const gchar *file,
				GKeyFileFlags flags,
				GError **error)
{
	/* Not implemented */
	return FALSE;
}

gchar *g_key_file_get_string(GKeyFile *key_file,
				const gchar *group_name,
				const gchar *key,
				GError **error)
{
	/* Not implemented */
	return NULL;
}

gboolean g_key_file_get_boolean(GKeyFile *key_file,
				const gchar *group_name,
				const gchar *key,
				GError **error)
{
	/* Not implemented */
	return FALSE;
}

