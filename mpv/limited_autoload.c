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

#include <mpv/client.h>

mpv_handle *HANDLE = NULL;

struct dir_files {
    char **files; // paths of files read
    int64_t num_entries; // number of files in files currently in files
    int64_t offset; // value of dirent->d_off or returned by telldir()
} dir_files;

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
    // char **array = malloc(count * sizeof(char*));
    char **array = calloc(count, sizeof(char*));
    int length = snprintf(NULL, 0, "%d", count) + 1;
    length = length + 9 + 9 + 1;  // playlist/ + digit + filename + \0
    char query[length];
    for (int i = 0; i < count; ++i) {
        query[0] = '\0';
        snprintf(query, sizeof(query), "playlist/%d/filename", i);
        printf("Query prop: \"%s\".\n", query);

        char *path = mpv_get_property_string(HANDLE, query);
        if (!path) {
            fprintf(stderr, "Nothing more at index %d\n", i);
            return array;
        }
        printf("Playlist entry: %s.\n", path);

        array[i] = path; // only keep pointer, remember to mpv_free()!
    }
    return array;
}

int64_t playlist_length() {
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
    D_DIRS,
    D_NORMAL
};

int append_to_playlist(const char * path) {
    const char *cmd[] = {"loadfile", path, "append", NULL};
    int err = mpv_command(HANDLE, cmd);
    check_mpv_err(err);
}


int64_t recurse_dir(const char * cmd) {
    return 1;
}

/* Returns the number of files added to list
*/
int64_t enumerate_dir(DIR* dir,
                     const char* dirPath,
                     enum dirFilter filter,
                     int64_t maxfiles,
                     int64_t* offset )
{
    unsigned int pathLength = snprintf(NULL, 0, "%s", dirPath) + 1;
    const char *cmd[] = {"loadfile", NULL, "append", NULL};

    struct dirent *e;
    int64_t *added_files = 0;

    while (added_files < maxfiles) {
        e = readdir(dir);
        if (!e) { // end of stream
            // rewinddir(dir);
            // continue;
            break;
        }
        
        char *name = e->d_name;

        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
                continue;

        unsigned char type = e->d_type;

        // if (filter & D_FILES) 

        size_t full_path_length = pathLength + _D_ALLOC_NAMLEN(e) + 1; // for '/'
        char fullpath[full_path_length];
        snprintf(fullpath, full_path_length, "%s/%s", dirPath, name);

        cmd[1] = fullpath;
        mpv_command(HANDLE, cmd);

        ++added_files;
    }

    return added_files;
}

int on_init() {
    /* Get the initial elements loaded in the playlist.
     * These should correspond to the positional arguments passed to mpv.
     */
    int64_t count = playlist_length();
    printf("Playlist length = %d.\n", count);

    char **pl_entries = get_playlist_entries(count);

    const char *cmd[] = {"playlist-clear", NULL};
    int err = mpv_command(HANDLE, cmd);
    check_mpv_err(err);

    for (int i = 0; i < count; ++i) {
        printf("Current entry %d: %s.\n", i, pl_entries[i]);

        /* We want to load files from that entry if it's a directory */
        DIR *dir = NULL;
        dir = opendir(pl_entries[i]);
        if (dir != NULL) {
            int64_t offset = 0;
            int64_t num_added = enumerate_dir(dir, pl_entries[i], D_NORMAL, 5, &offset);
            printf("Number of added files for %s: %d\n", pl_entries[i], num_added);
        } else {
            append_to_playlist(pl_entries[i]);
        }
        mpv_free(pl_entries[i]);
    }


    int64_t newcount = playlist_length();
    printf("Playlist new length = %d.\n", newcount);

    get_playlist_entries(newcount);

    free(pl_entries);

    // DIR **dirs; // array of dirs to be loaded later.

    // for ( int i=0; i < sizeof(dirs); ++i ) {
    //     // load each entry into playlist
    //     readPartial(dirs[i], 3);
    //     closedir(dirs[i]);
    // }
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
            break;
    }
    return 0;
}
