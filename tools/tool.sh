#!/bin/bash
# Default some values for convenience
DEVICE_NAME=""
PROTOCOL="IPv4"
QUALITY="44100/16/2"
COMMAND_PIPE="/tmp/cliap.cmd"
VOLUME="50"
SECS="3"
AUTH="DEADBEEF"

# Locate the binary
CLIAP=`whereis -b cliap | awk -F: '{ print $2 }'`
if [[ -z "$CLIAP" ]]; then
    echo "cliap not found in \$PATH. Looking in src directory"
    CLIAP=`whereis -B src -b cliap | awk -F: '{ print $2 }'`
    if [ -z "$CLIAP" ]; then
        echo "cliap binary not found in \$PATH or in src directory"
        exit 1
    fi
fi

# Usage function for error messages
usage() {
  echo "A tool to make it easy to test $CLIAP from the command line."
  echo "Raw PCM audio is rad from stdin and played on the device"
  echo "Usage: $0 [-h] [-d device_name] [-4 | -6] [-q quality] [-c command_pipe] [-v volume]"
  echo "Defaults:"
  echo "    -4 for IPv4"
  echo "    44100/16/2 for quality"
  echo "    /tmp/cliap.cmd for command_pipe"
  echo "    50 for volume"
  exit 1
}

# Process options with getopts
# : suppresses errors; n: and t: expect arguments
while getopts ":hd:46q:c:v:" opt; do
  case $opt in
    h)
      usage
      exit 0
      ;;
    d)
      DEVICE_NAME="$OPTARG"
      ;;
    4)
      PROTOCOL="IPv4"
      ;;
    6)
      PROTOCOL="IPv6"
      ;;
    q)
      QUALITY="$OPTARG"
      ;;
    c)
      COMMAND_PIPE="$OPTARG"
      ;;
    v)
      VOLUME="$OPTARG"
      ;;
    :)
      echo "Error: -${OPTARG} requires an argument."
      usage
      ;;
    \\?)
      echo "Error: Invalid option: -${OPTARG}"
      usage
      exit 1
      ;;
  esac
done

# Shift positional parameters past the options
shift $((OPTIND - 1))

# Process remaining positional arguments (e.g., filenames)
echo "Positional arguments (files): $@"
echo "Device Name: $DEVICE_NAME, Protocol: $PROTOCOL"

# Validate arguments
if [[ -p "$COMMAND_PIPE" ]]; then
    echo "Command pipe $COMMAND_PIPE already exists. Good."
elif [[ -f "$COMMAND_PIPE" ]]; then
    echo "$COMMAND_PIPE already exists, but is not a named pipe"
    usage
    exit 1
else
    if mkfifo "$COMMAND_PIPE" ; then
        echo "Command pipe $COMMAND_PIPE created."
    else
        echo "Error creating command pipe"
        exit 1
    fi
fi


# Resolve the mDNS entry for the protocol
MDNS=`avahi-browse -p -t -k -v -r _airplay._tcp | \
    sed -f tool.sed | \
    grep "$DEVICE_NAME" | \
    grep "$PROTOCOL" | \
    grep "^="`


if [ -z "$MDNS" ]; then
    echo "Device $DEVICE_NAME not found with $PROTOCOL details"
    usage
    exit 1
else
    echo "Found $PROTOCOL info for $DEVICE_NAME. $MDNS"
fi

# extract the fields we need for cliap
HOSTNAME=`echo $MDNS | awk -F\; '{ print $7 }'`
ADDRESS=`echo $MDNS | awk -F\; '{ print $8 }'`
PORT=`echo $MDNS | awk -F\; '{ print $9 }'`
TXT=`echo $MDNS | awk -F\; '{ print $10 }'`

NTP=`$CLIAP --ntp`
NTPSTART=$(( $NTP + (1<<33)*$SECS ))

CMD="$CLIAP --loglevel 4 \
    --name "$DEVICE_NAME" \
    --hostname "$HOSTNAME" \
    --address "$ADDRESS" \
    --port "$PORT" \
    --txt "$TXT" \
    --dacp_id "$AUTH" \
    --pipe - \
    --command_pipe "$COMMAND_PIPE" \
    --ntpstart "$NTPSTART" \
    --volume "$VOLUME" \
    --quality "$QUALITY" "
echo $CMD