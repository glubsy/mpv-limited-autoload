# Rationale

This script incrementally loads files from directories passed as positional arguments to MPV.
Key bindings are available to load more files from storage at the user's request.

This script might solve two issues:

1. By default, MPV loads files from a directory found in its playlist only as it stumbles upon it. This leads to issues when loading multiple directories in MPV, only the first one will be loaded, the other directories will be lost in the vast amount of files loaded by MPV from the first directory. Thus, their chance of being found is unfortunately greatly reduced.

2. Very large amounts of files are loaded into MPV's internal playlist, which may or may not be required by the user. Perhaps the user only wants a peek at what is inside the directory, perhaps the user wants a random selection of files. This script reduces the amount of files fetched from storage, akin to a "lazy load".

# Installation

## Build

```
gcc -o limited_autoload.so limited_autoload.c `pkg-config --cflags mpv` -shared -fPIC
```

Then copy `limited_autoload.so` into `${XDG_CONFIG_HOME}/mpv/scripts/` (or invoke the script with `mpv --scripts=/path/to/script`).

A prebuilt GNU/Linux binary is available for your convenience at the release page.

## Configuration

1. Configure the default amount of files fetched from storage with a file in `${XDG_CONFIG_HOME}/mpv/script-opts/limited_autoload.conf` with the following content:
```
limit=500
recurse=1
exclude=blend,rar,zip,tar,7z
````
* Initially load at most `limit` number of files from any directory found in the initial playlist, ie. the list of pathnames passed to MPV as positional arguments.
* If `recurse=0`, files from any sub-directory encountered will not be loaded.
* Files with a filename extension present in the `exclude` list will be skipped (case insensitive). 

You can also override these values from the command line: 
```
mpv --script-opts=limited_autoload-limit=800,limited_autoload-recurse=0,limited_autoload-exclude=blend:rar:zip:7z
```
Notice the delimiter is different for `exclude` on the command line. The values override those in the .conf file.

2. Add the following key bindings to `input.conf`, usually in `${XDG_CONFIG_HOME}/mpv/input.conf`:
```
f script-message limited_autoload append 100
F script-message limited_autoload replace 100
```
Change to whatever key you prefer.
"append" and "replace" are explained below.
The number represents the amount of files to fetch every time the key is pressed.

# Usage:

* "append" method: this will append files found in each sub-directory as they are returned by the operating system, similar to how MPV usually does on its own, except the files are NOT sorted in any way. When all files have been loaded, no more files will be appended. This is the "default" method.

* "replace" method: this will replace the current playlist with the next batch of files returned by the operating system each time the key is pressed. It acts as a dynamic "view" over the file system tree.

The `mpv_wrapper.sh` script is just a convenience shell script not directly related to this here script, but perhaps it might be useful to somebody.

# License

GPLv3

# Compatibility

This was written for GNU/Linux, but it could easily be ported to other Operating Systems by tweaking a few function calls and including the proper header files. I probably won't bother doing that myself.