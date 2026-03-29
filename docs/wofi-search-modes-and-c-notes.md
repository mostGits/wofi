# Wofi `~` / `?` search modes and C notes

This document explains how the file-search (`~`) and web-search (`?`) features fit into wofi, why a segmentation fault could occur when typing those characters, and the C concepts involved.

---

## 1. What was crashing (and the fix)

### Symptom

Running `wofi --show drun` and typing **`~`** or **`?`** caused a **segmentation fault** (SIGSEGV).

### Cause (high level)

Wofi uses GTK’s **`GtkFlowBox`**. When the selection changes, GTK emits **`selected-children-changed`**, which calls `select_item()` in `wofi.c`.

The old code did roughly:

```c
GList* selected_children = gtk_flow_box_get_selected_children(flow_box);
GtkWidget* box = gtk_bin_get_child(GTK_BIN(selected_children->data));  /* unsafe */
```

**Problem:** `gtk_flow_box_get_selected_children()` can return **`NULL`** when nothing is selected, or a list whose first element has no valid widget yet. Using `selected_children->data` without checking is a **NULL pointer dereference** → undefined behavior → often an immediate crash (segmentation fault).

Typing `~` or `?` **replaces all children** of the flow box (file rows or web rows). During that update, there can be a **brief moment with zero children or no selection**, so the signal handler must tolerate “no selection”.

### Fix

Before using `selected_children->data`, guard:

```c
if (selected_children == NULL || selected_children->data == NULL) {
    g_list_free(selected_children);
    return;
}
```

The same pattern was applied anywhere else that assumed `children->data` existed after `gtk_flow_box_get_selected_children()` (e.g. `do_expand`, `do_copy`, and one branch in `key_press`).

---

## 2. How `~` and `?` work in this fork (architecture)

### Event loop and the search callback

GTK runs a **main loop**. A **timeout** (`gdk_threads_add_timeout`) calls `do_search()` periodically. Each time, it reads the entry text and:

- If it starts with **`~`** → ensure mode widgets are loaded, **save** the normal list off-screen, then show **file search** results.
- If it starts with **`?`** → same idea, but **web search** rows (default browser + optional extra browsers).
- Otherwise → **restore** the saved list if needed, run the calculator row logic, and apply the normal filter.

### Modular C files

| Piece | Role |
|--------|------|
| `src/files_search.c` | Parse `~…`, resolve a **base directory** (including fuzzy match under `$HOME`), walk files, filter by substring. |
| `src/web_search.c` | Parse `?…`, build a search URL, build one **action string per row** (default browser vs named browser). |
| `wofi.c` | GTK wiring: save/restore flow box children, create rows, `execute_action` for internal “modes” `file_search` / `web_search`. |

This keeps **UI** (GTK) separate from **pure logic** (paths, URLs, `fork`/`execlp` for browsers) as much as practical.

### Save / restore of the flow box

When you leave normal mode for `~`/`?`, the existing rows (apps, etc.) are **not destroyed**: they are **removed from the container** and kept in a **`GList`** of referenced widgets (`g_object_ref` / `gtk_container_remove`). When you delete the prefix, those widgets are **put back** into `inner_box`. That avoids losing the mode’s widgets (wofi only inserts them once from the mode thread).

---

## 3. C basics used here (short)

### Pointers and `NULL`

A **pointer** holds a memory address. **`NULL`** is a special value meaning “points to nothing.” Dereferencing `NULL` (reading `*p` or `p->field` when `p == NULL`) is invalid and typically crashes.

**Defensive style:** test pointers before use:

```c
if (ptr == NULL) { return; }
```

### Strings

C strings are **`char *`** pointing to bytes ending with **`'\0'`**. Functions like `strcmp`, `strdup`, `strstr` are from the standard library (`string.h`). GLib adds helpers like `g_strdup` and `g_uri_escape_string`.

### Stack vs heap

- **Automatic variables** (inside a function, no `malloc`): live on the **stack**; gone when the function returns.
- **`malloc` / `strdup` / `g_strdup`**: allocate on the **heap**; must be **`free` / `g_free`** exactly once when no longer needed, or you leak memory.

GTK often **takes ownership** of strings passed into properties depending on API; here, `wofi_property_box_add_property` stores the pointer you pass—so that string must stay valid for the widget’s lifetime (or be duplicated by the caller).

### `static` file scope

Functions and variables declared **`static`** at file scope are **only visible inside that `.c` file**. That avoids name clashes with other files and is a simple form of **encapsulation**.

### Forward declarations

C compiles top-to-bottom. If function `A` calls `B` before `B` is defined, you need a **forward declaration** (prototype) at the top:

```c
static void B(void);
static void A(void) { B(); }
static void B(void) { /* ... */ }
```

---

## 4. More advanced topics touched by this code

### GTK reference counting (`g_object_ref` / `g_object_unref`)

GTK widgets are `GObject`s. Removing a widget from a container might **destroy** it unless something else holds a reference. **`g_object_ref`** before **`gtk_container_remove`** keeps the widget alive while it sits in your `GList`; **`g_object_unref`** after **`gtk_container_add`** transfers ownership back to the container.

### GLib lists (`GList`)

`gtk_flow_box_get_selected_children` returns a **newly allocated linked list**; you must **`g_list_free`** it (it does not free the widgets). If the list is empty, the pointer may be **`NULL`**—always check before `->data`.

### Signals and re-entrancy

GTK **signals** (like selection changed) can run **while** your code is updating children. That’s why updating the flow box must leave the handlers safe when the box is **empty** or **temporarily has no selection**.

### Process creation (`fork`, `execlp`, `waitpid`)

Opening a URL in the default browser often uses **`xdg-open`**. The code typically **`fork()`**s a child process; the child calls **`execlp("xdg-open", "xdg-open", url, NULL)`** to replace itself with the browser opener. The parent may **`waitpid`** in a non-blocking way to reap zombies. Wrong use of `fork`/`exec` is a common source of bugs; here it’s kept in small, dedicated functions.

### Fuzzy directory matching (`utils_distance`)

`utils_distance` implements a form of **edit distance** between strings, used to pick the “closest” directory name under `$HOME` when you type a short hint (e.g. `Pro` → `Projects`). This is **heuristic**, not magic: wrong matches are possible if names are similar.

---

## 5. How to verify after a change

1. Build: `meson compile -C build`
2. Run on a **Wayland** session: `./build/wofi --show drun`
3. Type **`~`** then a space and a folder hint and a file substring; clear back to normal list.
4. Type **`?`** and a query; pick a row; confirm browser opens.

If anything crashes again, run under **gdb**:

```bash
gdb --args ./build/wofi --show drun
# at crash: bt
```

The backtrace shows the exact function and line.

---

## 6. Stale `previous_selection` and GTK-CRITICAL

The global `previous_selection` points at the **inner property box** of the currently highlighted row. When **`~`** or **`?`** rebuilds the list, old rows are **destroyed**. If `previous_selection` is not cleared first, the next **`select_item`** or **`gtk_widget_set_name(previous_selection, …)`** can run on a **freed widget** → `GTK_IS_WIDGET` assertion failure, or **`gtk_container_foreach`** errors if something else got corrupted.

**Fix:** set `previous_selection = NULL` **before** destroying any `inner_box` children in `update_file_search_ui`, `update_web_search_ui`, and at the **start** of `restore_inner_box_state` (before `gtk_container_foreach`).

---

## 7. File search bar (type + extension)

When the filter starts with **`~`**, a small bar appears between the search field and the results:

- **Type:** `Files` (regular files only), `Folders` (directories only), or `Both`.
- **Ext:** Comma-separated suffixes (e.g. `.c, .h, txt`). Only applies to **files**; for **Folders** the ext field is disabled.

The bar uses GTK names you can style in CSS: `#file-search-bar`, `#file-search-type`, `#file-search-ext`, `#file-search-label`.

---

## 8. File search performance (index cache, UI debounce, row cap)

1. **Disk:** The tree is **walked once per base directory** and cached (`master_entries`). Only the **query** changes on each keypress; filtering is in-memory.

2. **GTK:** Rebuilding **every** `GtkFlowBox` row on each keypress is still expensive (destroy + `create_label` × N). So the UI refresh is **debounced** (default **120 ms**, config `files_search_debounce_ms`) **only when the filter grows** (typing forward). **Backspace/delete** (shorter filter) runs an **immediate** refresh so deleting characters stays responsive. A lone **`~`** also updates immediately. Set **`files_search_debounce_ms=0`** to disable debounce entirely.

3. **Matches:** At most **`files_search_max_matches`** rows are shown (default **200**), so the list stays bounded even if thousands of paths match.

Search roots stay under **`$HOME`** (see above).

---

## 9. Summary

- Crashes were **NULL dereferences** in **`select_item`** (empty selection) and **use-after-free** on **`previous_selection`** when file/web rows were rebuilt.
- The feature is split into **`files_search`**, **`web_search`**, and **`wofi.c`** glue, with save/restore of the main list so normal mode entries are not lost.
- Safe C with GTK means **checking pointers** from APIs like `gtk_flow_box_get_selected_children()` and **invalidating stale widget pointers** before destroying their ancestors.
