#include <web_search.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <glib.h>

static char* engine_template = NULL;
static char** browsers = NULL;
static size_t browser_count = 0;

static void free_browsers(void) {
	for(size_t i = 0; i < browser_count; ++i) {
		free(browsers[i]);
	}
	free(browsers);
	browsers = NULL;
	browser_count = 0;
}

void web_search_configure(const char* tmpl, const char* browsers_csv) {
	free(engine_template);
	engine_template = NULL;
	free_browsers();

	if(tmpl != NULL && *tmpl != '\0') {
		engine_template = strdup(tmpl);
	} else {
		/* ia=web: prefer web tab over news/maps where applicable */
		engine_template = strdup("https://duckduckgo.com/?q=%s&ia=web");
	}

	if(browsers_csv == NULL || *browsers_csv == '\0') {
		return;
	}

	char* dup = strdup(browsers_csv);
	char* save = NULL;
	for(char* tok = strtok_r(dup, ",", &save); tok != NULL; tok = strtok_r(NULL, ",", &save)) {
		while(*tok == ' ') {
			++tok;
		}
		char* end = tok + strlen(tok);
		while(end > tok && (end[-1] == ' ' || end[-1] == '\t')) {
			*--end = '\0';
		}
		if(*tok == '\0') {
			continue;
		}
		char** nb = realloc(browsers, sizeof(char*) * (browser_count + 1));
		if(nb == NULL) {
			break;
		}
		browsers = nb;
		browsers[browser_count] = strdup(tok);
		++browser_count;
	}
	free(dup);
}

static const char* query_after_prefix(const char* filter) {
	if(filter == NULL || filter[0] != '?') {
		return NULL;
	}
	const char* q = filter + 1;
	while(*q == ' ') {
		++q;
	}
	return q;
}

bool web_search_is_trigger(const char* filter) {
	return filter != NULL && filter[0] == '?';
}

size_t web_search_row_count(const char* filter) {
	const char* q = query_after_prefix(filter);
	if(q == NULL || *q == '\0') {
		return 0;
	}
	return 1 + browser_count;
}

bool web_search_get_row(size_t i, const char* filter, char** out_label, char** out_action) {
	const char* q = query_after_prefix(filter);
	if(q == NULL || *q == '\0') {
		return false;
	}

	char* escaped = g_uri_escape_string(q, NULL, TRUE);
	if(escaped == NULL) {
		return false;
	}

	char* url = g_strdup_printf(engine_template, escaped);
	g_free(escaped);

	if(url == NULL) {
		return false;
	}

	if(i == 0) {
		*out_label = g_strdup_printf("Default browser · %s", q);
		*out_action = g_strdup_printf("default\t%s", url);
		g_free(url);
		return true;
	}

	if(i - 1 >= browser_count) {
		g_free(url);
		return false;
	}

	const char* br = browsers[i - 1];
	*out_label = g_strdup_printf("%s · %s", br, q);
	*out_action = g_strdup_printf("%s\t%s", br, url);
	g_free(url);
	return true;
}

void web_search_execute(const char* action) {
	if(action == NULL) {
		return;
	}

	char* dup = strdup(action);
	if(dup == NULL) {
		return;
	}

	char* tab = strchr(dup, '\t');
	if(tab == NULL) {
		free(dup);
		return;
	}

	*tab++ = '\0';
	const char* which = dup;
	const char* url = tab;

	pid_t pid = fork();
	if(pid == -1) {
		perror("fork");
		free(dup);
		return;
	}

	if(pid == 0) {
		if(strcmp(which, "default") == 0) {
			execlp("xdg-open", "xdg-open", url, NULL);
			fprintf(stderr, "xdg-open: %s\n", strerror(errno));
		} else {
			execlp(which, which, url, NULL);
			fprintf(stderr, "%s: %s\n", which, strerror(errno));
		}
		exit(EXIT_FAILURE);
	}

	while(waitpid(-1, NULL, WNOHANG) > 0) {
	}

	free(dup);
}
