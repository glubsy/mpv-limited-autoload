// Build with: gcc -o limited_autoload.so limited_autoload.c `pkg-config --cflags mpv` -shared -fPIC
// Warning: do not link against libmpv.so! Read:
//    https://mpv.io/manual/master/#linkage-to-libmpv
// The pkg-config call is for adding the proper client.h include path.

/* Objective: when directories are passed as positional arguments to MPV,
 * load files from these directories ourself instead of instead of letting the
 * mpv internal autoload script load the files the wrong way. ie. files
 * from the first read directory are loaded at once, and the directories left
 * in the playlist are lost in the amount of files, which reduces greatly
 * the chance of encountering them and loading their files too.
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/dir.h>
#include <errno.h>
// #define _GNU_SOURCE //strcasestr
#include <string.h> //strcasestr strstr strdup
#include <sys/stat.h>
#include <unistd.h>
// #include <limits.h>
#define __USE_GNU
#include <dlfcn.h> //dladdr
#include <libgen.h> //dirname
#include <ctype.h> // isspace

#include <mpv/client.h>

// #define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

mpv_handle *HANDLE = NULL;
uint64_t MAX_READ_FILES = 30;

typedef enum MethodType {
    M_REPLACE = 0,
    M_APPEND
} methodType;

const char* methodNames[] = {"replace", "append"};

static const char *LOAD_CMD[] = {"loadfile", NULL, "append", NULL};
methodType METHOD = M_REPLACE;

typedef struct DirNode {
    
    long offset;
    long mtime;
    struct DirNode *next;
} dirNode;


typedef struct DirState {
    char *root_dir;  // name of the root directory
    DIR *ptr; // opened fd to the directory
    int64_t num_entries; // number of files added last time
    char *last_dir; // name of the last sub-directory read
    int64_t offset; // value of dirent->d_off or returned by telldir() from the last_dir
} dirState;

int iScriptActive = 0;

typedef enum FileType {
    FT_DIR = 0,
    FT_FILE
} fileType;

typedef struct PlaylistEntry {
    union {
        char *name; // only valid if type is FT_DIR
        dirState *state; // only valid if type is FT_FILE
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

int check_mpv_err(int status) {
    if ( status < MPV_ERROR_SUCCESS ) {
        printf("mpv API error %d: %s\n", status,
                mpv_error_string(status));
    }
    return status; // == 0 for MPV_ERROR_SUCCESS
}

int on_before_start_file_handler(mpv_event *event) {
    mpv_event_hook *hook = (mpv_event_hook *)event->data;
    uint64_t id = hook->id;
    // on_init();
    mpv_hook_continue(HANDLE, id);
    return 0;
}

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
        printf("Query prop: \"%s\".\n", query);

        char *szPath = mpv_get_property_string(HANDLE, query);
        if (!szPath) {
            fprintf(stderr, "Error: nothing at playlist index %d!\n", i);
            break;
        }
        printf("Found playlist entry: %s.\n", szPath);

        array[i] = szPath; // only keep pointer, remember to mpv_free()!
    }
    return array;
}

uint64_t get_playlist_length() {
    uint64_t count;
    int ret;
    ret = mpv_get_property(HANDLE, "playlist/count", MPV_FORMAT_INT64, &count);
    if (ret != MPV_ERROR_SUCCESS) {
        fprintf(stderr, "%s\n", mpv_error_string(ret));
        return 0;
    }
    return count;
}

// This doesn't work very well, as the playlist gets updated and starts playing
// the next file or something.
// int remove_file(int idx) {
//     int length = snprintf(NULL, 0, "%d", idx) + 1; // ++length; // for \0
//     char index[length];
//     snprintf(index, length, "%d", idx);
//     printf("Removing index %s...\n", index);
//     const char *cmd[] = {"playlist-remove",  index, NULL};
//     int err = mpv_command(HANDLE, cmd);
//     check_mpv_err(err);
// }

enum dirFilter {
    D_ALL = 0,
    D_FILES,
    D_DIRS, // recurse
    D_NORMAL
};


/* NOTES: in this implementation, we don't really care about the order of
 * the returned entries, ie. directories are not returned first and may be
 * loaded much later, after many regular files.
 * FIXME We could use scandir() to get directories first (or last!).
 * We could also use ftw() from ftw.h instead of recursing ourselves, but it
 * seems to be geared towards getting everything in the file tree.
 */
void append_to_playlist(const char * path) {
    const char *cmd[] = {"loadfile", path, "append", NULL};
    int err = mpv_command(HANDLE, cmd);
    check_mpv_err(err);
}

void enumerate_dir(const char* szDirPath,
                     enum dirFilter eFilter,
                     uint64_t iAmount,
                     uint64_t *iAddedFiles,
                     uint64_t* iOffset, 
                     int isRootDir
                    )
{
    DIR *_dir = NULL;
    _dir = opendir(szDirPath);
    if (_dir == NULL) return;
    printf("opendir: %s\n", szDirPath);

    struct dirent *entry;
#ifndef __USE_MISC
    struct stat st;
#endif
    while (*iAddedFiles < iAmount) {
        entry = readdir(_dir);
        if (!entry) /* FIXME && szDirPath is root_dir*/ { // end of stream
            // TODO handle errors too here.
            // rewinddir(dir); // if method "replace"
            // continue;
            break;
        }
        char *name = entry->d_name;

        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;

        // Build absolute path from szDirPath
        unsigned int iPathLength = snprintf(NULL, 0, "%s", szDirPath) + 1;
        size_t iFullPathLength = iPathLength + _D_ALLOC_NAMLEN(entry) + 1; // for '/'
        char szFullPath[iFullPathLength];
        snprintf(szFullPath, iFullPathLength, "%s/%s", szDirPath, name);

#ifdef __USE_MISC
        if ((entry->d_type == DT_DIR) /*&& (filter & D_DIRS)*/) {
            printf("Dir detected %s.\n", szFullPath);
            enumerate_dir(szFullPath, eFilter, iAmount, iAddedFiles, iOffset, 0);
            continue;
        } else {
#else
        if (lstat(szFullPath, &st) < 0) {
            perror(szFullPath);
            continue;
        }
        if(S_ISDIR(st.st_mode)) {
            printf("lstat(%s) -> ISDIR.\n", szFullPath);
            enumerate_dir(szFullPath, eFilter, iAmount, iAddedFiles, iOffset, 0);
            continue;
        } else {
#endif // __USE_MISC
            LOAD_CMD[1] = szFullPath;
            mpv_command(HANDLE, LOAD_CMD);
            (*iAddedFiles)++;
        }
    }
    closedir(_dir);
}

void clear_playlist(void){
    const char *cmd[] = {"playlist-clear", NULL};
    check_mpv_err(mpv_command(HANDLE, cmd));
}

int isValidDirPath(const char * path) {
    struct stat st;
    if (lstat(path, &st) < 0) {
        perror(path);
        return 0;
    }
    if(S_ISDIR(st.st_mode)) {
        printf("%s is a valid directory.\n", path);
        return 1;
    }
    return 0;
}

void update(uint64_t amount);

void print_current_pl_entries() {
    uint64_t newcount = get_playlist_length();
    printf("Playlist length = %lu.\n", newcount);

    char **entries = get_playlist_entries(newcount);
    while (newcount != 0) {
        --newcount;
        // printf("DEBUG freeing entry %lu: %s.\n", newcount, entries[newcount]);
        mpv_free(entries[newcount]);
    };
    free(entries);
}

int on_init() {
    /* Get the initial elements loaded in the playlist in static struct.
     * These should roughly correspond to the positional arguments passed to mpv.
     * Returns the number of directories detected, or -1 on error.
     */
    uint64_t pl_count = get_playlist_length();

    printf("Playlist initial length = %lu.\n", pl_count);

    if (pl_count <= 1) {
        // mpv can handle loading a single directory just fine already.
        return 0;
    }

    g_InitialPL.count = pl_count;
    char **pl_entries = get_playlist_entries(pl_count);

    int iNumState = 0;

    g_InitialPL.entries = calloc(pl_count, sizeof(playlistEntry));

    for (int i = 0; i < pl_count; ++i) {
        printf("Checking playlist[%d]: %s.\n", i, pl_entries[i]);

        if (isValidDirPath(pl_entries[i])) {
            iNumState++;
            dirState *_dt = malloc(sizeof(dirState));
            if (_dt == NULL) {
                perror("malloc dirState");
                return -1;
            }
            // g_InitialPL.entries[i] = PlaylistEntry;
            g_InitialPL.entries[i].type = FT_DIR;
            g_InitialPL.entries[i].u.state = _dt;
            g_InitialPL.entries[i].u.state->root_dir = strdup(pl_entries[i]);
            printf("new dir state for %s.\n", g_InitialPL.entries[i].u.state->root_dir);
        } else {
            g_InitialPL.entries[i].type = FT_FILE;
            g_InitialPL.entries[i].u.name = strdup(pl_entries[i]);
            printf("new entry for file %s.\n", g_InitialPL.entries[i].u.name);
        }
        mpv_free(pl_entries[i]);
    }
    free(pl_entries);
    update(MAX_READ_FILES);
    return iNumState;
}

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

char *trimwhitespace(char *str)
{
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

char *get_config_path(const char* szScriptName){
    /* Get the absolute path to our shared object, in order to deduce the path
     * to the config file relative to its path.
     */
    Dl_info dl_info;
    if (dladdr(get_config_path, &dl_info) == 0) {
        fprintf(stderr, "[%s] Failed getting SO path.\n", szScriptName);

        const char* s = getenv("XDG_CONFIG_HOME");
        printf("Got XDG_CONFIG_HOME : %s\n",
               (s != NULL) ? s : "getenv returned NULL");
        if (s == NULL) return NULL;

        // FIXME make sure the program in use is indeed "mpv"
        char szConfPath[strlen(s) + strlen(szScriptName) + 22 + 1];
        snprintf(szConfPath, sizeof(szConfPath),
                 "%s/mpv/script-opts/%s.conf", s, szScriptName);
        printf("Path to conf file might be: %s\n", szConfPath);
        return strdup(szConfPath);
    }

    printf("Loaded SO path found: %s\n", dl_info.dli_fname);
    char szSoPath[strlen(dl_info.dli_fname) + 1];
    strncpy(szSoPath, dl_info.dli_fname, strlen(dl_info.dli_fname));
    dirname(dirname(szSoPath));
    printf("Relative config directory to script location: %s\n", szSoPath);

    char szConfPath[strlen(szSoPath) + strlen(szScriptName) + 18 + 1];
    snprintf(szConfPath, sizeof(szConfPath),
             "%s/script-opts/%s.conf", szSoPath, szScriptName);
    printf("Config location might be: %s\n", szConfPath);

    // char* abs_path = realpath(szConfPath, NULL);
    // if (abs_path == NULL) {
    //     printf("Cannot find config file at: %s\n", szConfPath);
    //     return;
    // }
    // printf("Found config file at: %s\n", abs_path);
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
        printf("Retrieved line of length %zu:\n", read);
        trimwhitespace(line);
        printf("%s\n", line);

        char *key = strtok(line, "=");
        char *value = strtok(NULL, "=");
        if (value == NULL) continue;

        trimwhitespace(key);
        printf("key=%s / value=%s.\n", key, value);
        if (strcmp(key, "limit") == 0) {
            char *stop;
            MAX_READ_FILES = (uint64_t)strtol(value, &stop, 10);

            // get the first valid number in value, otherwise default to 0
            // uint64_t iValue;
            // if (sscanf(value, "%d", &iValue) != 1){
            //     iValue = 0;
            // }
            // MAX_READ_FILES = iValue;

            printf("Got limit key %lu\n", MAX_READ_FILES);
        }
    }
    free(line);
#else
    // Obsolete: not flexible
    int ret = fread_int(fp, "limit", &MAX_READ_FILES);
#endif
    fclose(fp);

    /* Get CLI arguments which should override the config file. */
    mpv_node props;
    int error = mpv_get_property(HANDLE, "options/script-opts",
                                 MPV_FORMAT_NODE, &props);
    if (error < 0) {
        printf("Error getting prop.\n");
        return;
    }
    printf("format=%d\n", (int)props.format);
    if (props.format == MPV_FORMAT_NODE_MAP) {
        mpv_node_list* _list = props.u.list;
        for (int i = 0; i < _list->num; ++i) {
            printf("key=%s. value=%s\n", _list->keys[i], _list->values[i].u.string);
            char prefix[strlen(szScriptName + 2)]; // '-' + '\0'
            strncpy(prefix, szScriptName, strlen(szScriptName));
            strncat(prefix, "-", 2);
            // "scriptname-" in found in key, get value
            if (strstr(_list->keys[i], prefix) != NULL) {
                printf("Found key prefix %s in %s.\n", prefix, _list->keys[i]);
                char key[strlen(_list->keys[i]) - strlen(prefix) + 1];
                memcpy(key, &(_list->keys[i])[strlen(prefix)], sizeof(key) - 1);
                key[sizeof(key) -1] = '\0';
                printf("key: %s. len: %lu.\n", key, sizeof(key));

                if (strcmp(key, "limit") == 0) {
                    printf("Valid key \"%s\" value: %s\n", key, _list->values[i].u.string);
                    char *stop;
                    MAX_READ_FILES = (uint64_t)strtol(_list->values[i].u.string, &stop, 10);
                }
            }
        }
    }
    mpv_free_node_contents(&props);

    // mpv_observe_property(HANDLE, 0, "options/script-opts", MPV_FORMAT_NODE)
}

void cleanup(void){
    //TODO free each playlistEntry.u.name and playlistEntry.u.state.root_dir
}

/* Fetch some more items from the file system.
 *
 * @param amount The number of files to fetch.
 */
void update(uint64_t amount) {
    printf("Update with method %s, amount %lu.\n", methodNames[METHOD], amount);

    if (METHOD == M_REPLACE) {
        clear_playlist();
    }

    uint64_t iTotalAdded = 0;

    for (int i = 0; i < g_InitialPL.count; ++i) {
        if (g_InitialPL.entries[i].type == FT_FILE) {
            append_to_playlist(g_InitialPL.entries[i].u.name);
            continue;
        }
        uint64_t offset = 0;
        uint64_t iAddedFiles = 0;
        enumerate_dir(g_InitialPL.entries[i].u.state->root_dir,
                      D_NORMAL,
                      amount,
                      &iAddedFiles,
                      &offset,
                      1);
        printf("Number of added files for %s: %lu\n",
                g_InitialPL.entries[i].u.state->root_dir, iAddedFiles);
        iTotalAdded += iAddedFiles;
    }
    printf("Added %lu files in total.\n", iTotalAdded);

    int length = snprintf(NULL, 0, "%lu", iTotalAdded);
    length = length + 6 + 19 + 1;  // "Added " + " files to playlist."
    char query[length];
    snprintf(query, sizeof(query), "Added %lu files to playlist.", iTotalAdded);
    const char *cmd[] = {"show-text", query, NULL};
    check_mpv_err(mpv_command(HANDLE, cmd));
    print_current_pl_entries();
}


void message_handler(mpv_event *event, const char* szScriptName){
    mpv_event_client_message *msg = event->data;
    if (msg->num_args >= 2) {
        /* DEBUG */
        for (int i = 0; i < msg->num_args; ++i) {
            printf("Got message arg: %d %s\n", i, msg->args[i]);
        }
        /* DEBUG */
        if (strcmp(szScriptName, msg->args[0]) != 0) {
            return;
        }
        if (strcmp(msg->args[1], "replace") != 0) {
            METHOD = M_REPLACE;
        }
        else if (strcmp(msg->args[1], "append") != 0) {
            printf("Found append!\n");
            METHOD = M_APPEND;
        } else {
            fprintf(stderr, "Error parsing update command. Using last used \
method: \"%s\"\n", methodNames[METHOD]);
        }
        if (msg->num_args >= 3) {
            char *stop;
            uint64_t amount = (uint64_t)strtol(msg->args[2], &stop, 10);
            update(amount);
            return;
        }
        // 2 arguments, amount not specified.
        update(MAX_READ_FILES);
    }
}

int mpv_open_cplugin(mpv_handle *handle){
    HANDLE = handle;
    const char* szScriptName = mpv_client_name(handle);
    printf("Loaded '%s'!\n", szScriptName);
    get_config(szScriptName);
    printf("Limit set to %lu\n", MAX_READ_FILES);

    // mpv_hook_add(handle, 0, "on_before_start_file", 50);

    iScriptActive = on_init();

    if (iScriptActive <= 0) {
        return 0;
    }

    while (1) {
        mpv_event *event = mpv_wait_event(handle, -1);
        // printf("Got event: %d\n", event->event_id);
        // if (event->event_id == MPV_EVENT_HOOK)
        //     on_before_start_file_handler(event);
        if (event->event_id == MPV_EVENT_CLIENT_MESSAGE)
            message_handler(event, szScriptName);
        if (event->event_id == MPV_EVENT_SHUTDOWN) {
            cleanup();
            break;
        }
    }
    return 0;
}
