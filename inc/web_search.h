#ifndef WEB_SEARCH_H
#define WEB_SEARCH_H

#include <stdbool.h>
#include <stddef.h>

void web_search_configure(const char* engine_template, const char* browsers_csv);

bool web_search_is_trigger(const char* filter);

/*
 * Number of result rows for the given filter (0 if no query after '?').
 */
size_t web_search_row_count(const char* filter);

/*
 * Allocates label and action; caller must g_free() both when non-NULL.
 * action is passed to web_search_execute().
 */
bool web_search_get_row(size_t i, const char* filter, char** out_label, char** out_action);

void web_search_execute(const char* action);

#endif
