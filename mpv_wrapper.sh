#!/bin/bash
# Wrapper script for mpv
# Usage: ${0} path/to/directory [path/to/directoriy, ...]

declare -a PARAMS
PARAM_COUNT=0
while (( "$#" )); do
  case "$1" in
    -s|--shuffle) # shuffle path passed
      SHUFFLE=1
      shift
      ;;
    -v|--video-only) # shuffle path passed
      VID_ONLY=1
      #FIXME probably best to include video formats only
      EXCLUDE="jpg,jpeg,png,bmp,tiff,zip,rar,7z,blend,py,pdf,txt"
      shift
      ;;
    -z|--sort-size) # sort by descending file sizes
      SORTED=1
      shift
      ;; 
    -x|--exclude) # exclude these comma separated extensions
      if [ -n "$2" ] && [ ${2:0:1} != "-" ]; then
        EXCLUDE=$2
        shift 2
      else
        echo "Error: Argument for $1 is missing" >&2
        exit 1
      fi
      ;;
    -xp|--exclude-path) # exclude this path (can have multiple)
      if [ -n "$2" ] && [ ${2:0:1} != "-" ]; then
        EXCLUDE_PATHS+=(${2})
        shift 2
      else
        echo "Error: Argument for $1 is missing" >&2
        exit 1
      fi
      ;;
    -p|--no-find) # don't use find, only generate plain playlist of directories and let mpv autoload
     PLAIN=1
     shift
     ;;
    -*|--*=) # unsupported flags
      echo "Error: Unsupported flag $1" >&2
      exit 1
      ;;
    *) # preserve positional arguments
      PARAMS+=("$1")
      PARAM_COUNT=$((PARAM_COUNT+1))
      shift
      ;;
  esac
done
# set positional arguments in their proper place
eval set -- "${PARAMS[@]}"

# Set a default sane exclusion filter
if [[ -z ${EXCLUDE} ]]; then
	EXCLUDE="blend,py,pdf,txt"
fi

echo -e "DEBUG POS PARAMS:${PARAMS[@]}\nPARAM_COUNT=${PARAM_COUNT}\nEXCLUDE:${EXCLUDE}"

if [[ ! -z ${EXCLUDE} ]]; then
	# replace commas with spaces
	for EXT in ${EXCLUDE//,/ }; do
		if [ -z "${FIND_OPTS}" ]; then
			FIND_OPTS="-not -iname \"*.${EXT}\"";
		else
			FIND_OPTS="${FIND_OPTS} -a -not -iname \"*.${EXT}\"";
		fi
	done;
	FIND_CMD="find ${PARAMS[@]} -type f \( ${FIND_OPTS} \)"
	if [[ "${SORTED}" -eq 1 ]]; then
		# sorted by decreasing file size https://stackoverflow.com/questions/22598205/how-sort-find-result-by-file-sizes
		FIND_CMD="${FIND_CMD} -printf '%s\t%p\n' | sort -nr | cut -f2-"
	fi
else
	FIND_CMD="find ${PARAMS[@]} -type f";
fi

if [[ ! -z ${EXCLUDE_PATHS} ]]; then
	FIND_OPTS=""
	for EPATH in "${EXCLUDE_PATHS[@]}"; do
		FIND_OPTS="${FIND_OPTS} -and -not -ipath \"*${EPATH}\"*"
	done;
	FIND_CMD="${FIND_CMD} ${FIND_OPTS}"
fi

if [[ "${SHUFFLE}" -eq 1 ]]; then
	if [[ ${PARAM_COUNT} -gt 1 ]]; then # gather files THEN shuffle
		echo "DEBUG:$FIND_CMD"; eval ${FIND_CMD};
		eval ${FIND_CMD} | mpv --shuffle --playlist=-;
	elif [[ -z ${EXCLUDE_PATH} && ${EXCLUDE} == "" ]]; then 
		# only one dir passed and no special rule, let mpv load & shuffle
		# can't seem to pass exclusion filters to mpv?
		mpv --shuffle -- "${PARAMS[@]}"
	else
		echo "DEBUG:$FIND_CMD"; eval ${FIND_CMD};
		eval ${FIND_CMD} | mpv ${OPTIONS} --playlist=-;
	fi
elif [[ "${PLAIN}" -eq 1 ]]; then
	# Build string from arguments, separated by \n characters
	# and feed it to mpv as a playlist. mpv will then auto-load files 
	# as they are being read.
	PAR="";
	for p in "$@"; do 	# or ${PARAMS[@]}
            PAR="${p}$'\n'${PAR}";
	done;
	echo "DEBUG: PAR: ${PAR}";
	eval echo "${PAR}" | mpv ${OPTIONS} --playlist=- --;

else
	echo "DEBUG:$FIND_CMD"; eval ${FIND_CMD};
	eval ${FIND_CMD} | mpv ${OPTIONS} --playlist=- --;

fi

# TODO 
# * save playlist 
# * limit find output with | head -500

# Old SHUFFLE version means the shell is expanding, which implies race conditions
#cd "${1}" || echo "couldn't change dir to ${1}";
#mpv --shuffle -- *

# DIR version, only takes one directory path, loads the files in it
# https://github.com/mpv-player/mpv/tree/master/TOOLS/lua
#loads all other files in directory
#mpv --script=~/.config/mpv/scripts/autoload.lua -- *
