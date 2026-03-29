#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>
#include <errno.h>

#include <config.h>
#include <utils.h>
#include <wofi_api.h>

static const char* arg_names[] = {"root"};

static struct mode* mode;
static char* root_dir;

struct node {
        struct widget* widget;
        struct wl_list link;
};

static struct wl_list widgets;

static void add_file_widget(const char* path) {
        char* text = strdup(path);
        char* action = strdup(path);

        struct node* node = malloc(sizeof(struct node));
        node->widget = wofi_create_widget(mode, &text, text, &action, 1);
        wl_list_insert(&widgets, &node->link);

        free(text);
        free(action);
}

static void scan_dir_recursive(const char* dir_path) {
        DIR* dir = opendir(dir_path);
        if(dir == NULL) {
                return;
        }

        struct dirent* entry;
        while((entry = readdir(dir)) != NULL) {
                if(strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                        continue;
                }

                char full_path[PATH_MAX];
                snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);

                struct stat st;
                if(lstat(full_path, &st) == -1) {
                        continue;
                }

                if(S_ISDIR(st.st_mode)) {
                        scan_dir_recursive(full_path);
                } else if(S_ISREG(st.st_mode)) {
                        add_file_widget(full_path);
                }
        }

        closedir(dir);
}

void wofi_files_init(struct mode* this, struct map* config) {
        mode = this;
        wl_list_init(&widgets);

        const char* cfg_root = config_get(config, "root", "~/Projects");
        if(cfg_root[0] == '~') {
                root_dir = utils_concat(2, getenv("HOME"), cfg_root + 1);
        } else {
                root_dir = strdup(cfg_root);
        }

        scan_dir_recursive(root_dir);
}

struct widget* wofi_files_get_widget(void) {
        struct node* node, *tmp;
        wl_list_for_each_reverse_safe(node, tmp, &widgets, link) {
                struct widget* widget = node->widget;
                wl_list_remove(&node->link);
                free(node);
                return widget;
        }
        return NULL;
}

void wofi_files_exec(const char* cmd) {
        if(cmd == NULL || *cmd == '\0') {
                wofi_exit(1);
        }

        if(fork() == 0) {
                execlp("xdg-open", "xdg-open", cmd, NULL);
                fprintf(stderr, "xdg-open failed for %s: %s\n", cmd, strerror(errno));
                exit(EXIT_FAILURE);
        }

        wofi_exit(0);
}

const char** wofi_files_get_arg_names(void) {
        return arg_names;
}

size_t wofi_files_get_arg_count(void) {
        return sizeof(arg_names) / sizeof(char*);
}