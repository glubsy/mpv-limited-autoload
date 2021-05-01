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
#include <string.h>
#include <sys/stat.h>

#include <mpv/client.h>

mpv_handle *HANDLE = NULL;
int64_t MAX_READ_FILES = 5; // DEBUG
const char *LOAD_CMD[] = {"loadfile", NULL, "append", NULL};


typedef struct DirState {
    char *root_dir;
    int64_t num_entries; // number of files added last time
    char *last_dir; // last sub-directory read
    int64_t offset; // value of dirent->d_off or returned by telldir() from the last_dir
} dirState;

dirState **g_aDirState = NULL;
int64_t g_iNumState = 0;

struct InitialPlaylist{
    // This is sort of like getting argc + argv from mpv
    int64_t count; // number of items in the playlist after mpv's first load
    char **entries; // entries in the playlist after mpv's first load
};

struct InitialPlaylist g_InitialPL;

int check_mpv_err(int status) {
    if( status < MPV_ERROR_SUCCESS ){
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
    struct stat st;
    DIR *_dir = NULL;
    _dir = opendir(szDirPath);
    printf("DEBUG opening: %s\n", szDirPath);

    while (*iAddedFiles < MAX_READ_FILES) {
        entry = readdir(_dir);
        if (!entry) { // end of stream
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

#ifdef __USE_MISC
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
            printf("Dir stat detected %s\n", szFullPath);
            enumerate_dir(szFullPath, eFilter, iAddedFiles, iOffset);
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
    int err = mpv_command(HANDLE, cmd);
    check_mpv_err(err);
}

int on_init() {
    /* Get the initial elements loaded in the playlist.
     * These should correspond to the positional arguments passed to mpv.
     */
    int64_t pl_count = get_playlist_length();
    g_InitialPL.count = pl_count;
    printf("Playlist length = %d.\n", g_InitialPL.count);

    char **pl_entries = get_playlist_entries(pl_count);
    g_InitialPL.entries = pl_entries;

    clear_playlist();

    for (int i = 0; i < pl_count; ++i) {
        printf("Current entry %d: %s.\n", i, pl_entries[i]);

        /* We want to load files from that entry if it's a directory */
        DIR *dir = NULL;
        dir = opendir(pl_entries[i]);
        if (dir != NULL) {
            closedir(dir);
            g_iNumState++;
            dirState *_dirState = (dirState *) malloc(sizeof(dirState));
            g_aDirState = (dirState **) realloc(g_aDirState, g_iNumState * sizeof(dirState));
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
}

void cleanup(void){
    return;
}

int mpv_open_cplugin(mpv_handle *handle) {
    HANDLE = handle;
    printf("Loaded '%s'!\n", mpv_client_name(handle));
    // mpv_hook_add(handle, 0, "on_before_start_file", 50);
    on_init();

    while (1) {
        mpv_event *event = mpv_wait_event(handle, -1);
        // printf("Got event: %d\n", event->event_id);
        // if (event->event_id == MPV_EVENT_HOOK)
        //     on_before_start_file_handler(event);
        if (event->event_id == MPV_EVENT_SHUTDOWN)
            cleanup();
            break;
    }
    return 0;
}
