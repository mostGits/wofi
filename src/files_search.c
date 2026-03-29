#include <files_search.h>

#include <utils.h>

#include <stdbool.h>
#include <stdio.h>
#include <dirent.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pthread.h>
#include <stdint.h>

#include <glib.h>

struct file_entry {
	char* full_path;
	char* display;
	bool is_dir;
};

/* Full tree index for the current base directory (disk walk only when base changes). */
static struct file_entry* master_entries = NULL;
static size_t master_count = 0;
static char* master_base = NULL;

/* Subset of master_entries indices matching the current query (rebuilt every filter update). */
static size_t* match_indices = NULL;
static size_t match_count = 0;

static bool case_insensitive = false;
static size_t max_matches = 200;
static FilesSearchListing listing_mode = FILES_SEARCH_FILES_ONLY;
static int indexed_listing = -1;
static int master_index_format = -1;
static char* ext_filter = NULL;

/* Bump when index contents/rules change so cached master is rebuilt. */
#define FILES_SEARCH_INDEX_FORMAT 3

/* Incremented when a new index is requested; workers compare to detect stale/cancelled work. */
static volatile uint64_t index_generation = 0;
static void (*index_ready_cb)(void*) = NULL;
static void* index_ready_data = NULL;
static volatile int index_outstanding = 0;
static pthread_mutex_t index_lock = PTHREAD_MUTEX_INITIALIZER;
/* Latest base to index when a walk is already running (replaces older pending). */
static char* pending_index_base = NULL;

struct index_thread_arg {
	char* base;
	uint64_t gen;
	FilesSearchListing mode;
};

struct index_merge_payload {
	struct file_entry* entries;
	size_t count;
	char* base;
	uint64_t gen;
	int listing;
};

static void free_master(void);

static void free_entries_array(struct file_entry* e, size_t n) {
	for(size_t i = 0; i < n; ++i) {
		free(e[i].full_path);
		free(e[i].display);
	}
	free(e);
}

void files_search_set_index_ready_callback(void (*cb)(void*), void* user_data) {
	index_ready_cb = cb;
	index_ready_data = user_data;
}

bool files_search_index_busy(void) {
	pthread_mutex_lock(&index_lock);
	bool busy = index_outstanding > 0 || pending_index_base != NULL;
	pthread_mutex_unlock(&index_lock);
	return busy;
}

static void try_start_pending_index(void);
static void schedule_index_async(char* base_norm);

static gboolean merge_index_idle(gpointer data) {
	struct index_merge_payload* p = data;
	pthread_mutex_lock(&index_lock);
	index_outstanding--;
	pthread_mutex_unlock(&index_lock);

	if(p->gen != index_generation) {
		free_entries_array(p->entries, p->count);
		free(p->base);
		free(p);
		try_start_pending_index();
		return G_SOURCE_REMOVE;
	}

	free_master();
	master_entries = p->entries;
	master_count = p->count;
	master_base = p->base;
	indexed_listing = p->listing;
	master_index_format = FILES_SEARCH_INDEX_FORMAT;
	free(p);

	if(index_ready_cb != NULL) {
		index_ready_cb(index_ready_data);
	}
	try_start_pending_index();
	return G_SOURCE_REMOVE;
}

static const char* path_basename_ptr(const char* path) {
	const char* s = strrchr(path, '/');
	return s != NULL ? s + 1 : path;
}

static bool push_local(struct file_entry** arr, size_t* n, const char* path, bool is_dir, uint64_t my_gen) {
	if(((*n) & 0xffu) == 0u && index_generation != my_gen) {
		return false;
	}
	struct file_entry* tmp = realloc(*arr, sizeof(struct file_entry) * (*n + 1));
	if(tmp == NULL) {
		return false;
	}
	*arr = tmp;
	(*arr)[*n].full_path = strdup(path);
	(*arr)[*n].display = strdup(path_basename_ptr(path));
	if((*arr)[*n].display == NULL) {
		free((*arr)[*n].full_path);
		return false;
	}
	(*arr)[*n].is_dir = is_dir;
	++(*n);
	return true;
}

/* Skip hidden paths and heavy cache trees (not . / ..). */
static bool skip_dirent(const char* name, bool is_dir) {
	if(name[0] == '.') {
		return true;
	}
	if(is_dir) {
		if(strcmp(name, "node_modules") == 0) {
			return true;
		}
		if(strcmp(name, "__pycache__") == 0) {
			return true;
		}
	}
	return false;
}

static bool index_dir_worker(const char* dir, struct file_entry** arr, size_t* n, FilesSearchListing mode, uint64_t my_gen) {
	if(index_generation != my_gen) {
		return false;
	}

	DIR* d = opendir(dir);
	if(d == NULL) {
		return true;
	}

	struct dirent* e;
	while((e = readdir(d)) != NULL) {
		if(index_generation != my_gen) {
			closedir(d);
			return false;
		}

		if(strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
			continue;
		}

		char path[PATH_MAX];
		if(snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) >= (int) sizeof(path)) {
			continue;
		}

		struct stat st;
		if(stat(path, &st) == -1) {
			continue;
		}

		bool is_dir = S_ISDIR(st.st_mode);
		if(skip_dirent(e->d_name, is_dir)) {
			continue;
		}

		if(is_dir) {
			if(mode == FILES_SEARCH_FOLDERS_ONLY || mode == FILES_SEARCH_BOTH) {
				if(!push_local(arr, n, path, true, my_gen)) {
					closedir(d);
					return false;
				}
			}
			if(!index_dir_worker(path, arr, n, mode, my_gen)) {
				closedir(d);
				return false;
			}
		} else if(S_ISREG(st.st_mode)) {
			if(mode == FILES_SEARCH_FILES_ONLY || mode == FILES_SEARCH_BOTH) {
				if(!push_local(arr, n, path, false, my_gen)) {
					closedir(d);
					return false;
				}
			}
		}
	}

	closedir(d);
	return true;
}

static void* index_thread_fn(void* arg) {
	struct index_thread_arg* a = arg;
	struct file_entry* local = NULL;
	size_t n = 0;

	(void) index_dir_worker(a->base, &local, &n, a->mode, a->gen);

	if(index_generation != a->gen) {
		free_entries_array(local, n);
		free(a->base);
		free(a);
		pthread_mutex_lock(&index_lock);
		index_outstanding--;
		pthread_mutex_unlock(&index_lock);
		try_start_pending_index();
		return NULL;
	}

	struct index_merge_payload* p = malloc(sizeof(struct index_merge_payload));
	if(p == NULL) {
		free_entries_array(local, n);
		free(a->base);
		free(a);
		pthread_mutex_lock(&index_lock);
		index_outstanding--;
		pthread_mutex_unlock(&index_lock);
		try_start_pending_index();
		return NULL;
	}

	p->entries = local;
	p->count = n;
	p->base = strdup(a->base);
	p->gen = a->gen;
	p->listing = (int) a->mode;

	free(a->base);
	free(a);

	g_idle_add(merge_index_idle, p);
	return NULL;
}

static void try_start_pending_index(void) {
	char* b = NULL;
	pthread_mutex_lock(&index_lock);
	b = pending_index_base;
	pending_index_base = NULL;
	pthread_mutex_unlock(&index_lock);
	if(b != NULL) {
		schedule_index_async(b);
	}
}

static void schedule_index_async(char* base_norm) {
	++index_generation;
	uint64_t my_gen = index_generation;

	pthread_mutex_lock(&index_lock);
	if(index_outstanding > 0) {
		free(pending_index_base);
		pending_index_base = base_norm;
		pthread_mutex_unlock(&index_lock);
		return;
	}
	index_outstanding++;
	pthread_mutex_unlock(&index_lock);

	struct index_thread_arg* a = malloc(sizeof(struct index_thread_arg));
	if(a == NULL) {
		pthread_mutex_lock(&index_lock);
		index_outstanding--;
		pthread_mutex_unlock(&index_lock);
		free(base_norm);
		try_start_pending_index();
		return;
	}
	a->base = base_norm;
	a->gen = my_gen;
	a->mode = listing_mode;

	pthread_t th;
	if(pthread_create(&th, NULL, index_thread_fn, a) != 0) {
		pthread_mutex_lock(&index_lock);
		index_outstanding--;
		pthread_mutex_unlock(&index_lock);
		free(a->base);
		free(a);
		try_start_pending_index();
		return;
	}
	pthread_detach(th);
}

static void free_master(void) {
	for(size_t i = 0; i < master_count; ++i) {
		free(master_entries[i].full_path);
		free(master_entries[i].display);
	}
	free(master_entries);
	master_entries = NULL;
	master_count = 0;
	free(master_base);
	master_base = NULL;
	indexed_listing = -1;
	master_index_format = -1;
}

static void free_matches(void) {
	free(match_indices);
	match_indices = NULL;
	match_count = 0;
}

void files_search_clear_index(void) {
	++index_generation;
	pthread_mutex_lock(&index_lock);
	free(pending_index_base);
	pending_index_base = NULL;
	pthread_mutex_unlock(&index_lock);
	free_master();
	free_matches();
	free(ext_filter);
	ext_filter = NULL;
}

void files_search_set_options(FilesSearchListing listing, const char* extension_filter) {
	listing_mode = listing;
	free(ext_filter);
	ext_filter = NULL;
	if(extension_filter != NULL) {
		while(*extension_filter == ' ' || *extension_filter == '\t') {
			++extension_filter;
		}
	}
	if(extension_filter != NULL && *extension_filter != '\0') {
		ext_filter = strdup(extension_filter);
	}
}

void files_search_configure(const char* extra_root_dirs_csv, bool insensitive, size_t max_match_rows) {
	(void) extra_root_dirs_csv;
	case_insensitive = insensitive;
	if(max_match_rows > 0) {
		max_matches = max_match_rows;
	}
}

static bool path_passes_ext(const char* path, bool is_dir) {
	if(is_dir) {
		return true;
	}
	if(ext_filter == NULL || *ext_filter == '\0') {
		return true;
	}

	const char* bn = strrchr(path, '/');
	bn = bn != NULL ? bn + 1 : path;

	char* dup = strdup(ext_filter);
	if(dup == NULL) {
		return true;
	}

	bool ok = false;
	char* saveptr = NULL;
	for(char* tok = strtok_r(dup, ",", &saveptr); tok != NULL; tok = strtok_r(NULL, ",", &saveptr)) {
		while(*tok == ' ' || *tok == '\t') {
			++tok;
		}
		char* end = tok + strlen(tok);
		while(end > tok && (end[-1] == ' ' || end[-1] == '\t')) {
			*--end = '\0';
		}
		if(*tok == '\0') {
			continue;
		}

		char pat[256];
		if(tok[0] == '.') {
			if(snprintf(pat, sizeof(pat), "%s", tok) >= (int) sizeof(pat)) {
				continue;
			}
		} else {
			if(snprintf(pat, sizeof(pat), ".%s", tok) >= (int) sizeof(pat)) {
				continue;
			}
		}

		size_t plen = strlen(pat);
		size_t bnlen = strlen(bn);
		if(bnlen >= plen && strcasecmp(bn + bnlen - plen, pat) == 0) {
			ok = true;
			break;
		}
	}
	free(dup);
	return ok;
}

static bool str_contains(const char* hay, const char* needle) {
	if(needle == NULL || *needle == '\0') {
		return true;
	}
	if(hay == NULL) {
		return false;
	}
	if(case_insensitive) {
		return strcasestr(hay, needle) != NULL;
	}
	return strstr(hay, needle) != NULL;
}

/* Match query against full path and basename (trim spaces); covers "name only" typing. */
static bool entry_matches_query(const char* full_path, const char* basename_disp, const char* q_raw) {
	if(q_raw == NULL) {
		return true;
	}
	const char* q = q_raw;
	while(*q == ' ') {
		++q;
	}
	if(*q == '\0') {
		return true;
	}
	const char* end = q + strlen(q);
	while(end > q && (end[-1] == ' ' || end[-1] == '\t')) {
		--end;
	}
	size_t qlen = (size_t)(end - q);
	if(qlen == 0) {
		return true;
	}

	char stack[512];
	char* needle = stack;
	if(qlen >= sizeof(stack)) {
		needle = malloc(qlen + 1);
		if(needle == NULL) {
			return str_contains(full_path, q)
				|| (basename_disp != NULL && str_contains(basename_disp, q));
		}
	}
	memcpy(needle, q, qlen);
	needle[qlen] = '\0';

	bool hit = str_contains(full_path, needle);
	if(!hit && basename_disp != NULL) {
		hit = str_contains(basename_disp, needle);
	}
	if(needle != stack) {
		free(needle);
	}
	return hit;
}

/* True if path is $HOME or a subdirectory of $HOME (not a prefix trap like /home/user2). */
static bool path_is_under_home(const char* path, const char* home) {
	if(path == NULL || home == NULL) {
		return false;
	}
	size_t hlen = strlen(home);
	if(strcmp(path, home) == 0) {
		return true;
	}
	if(strncmp(path, home, hlen) != 0) {
		return false;
	}
	return path[hlen] == '/';
}

static char* normalize_base_path(const char* path) {
	char buf[PATH_MAX];
	if(path != NULL && realpath(path, buf) != NULL) {
		return strdup(buf);
	}
	return path != NULL ? strdup(path) : NULL;
}

static char* resolve_base(const char* hint) {
	const char* home = getenv("HOME");
	if(home == NULL) {
		return NULL;
	}

	if(hint == NULL || *hint == '\0') {
		return strdup(home);
	}

	if(hint[0] == '/') {
		char* c = strdup(hint);
		struct stat st;
		if(stat(c, &st) == 0 && S_ISDIR(st.st_mode) && path_is_under_home(c, home)) {
			return c;
		}
		free(c);
		return strdup(home);
	}

	if(strncmp(hint, "~/", 2) == 0) {
		char* expanded = utils_concat(2, home, hint + 1);
		struct stat st;
		if(stat(expanded, &st) == 0 && S_ISDIR(st.st_mode)) {
			return expanded;
		}
		free(expanded);
		return strdup(home);
	}

	char try_path[PATH_MAX];
	if(snprintf(try_path, sizeof(try_path), "%s/%s", home, hint) < (int) sizeof(try_path)) {
		struct stat st;
		if(stat(try_path, &st) == 0 && S_ISDIR(st.st_mode)) {
			return strdup(try_path);
		}
	}

	DIR* d = opendir(home);
	if(d == NULL) {
		return strdup(home);
	}

	size_t* scores = NULL;
	char** names = NULL;
	size_t n = 0;

	struct dirent* e;
	while((e = readdir(d)) != NULL) {
		if(strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
			continue;
		}

		char full[PATH_MAX];
		if(snprintf(full, sizeof(full), "%s/%s", home, e->d_name) >= (int) sizeof(full)) {
			continue;
		}

		struct stat st;
		if(stat(full, &st) == -1 || !S_ISDIR(st.st_mode)) {
			continue;
		}

		size_t sc = utils_distance(e->d_name, hint);
		char** nn = realloc(names, sizeof(char*) * (n + 1));
		if(nn == NULL) {
			for(size_t i = 0; i < n; ++i) {
				free(names[i]);
			}
			free(names);
			free(scores);
			closedir(d);
			return strdup(home);
		}
		names = nn;
		size_t* ss = realloc(scores, sizeof(size_t) * (n + 1));
		if(ss == NULL) {
			for(size_t i = 0; i < n; ++i) {
				free(names[i]);
			}
			free(names);
			free(scores);
			closedir(d);
			return strdup(home);
		}
		scores = ss;
		names[n] = strdup(e->d_name);
		scores[n] = sc;
		++n;
	}
	closedir(d);

	if(n == 0) {
		return strdup(home);
	}

	size_t best = 0;
	for(size_t i = 1; i < n; ++i) {
		if(scores[i] < scores[best]) {
			best = i;
		}
	}
	for(size_t i = 0; i < n; ++i) {
		if(scores[i] == scores[best] && strcasecmp(names[i], hint) == 0) {
			best = i;
			break;
		}
	}

	char* result = utils_concat(3, home, "/", names[best]);

	for(size_t i = 0; i < n; ++i) {
		free(names[i]);
	}
	free(names);
	free(scores);

	struct stat st;
	if(stat(result, &st) == 0 && S_ISDIR(st.st_mode)) {
		return result;
	}
	free(result);
	return strdup(home);
}

bool files_search_parse(const char* filter, char** out_base_dir, char** out_query) {
	if(filter == NULL || filter[0] != '~') {
		return false;
	}

	const char* p = filter + 1;
	while(*p == ' ') {
		++p;
	}

	const char* space = strchr(p, ' ');

	/*
	 * With a space: first token is a directory hint (fuzzy or path), rest is filename query.
	 * Without a space: "~/…" or "/…" is a base path only; otherwise the whole token is a
	 * filename query under $HOME (so "~revi" finds …/Downloads/revi, not a folder named "revi").
	 */
	if(space != NULL) {
		char hint[PATH_MAX];
		size_t hl = (size_t)(space - p);
		if(hl >= sizeof(hint)) {
			return false;
		}
		memcpy(hint, p, hl);
		hint[hl] = '\0';
		const char* query_src = space + 1;
		while(*query_src == ' ') {
			++query_src;
		}
		char* base = resolve_base(hint);
		if(base == NULL) {
			return false;
		}
		*out_base_dir = base;
		*out_query = strdup(query_src);
		return true;
	}

	if(*p == '\0') {
		char* base = resolve_base("");
		if(base == NULL) {
			return false;
		}
		*out_base_dir = base;
		*out_query = strdup("");
		return true;
	}

	if(p[0] == '/' || strncmp(p, "~/", 2) == 0) {
		char* base = resolve_base(p);
		if(base == NULL) {
			return false;
		}
		*out_base_dir = base;
		*out_query = strdup("");
		return true;
	}

	const char* home = getenv("HOME");
	if(home == NULL) {
		return false;
	}
	*out_base_dir = strdup(home);
	if(*out_base_dir == NULL) {
		return false;
	}
	*out_query = strdup(p);
	if(*out_query == NULL) {
		free(*out_base_dir);
		*out_base_dir = NULL;
		return false;
	}
	return true;
}

void files_search_fill_matches(const char* base_dir, const char* query) {
	free_matches();

	if(base_dir == NULL || *base_dir == '\0') {
		free_master();
		return;
	}

	const char* home = getenv("HOME");
	char* base_norm = normalize_base_path(base_dir);
	if(base_norm == NULL) {
		free_master();
		return;
	}

	/* Search scope is only under $HOME (avoids scanning system dirs; matches "everything after home"). */
	if(home != NULL && !path_is_under_home(base_norm, home)) {
		free(base_norm);
		free_master();
		return;
	}

	const char* q = query != NULL ? query : "";
	/*
	 * Empty query at $HOME normally shows nothing (avoid dumping the whole tree).
	 * If the Ext field limits by extension (.txt, etc.), still run the match loop.
	 */
	if(home != NULL && *q == '\0') {
		char* home_norm = normalize_base_path(home);
		if(home_norm != NULL && strcmp(base_norm, home_norm) == 0) {
			free(home_norm);
			if(ext_filter == NULL || *ext_filter == '\0') {
				free(base_norm);
				return;
			}
		} else {
			free(home_norm);
		}
	}

	bool need_index = master_base == NULL || strcmp(master_base, base_norm) != 0
		|| indexed_listing != (int) listing_mode
		|| master_index_format != FILES_SEARCH_INDEX_FORMAT;
	if(need_index) {
		free_master();
		schedule_index_async(base_norm);
		return;
	}
	free(base_norm);

	for(size_t i = 0; i < master_count && match_count < max_matches; ++i) {
		if(!entry_matches_query(master_entries[i].full_path, master_entries[i].display, q)) {
			continue;
		}
		if(!path_passes_ext(master_entries[i].full_path, master_entries[i].is_dir)) {
			continue;
		}
		size_t* tmp = realloc(match_indices, sizeof(size_t) * (match_count + 1));
		if(tmp == NULL) {
			break;
		}
		match_indices = tmp;
		match_indices[match_count++] = i;
	}
}

size_t files_search_get_match_count(void) {
	return match_count;
}

const char* files_search_get_display(size_t i) {
	if(i >= match_count || match_indices == NULL) {
		return NULL;
	}
	size_t mi = match_indices[i];
	if(mi >= master_count) {
		return NULL;
	}
	return master_entries[mi].display;
}

const char* files_search_get_path(size_t i) {
	if(i >= match_count || match_indices == NULL) {
		return NULL;
	}
	size_t mi = match_indices[i];
	if(mi >= master_count) {
		return NULL;
	}
	return master_entries[mi].full_path;
}
