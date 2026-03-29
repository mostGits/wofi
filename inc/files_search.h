#ifndef FILES_SEARCH_H
#define FILES_SEARCH_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
	FILES_SEARCH_FILES_ONLY = 0,
	FILES_SEARCH_FOLDERS_ONLY = 1,
	FILES_SEARCH_BOTH = 2
} FilesSearchListing;

void files_search_configure(const char* extra_root_dirs_csv, bool insensitive, size_t max_matches);

void files_search_clear_index(void);

void files_search_set_options(FilesSearchListing listing, const char* extension_filter);

/*
 * filter: full entry text beginning with '~'.
 * Resolves base directory (fuzzy match on $HOME subdirs or explicit path) and
 * optional file query after the first space.
 * Returns false if not a file-search filter.
 */
bool files_search_parse(const char* filter, char** out_base_dir, char** out_query);

void files_search_fill_matches(const char* base_dir, const char* query);

/*
 * Called on the GTK main thread when a background directory index finishes.
 * Safe to call gtk_entry_get_text / update_file_search_ui from here.
 */
void files_search_set_index_ready_callback(void (*cb)(void* user_data), void* user_data);

bool files_search_index_busy(void);

size_t files_search_get_match_count(void);
const char* files_search_get_display(size_t i);
const char* files_search_get_path(size_t i);

#endif
