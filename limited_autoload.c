// Build with: gcc -o limited_autoload.so limited_autoload.c `pkg-config --cflags mpv` -shared -fPIC
// Warning: do not link against libmpv.so! Read:
//    https://mpv.io/manual/master/#linkage-to-libmpv
// The pkg-config call is for adding the proper client.h include path.
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/dir.h>
#include <errno.h>
// #define _GNU_SOURCE //strcasestr, dirent->d_type
#include <string.h> //strcasestr strstr strdup
#include <sys/stat.h>
#include <unistd.h> // readlink
// #include <limits.h>
#define __USE_GNU
#include <dlfcn.h> //dladdr
#include <libgen.h> //dirname
#include <ctype.h> // isspace

#include <mpv/client.h>

// #define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

// #undef __USE_MISC // DEBUG
#define DEBUG 0
#define debug_print(fmt, ...) \
            do { if (DEBUG) {\
fprintf(stderr, "%d:%s(): ", __LINE__, __func__); \
fprintf(stderr, fmt,  ##__VA_ARGS__); } } while (0)

mpv_handle *g_Handle = NULL;
uint64_t g_maxReadFiles = 500;

typedef enum MethodType {
    M_REPLACE = 0,
    M_APPEND
} methodType;

static const char* METHOD_NAMES[] = {"replace", "append"};
methodType g_lastMethod = M_REPLACE;
char reset_memory = -1;

static const char *g_loadCommand[] = {"loadfile", NULL, "append", NULL};

typedef struct DirNode dirNode;
struct DirNode {
    char *name;  // name of the root directory
    char isRootDir; // is part of initial playlist or not
    long offset; // value of dirent->d_off or telldir()
    time_t mtime;
    dirNode *next;
    dirNode *prev;
    // int64_t num_entries; // number of files added last time
};

#define NODE_INITIALIZER(NAME, ISROOT, PREV) {\
.name = NAME, \
.isRootDir = ISROOT,\
.offset = 0,\
.mtime = 0,\
.next = NULL,\
.prev = PREV\
};

unsigned char g_scriptActive = 0;
unsigned char g_recurseDirs = 1;

typedef enum FileType {
    FT_DIR = 0,
    FT_FILE
} fileType;

typedef struct PlaylistEntry {
    union {
        char *name; // only valid if type is FT_DIR
        dirNode *dnode; // only valid if type is FT_FILE
    } u;
    fileType type;
} playlistEntry;

struct PlaylistDescriptor { // mpv initial playlist entries
    // This is sort of like getting argc + argv from mpv
    uint64_t count; // number of items in the playlist after mpv's first load
    playlistEntry *entries; // pointer to the first entry in the playlist (array)
};

struct PlaylistDescriptor g_InitialPL = {
    .count = 0,
    .entries = NULL
};

enum dirFilter {
    D_REGFILES = 1,
    D_DIRS,
    D_NORMAL, // D_FILES | D_DIRS
    D_ALL
};

int check_mpv_err(int status) {
    if ( status < MPV_ERROR_SUCCESS ) {
        printf("mpv API error %d: %s\n", status,
                mpv_error_string(status));
    }
    return status; // == 0 for MPV_ERROR_SUCCESS
}

#if 0
int on_before_start_file_handler(mpv_event *event) {
    mpv_event_hook *hook = (mpv_event_hook *)event->data;
    uint64_t id = hook->id;
    mpv_hook_continue(g_Handle, id);
    return 0;
}
#endif

/* @param count number of item returned by property "playlist-count".
 */
char **get_playlist_entries(int count) {
    char **array = (char **)calloc(count, sizeof(char*));
    int length = snprintf(NULL, 0, "%d", count);
    length = 9 + length + 9 + 1;  //  playlist/ + digits +filename + \0
    char query[length];
    for (int i = 0; i < count; ++i) {
        query[0] = '\0';
        snprintf(query, sizeof(query), "playlist/%d/filename", i);
        // debug_print("Query prop: \"%s\".\n", query);

        char *szPath = mpv_get_property_string(g_Handle, query);
        if (!szPath) {
            fprintf(stderr, "[%s] Error: nothing at playlist index %d!\n",
                    mpv_client_name(g_Handle), i);
            break;
        }
        debug_print("Found playlist entry [%d] \"%s\".\n", i, szPath);

        array[i] = szPath; // only keep the pointer, but remember to mpv_free!
    }
    return array;
}

uint64_t get_playlist_length() {
    uint64_t count;
    int ret;
    ret = mpv_get_property(g_Handle, "playlist/count", MPV_FORMAT_INT64, &count);
    if (ret != MPV_ERROR_SUCCESS) {
        fprintf(stderr, "%s\n", mpv_error_string(ret));
        return 0;
    }
    return count;
}

#if 0
// /* This doesn't work very well, as the playlist gets updated and starts
// playing the next file or something... */
// int remove_file(int idx) {
//     int length = snprintf(NULL, 0, "%d", idx) + 1; // ++length; // for \0
//     char index[length];
//     snprintf(index, length, "%d", idx);
//     debug_print("Removing index %s...\n", index);
//     const char *cmd[] = {"playlist-remove",  index, NULL};
//     int err = mpv_command(g_Handle, cmd);
//     check_mpv_err(err);
// }
#endif

void append_to_playlist(const char * path) {
    const char *cmd[] = {"loadfile", path, "append", NULL};
    int err = mpv_command(g_Handle, cmd);
    check_mpv_err(err);
}

void free_nodes(dirNode* node){
    while(node->next != NULL) {
        node = node->next;
    }
    while (node->prev != NULL) {
        node = node->prev;
        free(node->next);
        node->next = NULL;
    }
}

/* In this implementation, we don't really care about the order of
 * the returned entries, ie. directories are not returned first and may be
 * loaded much later, after many regular files.
 * FIXME We could use scandir() to get directories first (or last!).
 * We could also use ftw() from ftw.h instead of recursing ourselves, but it
 * seems to be geared towards getting everything in the file tree.
 */

/* @return 0 if limit has been reached and we need to come back, 1 otherwise.
 */
int enumerate_dir( dirNode *node,
                    enum dirFilter eFilter,
                    uint64_t iAmount,
                    uint64_t *iAddedFiles )
{
    DIR *_dir = NULL;
    const char *szDirPath = node->name;
    _dir = opendir(szDirPath);
    if (_dir == NULL) return 1;

    struct stat st;

    if (stat(szDirPath, &st) < 0) {
        perror(szDirPath);
    }
    if (node->mtime == 0) { // Initialize mtime for this directory
        node->mtime = st.st_mtime;
    }
    if (node->offset > 0) {
        // debug_print("MTIME for %s: %lu\n", node->name, node->mtime);
        if (node->mtime != st.st_mtime) {
            debug_print("%s mtime changed! Starting over.\n", szDirPath);
            node->offset = 0;
        } else {
            debug_print("Opening %s at offset %ld.\n", szDirPath, node->offset);
            seekdir(_dir, node->offset);
        }
    }
    long prev_offset = node->offset;

    struct dirent *entry;
    errno = 0;
    char rewound = 0;

    while (*iAddedFiles < iAmount) {
        entry = NULL;
        entry = readdir(_dir);

        if (entry == NULL) { // end of stream
            if (errno) {
                // TODO handle errors properly
                perror(szDirPath);
                return 1;
            }
            debug_print("No more entry found in %s.\n", szDirPath);

            node->offset = 0;
            if (node->isRootDir) {
                if (g_lastMethod == M_REPLACE) {
                    // This is probably superfulous.
                    // free_nodes(node);
                    if (rewound) {
                        break;
                    }
                    rewinddir(_dir);
                    rewound = 1;
                    continue;
                }
                debug_print("No more file to append for %s.\n", szDirPath);
                break;
            }
            // NOT a root dir, we don't care about it anymore
            closedir(_dir);
            return 1;
        }

        char *name = entry->d_name;

        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;

        // Build absolute path from szDirPath
        size_t fullPathLength = snprintf(NULL, 0, "%s", szDirPath)
                                 + _D_ALLOC_NAMLEN(entry)
                                 + 1 // for '/'
                                 + 1; // '\0'
        char szFullPath[fullPathLength];
        snprintf(szFullPath, fullPathLength, "%s/%s", szDirPath, name);
        // debug_print("Found entry: %s.\n", szFullPath);

#if defined __USE_MISC && defined _DIRENT_HAVE_D_TYPE // might not be the right macros to test for
        if (entry->d_type == DT_UNKNOWN || entry->d_type == DT_LNK) {
            if (stat(szFullPath, &st) < 0) {
                perror(szFullPath);
                continue;
            }
            if (S_ISDIR(st.st_mode))
                goto isdir;
            if (!S_ISREG(st.st_mode))
                goto isnotfile;
        }
        else if (entry->d_type == DT_DIR) {
            isdir:
#else
        if (stat(szFullPath, &st) < 0) {
            perror(szFullPath);
            continue;
        }
        if (S_ISDIR(st.st_mode) != 0) {
#endif
            debug_print("DIRECTORY detected: %s.\n", entry->d_name);
            if (g_recurseDirs != 1)
                continue;
            dirNode *_dt = (dirNode *)calloc(1, sizeof(dirNode));
            if (_dt == NULL) {
                perror(szFullPath);
                return 1;
            }
            node->next = _dt;
            *(_dt) = (dirNode)NODE_INITIALIZER(strdup(szFullPath), 0, node);

#if defined __USE_MISC && defined _DIRENT_HAVE_D_OFF
            node->offset = entry->d_off;
#else
            node->offset = telldir(_dir);
#endif
            debug_print("saved current offset for %s: %lu.\n", node->name, node->offset);

            if(enumerate_dir(node->next, eFilter, iAmount, iAddedFiles) == 1){
                debug_print("enumerate()->free() %s\n", node->next->name);
                free(node->next->name);
                free(node->next);
                node->next = NULL;
            }
            continue; // go get the left over files in the current directory
        }
#if defined __USE_MISC && defined _DIRENT_HAVE_D_TYPE
        else if (entry->d_type != DT_REG) {
            isnotfile:
#else
        if (!S_ISREG(st.st_mode)) {
#endif
            debug_print("SKIPPING non-regular file: %s.\n", szFullPath);
            continue;
        }

#if defined __USE_MISC && defined _DIRENT_HAVE_D_OFF
        if ((entry->d_off >= prev_offset) && (rewound)) {
#else
        if ((telldir(_dir) >= prev_offset) && (rewound)) {
#endif
            debug_print("Duplicate previous entry detected! Breaking!\n");
            break;
        }

        g_loadCommand[1] = szFullPath;
        mpv_command(g_Handle, g_loadCommand);
        debug_print("added file %s.\n", szFullPath);
        (*iAddedFiles)++;
    }

    node->offset = telldir(_dir);

    if (stat(szDirPath, &st) < 0) {
        perror(szDirPath);
    }
    node->mtime = st.st_mtime;

    closedir(_dir);
    debug_print("Done for %s -> 0.\n", node->name);
    return 0;
}

void clear_playlist(void) {
    const char *cmd[] = {"playlist-clear", NULL};
    check_mpv_err(mpv_command(g_Handle, cmd));
}

int isValidDirPath(const char * path) {
    struct stat st;
    if (lstat(path, &st) < 0) {
        perror(path);
        return 0;
    }
    if(S_ISDIR(st.st_mode)) {
        debug_print("Valid dir: %s.\n", path);
        return 1;
    }
    return 0;
}

void update(uint64_t, enum MethodType);

void print_current_pl_entries() {
    uint64_t newcount = get_playlist_length();
    // debug_print("Playlist length = %lu.\n", newcount);

    char **entries = get_playlist_entries(newcount);
    while (newcount != 0) {
        --newcount;
        // debug_print("freeing entry %lu: %s.\n", newcount, entries[newcount]);
        mpv_free(entries[newcount]);
    };
    free(entries);
}

int on_init() {
    /* Get the initial elements loaded in the playlist into a static struct.
     * These should roughly correspond to the positional arguments passed to mpv.
     * Returns the number of directories detected, or -1 on error.
     */
    uint64_t pl_count = get_playlist_length();

    debug_print("Initial playlist length = %lu.\n", pl_count);

    if (pl_count <= 1) {
        // mpv can handle loading a single directory just fine already.
        return 0;
    }

    g_InitialPL.count = pl_count;
    char **pl_entries = get_playlist_entries(pl_count);

    int iNumState = 0;

    g_InitialPL.entries = calloc(pl_count, sizeof(playlistEntry));

    for (int i = 0; i < pl_count; ++i) {
        debug_print("Initial playlist [%d] = %s.\n", i, pl_entries[i]);

        if (isValidDirPath(pl_entries[i])) {
            iNumState++;
            dirNode *_dt = malloc(sizeof(dirNode));
            if (_dt == NULL) {
                perror("malloc dirNode");
                return -1;
            }
            // Initialize the node.
            *(_dt) = (dirNode)NODE_INITIALIZER(strdup(pl_entries[i]), 1, NULL);
            g_InitialPL.entries[i].type = FT_DIR;
            g_InitialPL.entries[i].u.dnode = _dt;
            debug_print("init() new dnode entry %s.\n", g_InitialPL.entries[i].u.dnode->name);
        } else {
            g_InitialPL.entries[i].type = FT_FILE;
            g_InitialPL.entries[i].u.name = strdup(pl_entries[i]);
            debug_print("init() new file entry %s.\n", g_InitialPL.entries[i].u.name);
        }
        mpv_free(pl_entries[i]);
    }
    free(pl_entries);
    update(g_maxReadFiles, g_lastMethod);
    return iNumState;
}

#if 0
char *fread_string(FILE *file, char const *desired_name) {
    char name[128];
    char val[128];
    while (fscanf(file, "%127[^=]=%127[^\n]%*c", name, val) == 2) {
        if (0 == strcmp(name, desired_name)) {
            return strdup(val);
        }
    }
    return NULL;
}

int fread_int(FILE *file, char const *desired_name, uint64_t *ret) {
    char *temp = fread_string(file, desired_name);
    if (temp == NULL) {
        return 0;
    }
    char *stop;
    *ret = (uint64_t)strtol(temp, &stop, 10);
    int ret_val = stop == temp;
    free(temp);
    return ret_val;
}
#endif

char *trimwhitespace(char *str) {
  char *end;
  // Trim leading space
  while(isspace((unsigned char)*str)) str++;

  if(*str == 0)  // All spaces?
    return str;

  // Trim trailing space
  end = str + strlen(str) - 1;
  while(end > str && isspace((unsigned char)*end)) end--;

  // Write new null terminator character
  end[1] = '\0';
  return str;
}

char *get_config_path(const char* szScriptName) {
    /* Get the absolute path to our shared object, in order to deduce the path
     * to the config file relative to its path.
     */
    Dl_info dl_info;
    // Get the address of a symbol in this shared object
    if (dladdr((const void*)get_config_path, &dl_info) == 0) {
        fprintf(stderr, "[%s] Failed getting SO path.\n", szScriptName);
        // Fallback to using default config path.
        const char* s = getenv("XDG_CONFIG_HOME");
        debug_print("Got XDG_CONFIG_HOME : %s\n",
               (s != NULL) ? s : "getenv returned NULL");
        if (s == NULL) return NULL;

        // FIXME make sure the program in use is indeed "mpv".
        char szConfPath[strlen(s) + strlen(szScriptName) + 22 + 1];
        snprintf(szConfPath, sizeof(szConfPath),
                 "%s/mpv/script-opts/%s.conf", s, szScriptName);
        debug_print("Path to conf file might be: %s\n", szConfPath);
        return strdup(szConfPath);
    }

    debug_print("Loaded SO path found: %s\n", dl_info.dli_fname);
    char szSoPath[strlen(dl_info.dli_fname) + 1];
    strcpy(szSoPath, dl_info.dli_fname);
    dirname(dirname(szSoPath));
    debug_print("Relative config directory to script location: %s\n", szSoPath);

    char szConfPath[strlen(szSoPath) + strlen(szScriptName) + 18 + 1];
    snprintf(szConfPath, sizeof(szConfPath),
             "%s/script-opts/%s.conf", szSoPath, szScriptName);
    debug_print("Config location might be: %s\n", szConfPath);

    // char* abs_path = realpath(szConfPath, NULL);
    // if (abs_path == NULL) {
    //     debug_print("Cannot find config file at: %s\n", szConfPath);
    //     return;
    // }
    // debug_print("Found config file at: %s\n", abs_path);
    // free(abs_path);
    // }
    return strdup(szConfPath);
}

void get_config(const char* szScriptName) {
    char *szConfPath = get_config_path(szScriptName);

    if (szConfPath == NULL) return ;

    FILE *fp = fopen((const char*)szConfPath, "r");
    free(szConfPath);
    if (!fp) {
        perror("fopen");
        return;
    }
#if 1
    char *line = NULL;
    size_t len = 0;
    ssize_t read = 0;
    while ((read = getline(&line, &len, fp)) != -1) {
        debug_print("retrieved line \"%s\", length %zu:\n", line, read);
        trimwhitespace(line);
        debug_print("%s\n", line);

        char *key = strtok(line, "=");
        char *value = strtok(NULL, "=");
        if (value == NULL) continue;

        trimwhitespace(key);
        debug_print("key=%s / value=%s.\n", key, value);
        if (strcmp(key, "limit") == 0) {
            char *stop;
            g_maxReadFiles = (uint64_t)strtoul(value, &stop, 10);

            // get the first valid number in value, otherwise default to 0
            // uint64_t iValue;
            // if (sscanf(value, "%d", &iValue) != 1){
            //     iValue = 0;
            // }
            // g_maxReadFiles = iValue;

            debug_print("config file key \"%s\" = %lu\n", key, g_maxReadFiles);
            continue;
        }
        if (strcmp(key, "recurse") == 0) {
            char *stop;
            g_recurseDirs = (unsigned char)strtoul(value, &stop, 10);
            debug_print("config file key \"%s\" = %d\n", key, g_recurseDirs);
        }
    }
    free(line);
#else
    // Obsolete, not very flexible.
    int ret = fread_int(fp, "limit", &g_maxReadFiles);
#endif
    fclose(fp);

    /* Get CLI arguments which should override the config file. */
    mpv_node props;
    int error = mpv_get_property(g_Handle, "options/script-opts",
                                 MPV_FORMAT_NODE, &props);
    if (error < 0) {
        fprintf(stderr, "[%s] Error getting options/script-opts.\n", szScriptName);
        return;
    }
    // debug_print("format=%d\n", (int)props.format);
    if (props.format == MPV_FORMAT_NODE_MAP) {
        mpv_node_list* _list = props.u.list;
        for (int i = 0; i < _list->num; ++i) {
            // debug_print("key=%s. value=%s\n", _list->keys[i], _list->values[i].u.string);
            char prefix[strlen(szScriptName) + 2]; // '-' + '\0'
            prefix[sizeof(prefix) - 1] = '\0';
            strcpy(prefix, szScriptName);
            strncat(prefix, "-", 2);
            // "scriptname-" is found in key, get value.
            if (strstr(_list->keys[i], prefix) != NULL) {
                // debug_print("Found key prefix %s in %s.\n", prefix, _list->keys[i]);
                char key[strlen(_list->keys[i]) - strlen(prefix) + 1];
                memcpy(key, &(_list->keys[i])[strlen(prefix)], sizeof(key) - 1);
                key[sizeof(key) -1] = '\0';
                // debug_print("key: %s. len: %lu.\n", key, sizeof(key));

                if (strcmp(key, "limit") == 0) {
                    // debug_print("Valid key \"%s\" value: %s\n", key, _list->values[i].u.string);
                    char *stop;
                    g_maxReadFiles = (uint64_t)strtoul(_list->values[i].u.string, &stop, 10);
                    continue;
                }

                if (strcmp(key, "recurse") == 0) {
                    debug_print("Valid key \"%s\" value: %s\n", key, _list->values[i].u.string);
                    char *stop;
                    g_recurseDirs = (unsigned char)strtoul(_list->values[i].u.string, &stop, 10);
                }
            }
        }
    }
    mpv_free_node_contents(&props);
    // TODO observe changes in property.
    // mpv_observe_property(g_Handle, 0, "options/script-opts", MPV_FORMAT_NODE)
}

void display_added_files(uint64_t num_files) {
    int length = snprintf(NULL, 0, "%lu", num_files);
    static const char *msg[] = {"Replaced playlist with %lu files.",
                                "Appended %lu files to playlist."};
    if (g_lastMethod == M_REPLACE) {
        length = length + strlen(msg[0]) - 3 + 1; // minus "%lu"
    } else {
        length = length + strlen(msg[1]) - 3 + 1;
    }
    char query[length];
    snprintf(query, sizeof(query), msg[g_lastMethod], num_files);
    const char *cmd[] = {"show-text", query, "5000" , NULL};
    check_mpv_err(mpv_command(g_Handle, cmd));
}

/* Fetch some more items from the file system.
 * @param amount The number of files to fetch.
 */
void update(uint64_t amount, enum MethodType method) {
    debug_print("===============================================\n\
update with method %s, amount %lu.\n", METHOD_NAMES[method], amount);

    // char reset = 0;
    if (method == M_REPLACE) {
        clear_playlist();
    }
    if (method != g_lastMethod) {
        // HACK need to reset internal states, unless this is the first run.
        // -> the second run is special: KEEP memory always.
        reset_memory += 1;
        if (reset_memory >= 1)
            reset_memory = 1;
    }
    g_lastMethod = method;
    debug_print("RESET %d.\n", reset_memory);

    uint64_t iTotalAdded = 0;

    for (int i = 0; i < g_InitialPL.count; ++i) {
        if (g_InitialPL.entries[i].type == FT_FILE) {
            if (method == M_REPLACE) {
                append_to_playlist(g_InitialPL.entries[i].u.name);
            }
            continue;
        }

        uint64_t iAddedFiles = 0;

        dirNode *node = g_InitialPL.entries[i].u.dnode;

        if (reset_memory >= 1) {
            reset_memory = 0;
            node->offset = 0;
            node->mtime = 0;
            free_nodes(node);
        }

        if (node->next == NULL) {
            enumerate_dir( node, D_NORMAL, amount, &iAddedFiles );
        } else {
            // We have at least one previous subdir still in memory
            while (node->next != NULL) {
                debug_print("node->next %s as subdir of %s\n", node->next->name, node->name);
                // point at the last node in the linked list
                node = node->next;
            }
            while (node->prev != NULL) {
                debug_print("Processing %s as subdir of %s\n", node->name, node->prev->name);
                int done = enumerate_dir( node, D_NORMAL, amount, &iAddedFiles );
                node = node->prev;
                debug_print("Done processing %s? -> %d.\n", node->next->name, done);
                if (done == 1) {
                    debug_print("update()->free() %s\n", node->next->name);
                    free(node->next->name);
                    free(node->next);
                    node->next = NULL;
                    continue;
                } else {
                    break;
                }
            }
        }
        debug_print("Added files from node %s: %lu.\n", node->name, iAddedFiles);
        iTotalAdded += iAddedFiles;
    }
    debug_print("Added files in total: %lu.\n", iTotalAdded);

    display_added_files(iTotalAdded);

    print_current_pl_entries();
}

void message_handler(mpv_event *event, const char* szScriptName) {
    mpv_event_client_message *msg = event->data;
    if (msg->num_args >= 2) {
        for (int i = 0; i < msg->num_args; ++i) {
            debug_print("Got message arg: %d %s\n", i, msg->args[i]);
        }
        if (strcmp(szScriptName, msg->args[0]) != 0) {
            return;
        }
        enum MethodType method;
        if (strcmp(msg->args[1], "replace") == 0) {
            method = M_REPLACE;
        }
        else if (strcmp(msg->args[1], "append") == 0) {
            method = M_APPEND;
        } else {
            fprintf(stderr, "Error parsing update command. Using last used \
method: \"%s\"\n", METHOD_NAMES[g_lastMethod]);
            method = g_lastMethod;
        }

        if (msg->num_args >= 3) {
            char *stop;
            uint64_t maxAmount = (uint64_t)strtol(msg->args[2], &stop, 10);
            update(maxAmount, method);
            return;
        }
        // 2 arguments, maxAmount not specified.
        update(g_maxReadFiles, method);
    }
}

int mpv_open_cplugin(mpv_handle *handle) {
    g_Handle = handle;
    const char* szScriptName = mpv_client_name(handle);
    debug_print("Loaded script '%s'.\n", szScriptName);
    get_config(szScriptName);
    // debug_print("Limit set to %lu\n", g_maxReadFiles);

    // mpv_hook_add(handle, 0, "on_before_start_file", 50);

    g_scriptActive = on_init();

    if (g_scriptActive <= 0) {
        return 0;
    }

    while (1) {
        mpv_event *event = mpv_wait_event(handle, -1);
        // debug_print("Got event: %d\n", event->event_id);
        // if (event->event_id == MPV_EVENT_HOOK)
        //     on_before_start_file_handler(event);
        if (event->event_id == MPV_EVENT_CLIENT_MESSAGE)
            message_handler(event, szScriptName);
        if (event->event_id == MPV_EVENT_SHUTDOWN) {
            break;
        }
    }
    return 0;
}
