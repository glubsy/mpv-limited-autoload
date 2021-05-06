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
#include <string.h> //strcasestr strstr
#include <sys/stat.h>
#include <unistd.h>
// #include <limits.h>
#define __USE_GNU
#include <dlfcn.h> //dladdr
#include <libgen.h> //dirname
#include <ctype.h> // isspace

#include <mpv/client.h>

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

mpv_handle *HANDLE = NULL;
uint64_t MAX_READ_FILES = 30;

const char *LOAD_CMD[] = {"loadfile", NULL, "append", NULL};

typedef struct DirState {
    char *root_dir;  //name
    DIR *ptr;
    int64_t num_entries; // number of files added last time
    char *last_dir; // name of the last sub-directory read
    int64_t offset; // value of dirent->d_off or returned by telldir() from the last_dir
} dirState;

dirState **g_aDirState = NULL; // dynamic array of dirStates
int64_t g_iNumState = 0; // number of directories we keep track of

int iScriptActive = 0;

struct PlaylistDescriptor{
    // This is sort of like getting argc + argv from mpv
    int64_t count; // number of items in the playlist after mpv's first load
    char **entries; // entries in the playlist after mpv's first load
} g_InitialPL;


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

char **get_playlist_entries(int count) {
    char **array = (char **) calloc(count, sizeof(char*));
    int length = snprintf(NULL, 0, "%d", count) + 1;
    length = length + 9 + 9 + 1;  // playlist/ + digit + filename + \0
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

int64_t get_playlist_length() {
    int64_t count;
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

int append_to_playlist(const char * path) {
    const char *cmd[] = {"loadfile", path, "append", NULL};
    int err = mpv_command(HANDLE, cmd);
    check_mpv_err(err);
}

void enumerate_dir(const char* szDirPath,
                     enum dirFilter eFilter,
                     int64_t *iAddedFiles,
                     int64_t* iOffset )
{
    struct dirent *entry;
#ifndef ___USE_MISC
    struct stat st;
#endif
    DIR *_dir = NULL;
    _dir = opendir(szDirPath);
    printf("opendir: %s\n", szDirPath);

    /* NOTES: in this implementation, we don't really care about the order of
     * the returned entries, ie. directories are not returned first and may be
     * loaded much later, after many regular files.
     * FIXME We could use scandir() to get directories first (or last!).
     * We could also use ftw() from ftw.h instead of recursing ourselves, but it
     * seems to be geared towards getting everything in the file tree.
     */

    while (*iAddedFiles < MAX_READ_FILES) {
        entry = readdir(_dir);
        if (!entry) { // end of stream
            // TODO handle errors too here.
            // rewinddir(dir);
            // continue;
            break;
        }

        char *name = entry->d_name;

        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
                continue;

        unsigned int iPathLength = snprintf(NULL, 0, "%s", szDirPath) + 1;
        size_t iFullPathLength = iPathLength + _D_ALLOC_NAMLEN(entry) + 1; // for '/'
        char szFullPath[iFullPathLength];
        snprintf(szFullPath, iFullPathLength, "%s/%s", szDirPath, name);

#ifdef ___USE_MISC
        if ((entry->d_type == DT_DIR) /*&& (filter & D_DIRS)*/) {
            // FIXME pass in szFullPath??
            printf("Dir detected %s\n", szFullPath);
            enumerate_dir(szFullPath, eFilter, iAddedFiles, iOffset);
            continue;
        } else {
#else
        if (lstat(szFullPath, &st) < 0) {
            perror(szFullPath);
            continue;
        }
        if(S_ISDIR(st.st_mode)) {
            printf("lstat(%s) -> ISDIR.\n", szFullPath);
            enumerate_dir(szFullPath, eFilter, iAddedFiles, iOffset);
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

int on_init() {
    /* Get the initial elements loaded in the playlist in static struct.
     * These should roughly correspond to the positional arguments passed to mpv.
     * Returns the number of directories detected, or -1 on error.
     */
    int64_t pl_count = get_playlist_length();

    printf("Playlist length = %d.\n", pl_count);

    if (pl_count <= 1) {
        // mpv can handle loading a single directory just fine already.
        return 0;
    }

    g_InitialPL.count = pl_count;
    char **pl_entries = get_playlist_entries(pl_count);
    g_InitialPL.entries = pl_entries;

    clear_playlist();

    for (int i = 0; i < pl_count; ++i) {
        printf("Checking playlist[%d]: %s.\n", i, pl_entries[i]);

        if (isValidDirPath(pl_entries[i])) {
            g_iNumState++;
            dirState *_dirState = (dirState *) malloc(sizeof(dirState));
            if (_dirState == NULL) {
                perror("Failed allocating dirState");
                return -1;
            }
            g_aDirState = (dirState **) reallocarray(g_aDirState, g_iNumState, sizeof(dirState));
            if (g_aDirState == NULL) {
                perror("Failed allocating dirState");
                return -1;
            }
            g_aDirState[g_iNumState - 1] = _dirState;
            g_aDirState[g_iNumState - 1]->root_dir = pl_entries[i];
            printf("dir state: %s\n", g_aDirState[g_iNumState - 1]->root_dir);

            int64_t offset = 0;
            int64_t iAddedFiles = 0;
            enumerate_dir(pl_entries[i], D_NORMAL, &iAddedFiles, &offset);
            printf("Number of added files for %s: %d\n", pl_entries[i], iAddedFiles);
        } else {
            append_to_playlist(pl_entries[i]);
        }
        mpv_free(pl_entries[i]);
    }
    int64_t newcount = get_playlist_length();
    printf("New playlist length = %d.\n", newcount);

    char **entries = get_playlist_entries(newcount);
    --newcount;
    while (newcount >= 0) {
        mpv_free(entries[newcount]);
        --newcount;
    };
    free(entries);

    free(pl_entries);
    return g_iNumState;
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
    if (!fp) {
        perror("fopen");
        free(szConfPath);
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

            printf("Got limit key %d\n", MAX_READ_FILES);
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
            char prefix[strlen(szScriptName + 2)];
            strcpy(prefix, szScriptName);
            strncat(prefix, "-", 2);
            // "scriptname-" in found in key, get value
            if (strstr(_list->keys[i], prefix) != NULL) {
                printf("Found key prefix %s in %s.\n", prefix, _list->keys[i]);
                char key[strlen(_list->keys[i]) - strlen(prefix) + 1];
                memcpy(key, &(_list->keys[i])[strlen(prefix)], sizeof(key) - 1);
                key[sizeof(key) -1] = '\0';
                printf("key: %s. len: %d.\n", key, sizeof(key));

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
}

/* Fetch some more items from the file system.
 * 
 * @param amount The number of files to fetch.
 * @param method "append" simply appends until no more files are found in the 
 *               directories we keep track of. 
 *               "replace" clears the playlist before appending files. When
 *               there are no more new files in the directory, go back to the 
 *               beginning and load until amount is reached.
 */
void update(const char* method, uint64_t amount){
    // TODO print message to console mpv_command("show-text", ...)
    // const char *cmd[] = {"show-text", amount, NULL};
    // check_mpv_err(mpv_command(HANDLE, cmd));
    printf("Update with method %s, amount %d.\n", method, amount);


}


void message_hander(mpv_event *event, const char* szScriptName){
    mpv_event_client_message *msg = event->data;
    if (msg->num_args >= 2) {
        for (int i = 0; i < msg->num_args; ++i) {
            printf("Got message arg: %d %s\n", i, msg->args[i]);
        }
        if (strcmp(szScriptName, msg->args[0]) != 0) {
            return;
        }
        if ((strcmp(msg->args[1], "replace") != 0) 
           || (strcmp(msg->args[1], "append") != 0)) {
            if (msg->num_args == 2) {
                update(msg->args[1], MAX_READ_FILES);
                return;
            }
            char *stop;
            uint64_t amount = (uint64_t)strtol(msg->args[2], &stop, 10);
            update(msg->args[1], amount);
        }
    }
}

int mpv_open_cplugin(mpv_handle *handle){
    HANDLE = handle;
    const char* szScriptName = mpv_client_name(handle);
    printf("Loaded '%s'!\n", szScriptName);
    get_config(szScriptName);
    printf("Limit set to %d\n", MAX_READ_FILES);

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
            message_hander(event, szScriptName);
        if (event->event_id == MPV_EVENT_SHUTDOWN) {
            cleanup();
            break;
        }
    }
    return 0;
}
