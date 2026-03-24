/**
 * @brief Reads raw PCM audio and commands/metadata from named pipes and streams to on OwnTone player
 * 
 * About mass.c
 * ------------
 * This was copied from pipe.c from the OwnTone repo and adapted
 * to provide an interface between Music Assistant and the OwnTone codebase
 * using a pair of named pipes.
 * 
 * Raw PCM data is read from one named pipe and streamed to the player
 * Metadata and commands are read from a second named pipe and processed.
 * Player status is reported back to Music Assistant on stderr
 * This module is considered to be an input backend module in OwnTone parlance.
 * It runs in two threads:
 *  1. mass_aud: Responsible for reading raw PCM audio from a named pipe and supplying
 *      it to the input module
 *  2. mass_cmd: Responsible for handling metadata and commands received from 
 *      Music Assistant and reporting player status.
 * 
 * A mutex is used to ensure integrity of data objects shared between the mass_aud
 * and mass_cmd threads.
 *
 * original code:
 * Copyright (C) 2017 Espen Jurgensen
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <stdint.h>
#include <inttypes.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>

#include <event2/event.h>
#include <event2/buffer.h>

// #include <libavutil/samplefmt.h>
// #include <libavutil/timestamp.h>
// #include <libavformat/avformat.h>


#include "artwork.h"
#include "cliap.h"
#include "commands.h"
#include "conffile.h"
#include "db.h"
#include "http.h"
#include "input.h"
#include "limits.h"
#include "listener.h"
#include "logger.h"
#include "mass.h"
#include "misc.h"
#include "misc_xml.h"
#include "player.h"
#include "worker.h"
#include "commands.h"
#include "rtp_common.h"
#include "mass.h"
#include "wrappers.h"
#include "transcode.h"

#define MASS_UPDATE_INTERVAL_SEC   1 // every second
#define MASS_METADATA_KEYVAL_SEP   "="  // Key-value separator in metadata
#define MASS_METADATA_PROGRESS_KEY "PROGRESS"
#define MASS_METADATA_VOLUME_KEY   "VOLUME"
#define MASS_METADATA_ARTWORK_KEY  "ARTWORK"
#define MASS_METADATA_ALBUM_KEY    "ALBUM"
#define MASS_METADATA_TITLE_KEY    "TITLE"
#define MASS_METADATA_ARTIST_KEY   "ARTIST"
#define MASS_METADATA_DURATION_KEY "DURATION"
#define MASS_METADATA_ACTION_KEY   "ACTION"
#define MASS_METADATA_PIN_KEY      "PIN"

#define STDIN_FILENAME  "-"

// Select one of the three below options for 24-bit demuxing solution
// Note: transcoding with OwnTone transcode module currently not working fully. It appears that this use-case
// is beyond the design capabilities of the transcode module. After applying fixes to transcode.c to ensure
// valid demuxer options are supplied and that zero size read packets are ignored, the decoding started to work
// but gave the followig warnings/errors:
// ffmpeg: Multiple frames in a packet.
// ffmpeg: Invalid PCM packet, data has size 4 but at least a size of 6 was expected
// fifo: play:Unexpected transcoding mismatch. From 4096 raw bytes, expected to transcode 5461 bytes, but actually decoded 4092 bytes.
// CONCLUSION: Stick with local demuxer
// SUGGESTION: It might be that the transcoding module should be fed with a single unmuxed frame at a time i.e. 3 bytes and then it might work
// but then the overhead is huge compared to our local demuxer.
#define DEMUX_LOCAL            1 // Set to 1 to use local demux_24_to_32() 
#define DEMUX_TRANSCODE_DECODE 0 // Set to 1 to use transcode_decode() for 24-bit demuxing.
#define DEMUX_TRANSCODE        0 // Set to 1 to do full transcode() for 24-bit demuxing.

#define DEBUG_MASS 0
#define DEBUG_DEMUX 0

/* from cliap.c */
extern ap_device_info_t ap_device_info;
extern mass_named_pipes_t mass_named_pipes;

 /* mass specific stuff */
static struct event *mass_timer_event = NULL;
static struct timeval mass_tv = { MASS_UPDATE_INTERVAL_SEC, 0};
static struct timespec paused_start_ts = {0, 0};
static bool player_started = false;
static bool player_paused = false;
static int pipe_id = 0; // make a global of the id of our audio named pipe
static pthread_mutex_t audio_command_lock; // for mass_cmd <> mass_aud inter-thread cooridnation
static bool pause_flag = false; // we control when to pause and (re)commence reading from the audio pipe
static bool stop_flag = false; // used to communicate the receipt of a STOP command between mass_cmd and mass_aud threads

// Maximum number of pipes to watch for data
#define PIPE_MAX_WATCH 4
// Max number of bytes to read from the input streams (both audio and commands/metadata) at a time
#define PIPE_READ_MAX 65536
// Max number of bytes to buffer from the command/metadata pipe
#define PIPE_METADATA_BUFLEN_MAX 1048576
// Ignore pictures with larger size than this
#define PIPE_PICTURE_SIZE_MAX 1048576
// Where we store pictures for the artwork module to read
#define PIPE_TMPFILE_TEMPLATE "/tmp/" PACKAGE_NAME ".XXXXXX.ext"
#define PIPE_TMPFILE_TEMPLATE_EXTLEN 4

struct mass_ctx
{
  struct pipe *pipe;
#if DEMUX_TRANSCODE_DECODE
  struct decode_ctx *decode_ctx; // optional context if demuxing/decoding is required
#elif DEMUX_TRANSCODE
  struct transcode_ctx *transcode_ctx; // optional context if transcoding is required
#endif
  struct evbuffer *evbuf;  // the evbuffer to read raw audio into
};

enum pipetype
{
  PIPE_PCM,
  PIPE_METADATA,
};

enum pipe_metadata_msg
{
  PIPE_METADATA_MSG_METADATA         = (1 << 0),
  PIPE_METADATA_MSG_PROGRESS         = (1 << 1),
  PIPE_METADATA_MSG_VOLUME           = (1 << 2),
  PIPE_METADATA_MSG_PICTURE          = (1 << 3),
  PIPE_METADATA_MSG_FLUSH            = (1 << 4),
  PIPE_METADATA_MSG_PARTIAL_METADATA = (1 << 5),
  PIPE_METADATA_MSG_STOP             = (1 << 6),
  PIPE_METADATA_MSG_PAUSE            = (1 << 7),
  PIPE_METADATA_MSG_PLAY             = (1 << 8),
  PIPE_METADATA_MSG_PIN              = (1 << 9),
};

struct pipe
{
  int id;               // The mfi id of the pipe
  int fd;               // File descriptor
  char *path;           // Path
  enum pipetype type;   // PCM (audio) or metadata
  event_callback_fn cb; // Callback when there is data to read
  struct event *ev;     // Event for the callback

  struct pipe *next;
};

// struct for storing the data received via a metadata/command pipe
// We will never receive the artwok as a file, it will always be
// via a URL, which is handled in input_metadata struct.
struct pipe_metadata_prepared
{
  // Progress, artist etc goes here
  struct input_metadata input_metadata;
  // Picture (artwork) data
  int pict_tmpfile_fd;
  char pict_tmpfile_path[sizeof(PIPE_TMPFILE_TEMPLATE)];
  // Volume
  int volume;
  // PIN
  char *pin; // 4 digit PIN
  // Mutex to share the prepared metadata
  pthread_mutex_t lock;
};

// Extension of struct pipe with extra fields for metadata handling
struct pipe_metadata
{
  // Pipe that we start watching for metadata after playback starts
  struct pipe *pipe;
  // We read metadata into this evbuffer
  struct evbuffer *evbuf;
  // Storage of current metadata
  struct pipe_metadata_prepared prepared;
  // True if there is new metadata to push to the player
  bool is_new;
};

union pipe_arg
{
  uint32_t id;
  struct pipe *pipelist;
};

// The usual thread stuff
static pthread_t tid_audio_pipe;
static pthread_t tid_command_pipe;
static struct event_base *evbase_audio_pipe;
static struct event_base *evbase_command_pipe;
static struct commands_base *cmdbase;

// Global list of pipes we are watching (if watching/autostart is enabled)
static struct pipe *pipe_watch_list;

// Pipe + extra fields that we start watching for metadata after playback starts
static struct pipe_metadata pipe_metadata;

/* -------------------------------- HELPERS --------------------------------- */

/** Player status is human readable format
 *
 * @param  status  the player status
 * @returns player status in human readable format
 */
static const char *play_status_str(enum play_status status)
{
  switch (status)
    {
    case PLAY_STOPPED:
      return "stopped";
    case PLAY_PAUSED:
      return "paused";
    case PLAY_PLAYING:
      return "playing";
    default:
      return "unknown";
    }
}

/**
 * Determines if demuxing of raw input audio is required.
 * 
 * @note
 * Raw 24-bit audio needs to be demuxed to get the data into s32
 * sample format
 * 
 * @param quality the quality parameters of the raw input audio
 * @returns true if demuxing required, false if not
 */
static bool
demux_required(struct media_quality *quality)
{
  if (quality->bits_per_sample == 24) {
    return true;
  }
  return false;
}

#if DEMUX_LOCAL
/**
 * Demuxes a raw 24-bit source evbuffer to a 24-in-32 destination evbuffer
 * 
 * @note
 * Source evbuffer is modified, with the demuxed bytes removed from it.
 * It may contain residual raw bytes if it did not contain an integral
 * number of 24-bit frames.
 * 
 * @param dst the destination evbuffer to add the demuxed frames
 * @param src the source evbuffer to extract raw frames
 * @returns -1 on failure, number of demuxed bytes on success
 */
static inline int
demux_to_24_in_32(struct evbuffer *dst, struct evbuffer *src)
{
  uint8_t demuxed_frame[4] = {0x00, 0x00, 0x00, 0x00};
  size_t demuxed_bytes = 0;
  size_t removed_bytes = 0;
  size_t residual_bytes = evbuffer_get_length(src);
  int ret;

  while (residual_bytes >= 3) {
    removed_bytes = evbuffer_remove(src, &demuxed_frame[1], 3); // assumes little endian??
    if (removed_bytes != 3) {
      DPRINTF(E_LOG, L_FIFO, "%s:Error trying to remove 3 bytes from src evbuffer. src evbuffer length = %ld.\n",
        __func__, residual_bytes
      );
      goto out_err;
    }
    demuxed_bytes += removed_bytes;
    residual_bytes = evbuffer_get_length(src);

#if DEBUG_DEMUX
    if (demuxed_frame[1] || demuxed_frame[2] || demuxed_frame[3]) {
      // We have something other than silence
      DPRINTF(E_SPAM, L_FIFO, "%s: Demuxed frame 0x%02x,0x%02x,0x%02x,0x%02x. Demuxed bytes:%ld. Residual bytes:%ld.\n", 
        __func__, demuxed_frame[0], demuxed_frame[1], demuxed_frame[2], demuxed_frame[3],
        demuxed_bytes, residual_bytes
      );
    }
#endif

    ret = evbuffer_add(dst, demuxed_frame, 4);
    if (ret < 0) {
      DPRINTF(E_LOG, L_FIFO, "%s:Error adding frame 0x%02x,0x%02x,0x%02x,0x%02x to evbuffer. Demuxed bytes = %ld",
        __func__, demuxed_frame[0], demuxed_frame[1], demuxed_frame[2], demuxed_frame[3], demuxed_bytes
      );
      goto out_err;
    }
  }
#if DEBUG_DEMUX
  DPRINTF(E_DBG, L_FIFO, "%s:Success. Demuxed:%ld. Residual:%ld. Destination evbuffer length:%ld.\n",
    __func__, demuxed_bytes, residual_bytes, evbuffer_get_length(dst)
  );
#endif
  return demuxed_bytes;

out_err:
  DPRINTF(E_LOG, L_FIFO, "%s:Error. Demuxed:%ld. Residual:%ld. Destination evbuffer length:%ld.\n",
    __func__, demuxed_bytes, residual_bytes, evbuffer_get_length(dst)
  );
  return -1;
}
#elif DEMUX_TRANSCODE_DECODE | DEMUX_TRANSCODE

static enum transcode_profile
quality_to_xcode(struct media_quality *quality)
{
  if (quality->bits_per_sample == 16)
    return XCODE_PCM16;
  if (quality->bits_per_sample == 24)
    return XCODE_PCM24;
  if (quality->bits_per_sample == 32)
    return XCODE_PCM32;

  return XCODE_UNKNOWN;
}
#endif

/** 
 * Check if a path refers to stdin
 * 
 * @param path  the path to check
 * @returns true if path is "-" (stdin), false otherwise
 */
static bool
is_stdin(const char *path)
{
  return (strncmp(path, STDIN_FILENAME, strlen(STDIN_FILENAME)) == 0);
}

/**
 * Create a pipe data structure. Allocates memory for the data structure.
 * @param path  filename path
 * @param id    ID number to give the pipe
 * @param type  the type of pipe we are creating.
 * @param cb    callback function
 * @returns     the populated pipe data structure
 */
static struct pipe *
pipe_create(const char *path, int id, enum pipetype type, event_callback_fn cb)
{
  struct pipe *pipe;

  CHECK_NULL(L_FIFO, pipe = calloc(1, sizeof(struct pipe)));
  pipe->path  = strdup(path);
  pipe->id    = id;
  pipe->fd    = -1;
  pipe->type  = type;
  pipe->cb    = cb;

  return pipe;
}

/** Free memory allocated for a pipe data structure.
 * @param pipe  the pipe data structure to be freed
 */
static void
pipe_free(struct pipe *pipe)
{
  free(pipe->path);
  free(pipe);
}

/** Opens a file for reading and validates it is a FIFO pipe or stdin
 * @param path    the pathname of the file to open, or "-" for stdin
 * @param silent  suppress fstat error if it occurs
 * @returns the file descriptor on success, -1 on failure
 */
static int
pipe_open(const char *path, bool silent)
{
  struct stat sb;
  int fd;

  // Handle stdin special case
  if (is_stdin(path)) {
    DPRINTF(E_INFO, L_FIFO, "Using stdin for audio input\n");
    return STDIN_FILENO;
  }

  DPRINTF(E_SPAM, L_FIFO, "(Re)opening pipe: '%s'\n", path);

  fd = open(path, O_RDONLY | O_NONBLOCK);
  if (fd < 0) {
    DPRINTF(E_LOG, L_FIFO, "Could not open pipe for reading '%s': %s\n", path, strerror(errno));
    goto error;
  }

  if (fstat(fd, &sb) < 0) {
    if (!silent)
      DPRINTF(E_LOG, L_FIFO, "Could not fstat() '%s': %s\n", path, strerror(errno));
    goto error;
  }

  if (!S_ISFIFO(sb.st_mode)) {
    DPRINTF(E_LOG, L_FIFO, "Source type is pipe, but path is not a fifo: %s\n", path);
    goto error;
  }

  return fd;

 error:
  if (fd >= 0)
    close(fd);

  return -1;
}

/** Close a pipe
 * @param fd  file descriptor of the pipe to close
 * @note  Does not close stdin (fd 0)
 */
static void
pipe_close(int fd)
{
  if (fd > 0)  // Don't close stdin
    close(fd);
}

/** Add a libevent read event for the pipe
 * @param pipe    the pipe to watch for data to read
 * @param evbase  the event base to add the event to
 * @returns 0 on success, -1 on failure
 * @note For stdin, no libevent watch is added - playback is started immediately
 */
static int
watch_add(struct pipe *pipe, struct event_base *evbase)
{
  int ret;

  pipe->fd = pipe_open(pipe->path, 0);
  if (pipe->fd < 0)
    return -1;

  DPRINTF(E_DBG, L_FIFO, "%s: Opened pipe '%s' with fd %d\n", __func__, pipe->path, pipe->fd);

  // For stdin, don't use libevent - stdin is always "ready" which causes
  // the callback to fire continuously. Instead, just return success and
  // let the caller start playback directly.
  if (is_stdin(pipe->path))
    {
      DPRINTF(E_INFO, L_FIFO, "%s: Using stdin - skipping libevent watch\n", __func__);
      pipe->ev = NULL;
      return 0;
    }

  pipe->ev = event_new(evbase, pipe->fd, EV_READ | EV_PERSIST, pipe->cb, pipe);
  if (!pipe->ev)
    {
      DPRINTF(E_LOG, L_FIFO, "Could not watch pipe for new data '%s'\n", pipe->path);
      pipe_close(pipe->fd);
      return -1;
    }

  ret = event_add(pipe->ev, NULL);
  DPRINTF(E_DBG, L_FIFO, "%s: event_add returned %d for pipe '%s'\n", __func__, ret, pipe->path);

  return 0;
}

/** Delete the read event for the pipe and close the pipe
 * @param pipe  the pipe to delete the read event for
 */
static void
watch_del(struct pipe *pipe)
{
  if (pipe->ev)
    event_free(pipe->ev);

  pipe_close(pipe->fd);

  pipe->fd = -1;
}

/** Reset the libevent read event for the command/metadata pipe
 * @param pipe  the metadata/command pipe to reset
 * @returns 0 on success, -1 on failure
 */
static int
watch_reset_metadata(struct pipe *pipe)
{
  if (!pipe)
    return -1;

  watch_del(pipe);

  return watch_add(pipe, evbase_command_pipe);
}

/**
 * Add a pipe to a pipelist
 * @param list  the pipelist to be added to
 * @param pipe  the pipe to add to the pipelist
 */
static void
pipelist_add(struct pipe **list, struct pipe *pipe)
{
  pipe->next = *list;
  *list = pipe;
}

/** Remove a pipe from a pipelist
 * @param list  the pipelist to removed from
 * @param pipe  the pipe to remove from the pipelist
 */
static void
pipelist_remove(struct pipe **list, struct pipe *pipe)
{
  struct pipe *prev = NULL;
  struct pipe *p;

  for (p = *list; p; p = p->next)
    {
      if (p->id == pipe->id)
	break;

      prev = p;
    }

  if (!p)
    return;

  if (!prev)
    *list = pipe->next;
  else
    prev->next = pipe->next;

  pipe_free(pipe);
}

/** Find a pipe in a pipelist
 * @param list  the pipelist to search
 * @param id    the id of the pipe in the pipelist to find
 * @returns pointer to the pipe if found, else NULL if not found
 */
static struct pipe *
pipelist_find(struct pipe *list, int id)
{
  struct pipe *p;

  for (p = list; p; p = p->next)
    {
      if (id == p->id)
	return p;
    }

  return NULL;
}

/** Close a temporary artwork file and remove it
 * @param fd  file descriptor of the temporary artwork file
 * @param path  the pathname of the temporary artwork file
 */
static void
pict_tmpfile_close(int fd, const char *path)
{
  if (fd < 0)
    return;

  close(fd);
  unlink(path);
}

/** Recreates a tmpfile to store metadata artwork in.
 * @param path      pathname of the temporary artwork file
 * @param path_size length of the pathanme
 * @param fd        file descriptor for the tmpfile. If non-negative, then the file will be 
 *                  closed and deleted (using unlink, so path must be valid).
 * @param ext       extension to use for the tmpfile, eg .jpg or .png. Extension cannot 
 *                  be longer than PIPE_TMPFILE_TEMPLATE_EXTLEN.
 * @returns file descriptor of the tmpfile on success, -1 on failure. path will be updated
 *          with the new tmpfile name
 */
static int
pict_tmpfile_recreate(char *path, size_t path_size, int fd, const char *ext)
{
  int offset = strlen(PIPE_TMPFILE_TEMPLATE) - PIPE_TMPFILE_TEMPLATE_EXTLEN;

  if (strlen(ext) > PIPE_TMPFILE_TEMPLATE_EXTLEN)
    {
      DPRINTF(E_LOG, L_FIFO, "Invalid extension provided to pict_tmpfile_recreate: '%s'\n", ext);
      return -1;
    }

  if (path_size < sizeof(PIPE_TMPFILE_TEMPLATE))
    {
      DPRINTF(E_LOG, L_FIFO, "Invalid path buffer provided to pict_tmpfile_recreate\n");
      return -1;
    }

  pict_tmpfile_close(fd, path);

  strcpy(path, PIPE_TMPFILE_TEMPLATE);
  strcpy(path + offset, ext);

  fd = mkstemps(path, PIPE_TMPFILE_TEMPLATE_EXTLEN);

  return fd;
}

/** Retrieves artwork from a URL and writes the artwork to a tmpfile.
 * The tmpfile path is stored in prepared->pict_tmpfile_path and can be 
 * used by the output module.
 * The artwork URL is freed and updated to reference the tmpfile 
 * 
 * @param prepared   Prepared metadata struct containing the artwork URL and tmpfile path
 * @return        ART_FMT_* on success, ART_E_NONE or ART_E_ERROR on error
*/
static int
parse_artwork_url(struct pipe_metadata_prepared *prepared)
{
  struct input_metadata *m = &prepared->input_metadata;
  const char *ext;
  ssize_t ret;
  int format = ART_E_ERROR;
  struct evbuffer *raw;
  size_t artwork_image_size = 0;
  char *artwork_image = NULL;

  CHECK_NULL(L_FIFO, raw = evbuffer_new());

  format = artwork_read_byurl(raw, m->artwork_url);
  if (format <= 0) {
    DPRINTF(E_LOG, L_FIFO, "Could not read artwork from URL '%s'\n", m->artwork_url);
    goto error;
  }

  artwork_image_size = evbuffer_get_length(raw);
  artwork_image = malloc(artwork_image_size);
  if (!artwork_image) {
    DPRINTF(E_LOG, L_FIFO, "Could not allocate memory for artwork from URL '%s'\n", m->artwork_url);
    goto error;
  }
  ret = evbuffer_remove(raw, artwork_image, artwork_image_size);
  if (ret < 0 || (size_t)ret != artwork_image_size) {
    DPRINTF(E_LOG, L_FIFO, "Could not extract artwork from evbuffer for URL '%s'\n", m->artwork_url);
    goto error;
  }
  evbuffer_free(raw);
  raw = NULL;

  if (format == ART_FMT_JPEG)
    ext = ".jpg";
  else if (format == ART_FMT_PNG)
    ext = ".png";
  else {
    DPRINTF(E_LOG, L_FIFO, "Unsupported picture format from artwork URL '%s'\n", m->artwork_url);
    goto error;
  }

  free(m->artwork_url);
  m->artwork_url = NULL;

  prepared->pict_tmpfile_fd = pict_tmpfile_recreate(
    prepared->pict_tmpfile_path, sizeof(prepared->pict_tmpfile_path), prepared->pict_tmpfile_fd, ext
  );
  if (prepared->pict_tmpfile_fd < 0) {
    DPRINTF(E_LOG, L_FIFO, "Could not open tmpfile for pipe artwork '%s': %s\n",
      prepared->pict_tmpfile_path, strerror(errno)
    );
    goto error;
  }

  ret = write(prepared->pict_tmpfile_fd, artwork_image, artwork_image_size);
  if (ret < 0) {
    DPRINTF(E_LOG, L_FIFO, "Error writing artwork from metadata pipe to '%s': %s\n",
      prepared->pict_tmpfile_path, strerror(errno)
    );
    goto error;
  }
  else if (ret != artwork_image_size) {
    DPRINTF(E_LOG, L_FIFO, "Incomplete write of artwork to '%s' (%zd/%ld)\n",
      prepared->pict_tmpfile_path, ret, artwork_image_size
    );
    goto error;
  }

  DPRINTF(E_SPAM, L_FIFO, "Wrote pipe artwork to '%s'\n", prepared->pict_tmpfile_path);

  m->artwork_url = safe_asprintf("file:%s", prepared->pict_tmpfile_path);
  free(artwork_image);

  return 0;

 error:
  if (m->artwork_url) free(m->artwork_url);
  m->artwork_url = NULL;
  if (raw) evbuffer_free(raw);
  if (artwork_image) free(artwork_image);
  return -1;
}

/** Extract key and value from an input string and allocate memory.
 * e.g. key=value
 * @param input_string  string containing `=` delimited key and value
 * @param key           key is returned after memory allocated for it
 * @param value         value is returned after memory allocated for it.
 * @note The delimiter between the key and value is '='
 * @note  The consumer of the key and value must free the allocated memory once consumed.
 */
static
void extract_key_value(const char *input_string, char **key, char **value) 
{
    char *delimiter_pos = strchr(input_string, '='); // Find the '=' delimiter

    if (delimiter_pos == NULL) {
        // Handle cases where the delimiter is not found
        *key = NULL;
        *value = NULL;
        return;
    }

    // Calculate lengths
    size_t key_len = delimiter_pos - input_string;
    size_t value_len = strlen(delimiter_pos + 1);

    // Allocate memory for key and value
    *key = (char *)malloc(key_len + 1);
    *value = (char *)malloc(value_len + 1);

    if (*key == NULL || *value == NULL) {
        // Handle memory allocation failure
        free(*key); // Free if one was allocated
        free(*value);
        *key = NULL;
        *value = NULL;
        return;
    }

    // Copy the key
    strncpy(*key, input_string, key_len);
    (*key)[key_len] = '\0'; // Null-terminate the key

    // Copy the value
    strcpy(*value, delimiter_pos + 1); // Copy from after the delimiter
}

/** Parse one metadata/command item from Music Assistant
 * @param out_msg   the type of metadata or command received is returned in out_msg
 * @param prepared  updated metadata information is returned in prepared 
 * @param item      the metadata or command item received from Music Assistant to be parsed
 * @note  prepared is a shared with the input thread and this function exists within a mutex lock.
 *        This means we must not do anything that could cause a deadlock (e.g. make a sync call to the player thread).
 * @note  Memory is allocated for various metadata attributes such as album, artist etc. The consumer of these attributes
 *        is responsible for freeing the allocated memory.
 */
static int
parse_mass_item(enum pipe_metadata_msg *out_msg, struct pipe_metadata_prepared *prepared, const char *item)
{
  enum pipe_metadata_msg message;
  int ret;
  char *key, *value = NULL;
  int duration_sec = 0;
  int progress_sec = 0;

  extract_key_value(item, &key, &value);
  if (!key || !value) {
      DPRINTF(E_LOG, L_FIFO, "%s:Invalid key-value pair in Music Assistant metadata: '%s'\n", __func__, item);
      if (key) free(key);
      if (value) free(value);
      return -1;
  }

  DPRINTF(E_SPAM, L_FIFO, "%s:Parsed Music Assistant metadata key='%s' value='%s'\n", __func__, key, value);

  if (!strncmp(key,MASS_METADATA_ALBUM_KEY, strlen(MASS_METADATA_ALBUM_KEY))) {
      message = PIPE_METADATA_MSG_PARTIAL_METADATA;
      prepared->input_metadata.album = value; // The consumer must free value
      free(key);
  }
  else if (!strncmp(key,MASS_METADATA_ARTIST_KEY, strlen(MASS_METADATA_ARTIST_KEY))) {
      message = PIPE_METADATA_MSG_PARTIAL_METADATA;
      prepared->input_metadata.artist = value; // The consumer must free value
      free(key);
  }
  else if (!strncmp(key,MASS_METADATA_TITLE_KEY, strlen(MASS_METADATA_TITLE_KEY))) {
      message = PIPE_METADATA_MSG_PARTIAL_METADATA;
      prepared->input_metadata.title = value; // The consumer must free value
      free(key);
  }
  else if (!strncmp(key,MASS_METADATA_DURATION_KEY, strlen(MASS_METADATA_DURATION_KEY))) {
      message = PIPE_METADATA_MSG_PARTIAL_METADATA;
      ret = safe_atoi32(value, &duration_sec);
      if (ret < 0) {
          DPRINTF(E_LOG, L_FIFO, "%s:Invalid duration value in Music Assistant metadata: '%s'\n", __func__, value);
          free(key);
          free(value);
          return -1;
      }
      prepared->input_metadata.len_ms = duration_sec * 1000;
      free(key);
      free(value);
  }
  else if (!strncmp(key,MASS_METADATA_PROGRESS_KEY, strlen(MASS_METADATA_PROGRESS_KEY))) {
      message = PIPE_METADATA_MSG_PROGRESS;
      ret = safe_atoi32(value, &progress_sec);
      if (ret < 0) {
          DPRINTF(E_LOG, L_FIFO, "%s:Invalid progress value in Music Assistant metadata: '%s'\n", __func__, value);
          free(key);
          free(value);
          return -1;
      }
      prepared->input_metadata.pos_ms = progress_sec * 1000;
      prepared->input_metadata.pos_is_updated = true;
      DPRINTF(E_DBG, L_FIFO, "%s:Progress metadata value of %s s received and processed as %d ms.\n", 
        __func__, value, prepared->input_metadata.pos_ms
      );
      free(key);
      free(value);
  }
  else if (!strncmp(key,MASS_METADATA_ARTWORK_KEY, strlen(MASS_METADATA_ARTWORK_KEY))) {
      message = PIPE_METADATA_MSG_PARTIAL_METADATA;
      prepared->input_metadata.artwork_url = value; // The consumer must free value
      free(key);
      ret = parse_artwork_url(prepared);
      if (ret < 0) {
          DPRINTF(E_LOG, L_FIFO, "%s:Invalid artwork URL in Music Assistant metadata: '%s'\n", __func__, value);
          return -1;
      }
      message = PIPE_METADATA_MSG_PICTURE;
  }
  else if (!strncmp(key,MASS_METADATA_VOLUME_KEY, strlen(MASS_METADATA_VOLUME_KEY))) {
    message = PIPE_METADATA_MSG_VOLUME;
    ret = safe_atoi32(value, &prepared->volume);
    if (ret < 0) {
        DPRINTF(E_LOG, L_FIFO, "%s:Invalid volume value in Music Assistant metadata: '%s'\n", __func__, value);
        free(key);
        free(value);
        return -1;
    }
    free(key);
    free(value);
    DPRINTF(E_SPAM, L_FIFO, "%s:Parsed Music Assistant volume: %d\n", __func__, prepared->volume);
  }
  else if (!strncmp(key,MASS_METADATA_PIN_KEY, strlen(MASS_METADATA_PIN_KEY))) {
    message = PIPE_METADATA_MSG_PIN;
    uint32_t pin;
    ret = safe_atou32(value, &pin);
    if (ret < 0 || ret > 9999) { // PIN's limited to 4 digits
        DPRINTF(E_LOG, L_FIFO, "%s:Invalid PIN value in Music Assistant metadata: '%s'\n", __func__, value);
        free(key);
        free(value);
        return -1;
    }
    ret = asprintf(&prepared->pin, "%.4u", pin);
    free(key);
    free(value);
    DPRINTF(E_SPAM, L_FIFO, "%s:Parsed Music Assistant PIN: %.4s\n", __func__, prepared->pin);
  }
  else if (!strncmp(key,MASS_METADATA_ACTION_KEY, strlen(MASS_METADATA_ACTION_KEY))) {
      if (strncmp(value, "SENDMETA", strlen("SENDMETA")) == 0) {
         message = PIPE_METADATA_MSG_METADATA;
         free(key);
         free(value);
      }
      else if (strncmp(value, "STOP", strlen("STOP")) == 0) {
         message = PIPE_METADATA_MSG_STOP;
         free(key);
         free(value);
      }
      else if (strncmp(value, "PAUSE", strlen("PAUSE")) == 0) {
         message = PIPE_METADATA_MSG_PAUSE;
         free(key);
         free(value);
      }
      else if (strncmp(value, "PLAY", strlen("PLAY")) == 0) {
         message = PIPE_METADATA_MSG_PLAY;
         free(key);
         free(value);
      }
      else {
          DPRINTF(E_LOG, L_FIFO, "%s:Unsupported action value in Music Assistant metadata: '%s'\n", __func__, value);
          free(key);
          free(value);
          return -1;
      }
  }
  else {
      DPRINTF(E_LOG, L_FIFO, "%s:Unknown key in Music Assistant metadata: '%s=%s'\n", __func__, key, value);
      free(key);
      free(value);
      return -1;
  }
  *out_msg = message;
  return 0;
}

/** Extract one metadata/command item from the event buffer and allocate memory for it
 * @param evbuf theevent buffer to extract the item from
 * @returns     the extracted item, or NULL if unable to extract an item
 * @note  Music Assistant terminates commands/metadata items with a newline. The newline
 *        is stripped from the item upon extraction
 * @note  The consumer of the item is responsible to free the allocated memory upon consumption.
 */
static char *
extract_item(struct evbuffer *evbuf)
{
  struct evbuffer_ptr evptr;
  size_t size;
  char *item;

  evptr = evbuffer_search(evbuf, "\n", strlen("\n"), NULL);
  if (evptr.pos < 0)
    return NULL;

  size = evptr.pos + strlen("\n") + 1;
  item = malloc(size);
  if (!item)
    return NULL;

  evbuffer_remove(evbuf, item, size - 1);
  item[size - 2] = '\0'; // Replace newline with null terminator

  return item;
}

/** Parses the metadata/command content of an event buffer
 * @param out_msg   Bitmask describing all the item types found during parsing.
 *                  e.g. PIPE_METADATA_MSG_VOLUME & PIPE_METADATA_MSG_METADATA
 * @param prepared  Prepares the structure with validated metadata
 * @param evbuf     The event buffer to parse
 * @note  The prepared strucutre contains pointers to allocated memory for the metadata attributes. The end consumer
 *        of these attributes is responsible for freeing the allocated memory.
 * @note  prepared is a shared with the input thread and this function exists within a mutex lock.
 *        This means we must not do anything that could cause a deadlock (e.g. make a sync call to the player thread).
 */
static int
pipe_metadata_parse(enum pipe_metadata_msg *out_msg, struct pipe_metadata_prepared *prepared, struct evbuffer *evbuf)
{
  enum pipe_metadata_msg message;
  char *item;
  int ret;

  *out_msg = 0;
  while ((item = extract_item(evbuf))) {
    DPRINTF(E_SPAM, L_FIFO, "%s:Parsed pipe metadata item: '%s'\n", __func__, item);
    ret = parse_mass_item(&message, prepared, item);
    free(item);
    if (ret < 0) {
      DPRINTF(E_LOG, L_FIFO, "%s:parse_mass_item() failed to parse Music Assistant metadata item\n", __func__);
      return -1;
    }

      *out_msg |= message;
  }

  return 0;
}


/* ------------------------------ PIPE WATCHING ----------------------------- */
/*                             Thread: mass_aud                             */


/** Some data arrived on an audio pipe we watch. Start playback if not already playing.
 * @param fd    file descripter of the audio named pipe where data has arrived
 * @param event ?? Not used
 * @param arg   Pointer to the pipe structure
 * @note  This function runs in the mass_aud thread
 */
static void
pipe_read_cb(evutil_socket_t fd, short event, void *arg)
{
  struct pipe *pipe = arg;
  struct player_status status;
  int ret;

  DPRINTF(E_DBG, L_FIFO, "%s\n", __func__);

  // Initial read - can maybe use this to undertake any other required initialisation tasks
  if (pipe_id == 0) {
    pipe_id = pipe->id;
    DPRINTF(E_DBG, L_FIFO, "%s:Initialised global pipe_id to %d\n", __func__, pipe_id);
  }

  ret = player_get_status(&status);
  if (status.id == pipe->id) {
    DPRINTF(E_SPAM, L_FIFO, "%s:Pipe '%s' already playing with status %s\n",
      __func__, pipe->path, play_status_str(status.status)
    );
    return; // We are already playing the pipe
  }
  else if (ret < 0) {
    DPRINTF(E_LOG, L_FIFO, 
      "%s:Playback start for audio from '%s' failed because state of player is unknown\n", 
      __func__, pipe->path
    );
    return;
  }

  player_playback_stop(); // Not sure this is a good idea. Will flush data from the input buffer

  DPRINTF(E_SPAM, L_FIFO, "%s:player_playback_start_byid(%d)\n", __func__, pipe->id);
  ret = player_playback_start_byid(pipe->id);
  if (ret < 0) {
    DPRINTF(E_LOG, L_FIFO, "%s:Starting playback for data from pipe '%s' (fd %d) failed.\n", 
      __func__, pipe->path, fd
    );
    return;
  }

  /* Music Assistant looks for "restarting w/o pause" */
  DPRINTF(E_INFO, L_FIFO, "%s: restarting w/o pause\n", __func__);

}

/** Updates the pipes to be watched for pipes in the global pipe_watch_list
 * @param arg     pointer to the pipelist
 * @param retval  always updated to 0
 * @note  this function is a hangover from the OwnTone pipe.c design. It can, and should,
 *        be refactored and simplified to align with the Music Assistant integration which
 *        is restricted to a single audio named pipe and single metadata named pipe.
 * @note  This function runs in the mass_aud thread ******TO BE VALIDATED*******
 */
static enum command_state
pipe_watch_update(void *arg, int *retval)
{
  union pipe_arg *cmdarg = arg;
  struct pipe *pipelist;
  struct pipe *pipe;
  struct pipe *next;
  int count;

  if (cmdarg)
    pipelist = cmdarg->pipelist;
  else
    pipelist = NULL;

  // Removes pipes that are gone from the watchlist
  for (pipe = pipe_watch_list; pipe; pipe = next)
    {
      next = pipe->next;

      if (!pipelist_find(pipelist, pipe->id))
	{
	  DPRINTF(E_SPAM, L_FIFO, "Pipe watch deleted: '%s'\n", pipe->path);
	  watch_del(pipe);
	  pipelist_remove(&pipe_watch_list, pipe); // Will free pipe
	}
    }

  // Looks for new pipes and adds them to the watchlist
  for (pipe = pipelist, count = 0; pipe; pipe = next, count++)
    {
      next = pipe->next;

      if (count > PIPE_MAX_WATCH)
	{
	  DPRINTF(E_LOG, L_FIFO, "Max open pipes reached (%d), will not watch '%s'\n", PIPE_MAX_WATCH, pipe->path);
	  pipe_free(pipe);
	  continue;
	}

      if (!pipelist_find(pipe_watch_list, pipe->id))
	{
	  int ret = watch_add(pipe, evbase_audio_pipe);
	  DPRINTF(E_DBG, L_FIFO, "pipe_watch_update: watch_add for '%s' returned %d\n", pipe->path, ret);
	  if (ret == 0)
	    {
	      pipelist_add(&pipe_watch_list, pipe); // Changes pipe->next
	      // For stdin (no libevent watch), start playback immediately
	      // For named pipes, manually trigger the read callback to check for already-buffered data
	      if (pipe->ev)
	        {
	          DPRINTF(E_DBG, L_FIFO, "pipe_watch_update: Manually triggering pipe_read_cb for '%s'\n", pipe->path);
	          event_active(pipe->ev, EV_READ, 0);
	        }
	      else if (is_stdin(pipe->path))
	        {
	          // For stdin, start playback immediately - no libevent watch needed
	          DPRINTF(E_INFO, L_FIFO, "pipe_watch_update: Starting playback for stdin\n");
	          pipe_id = pipe->id;
	          player_playback_stop();
	          ret = player_playback_start_byid(pipe->id);
	          if (ret < 0)
	            DPRINTF(E_LOG, L_FIFO, "pipe_watch_update: Failed to start playback for stdin\n");
	          else
	            DPRINTF(E_INFO, L_FIFO, "restarting w/o pause\n");
	        }
	    }
	  else
	    DPRINTF(E_LOG, L_FIFO, "pipe_watch_update: Failed to watch pipe '%s'\n", pipe->path);
	}
      else
	{
	  pipe_free(pipe);
	}
    }

  *retval = 0;
  return COMMAND_END;
}

/** 
 * Sets the thread name and launches the event loop for processing input audio data.
 * Exits the thread upon break from the event loop.
 */
static void *
mass_audio_thread_run(void *arg)
{
  thread_setname("mass_aud");
  event_base_dispatch(evbase_audio_pipe);

  pthread_exit(NULL);
}

/* ----------------------- PIPE WATCH THREAD START/STOP --------------------- */
/*                             Thread: mass_aud                            */

/**
 * Establish event and commands base for the mass_aud thread and then create the thread.
 * @note On macOS, kqueue has known issues with FIFOs (named pipes) - it may not properly
 *       detect when data is written from another process. We use event_config to avoid
 *       kqueue for this event base on macOS.
 */
static void
pipe_thread_start(void)
{
  DPRINTF(E_DBG, L_FIFO, "%s\n", __func__);

#ifdef __APPLE__
  // On macOS, avoid kqueue for FIFO monitoring - it has issues detecting writes from other processes
  struct event_config *cfg;
  cfg = event_config_new();
  if (cfg)
    {
      event_config_avoid_method(cfg, "kqueue");
      evbase_audio_pipe = event_base_new_with_config(cfg);
      event_config_free(cfg);
    }
  else
    {
      evbase_audio_pipe = event_base_new();
    }
  CHECK_NULL(L_FIFO, evbase_audio_pipe);
  DPRINTF(E_DBG, L_FIFO, "%s: Using event method: %s\n", __func__, event_base_get_method(evbase_audio_pipe));
#else
  CHECK_NULL(L_FIFO, evbase_audio_pipe = event_base_new());
#endif
  CHECK_NULL(L_FIFO, cmdbase = commands_base_new(evbase_audio_pipe, NULL));
  CHECK_ERR(L_FIFO, pthread_create(&tid_audio_pipe, NULL, mass_audio_thread_run, NULL));

}

/**
 * Stop the mass_aud thread.
 * @details Removes the pipe watch for the audio named pipe.
 * Destroys the command base.
 * Joins the mass_aud thread and frees the streamed audio event base.
 */
static void
pipe_thread_stop(void)
{
  int ret;

  DPRINTF(E_DBG, L_FIFO, "%s\n", __func__);

  if (!tid_audio_pipe)
    return;

  commands_exec_sync(cmdbase, pipe_watch_update, NULL, NULL);
  commands_base_destroy(cmdbase);

  ret = pthread_join(tid_audio_pipe, NULL);
  if (ret != 0)
    DPRINTF(E_LOG, L_FIFO, "Could not join pipe thread: %s\n", strerror(errno));

  event_base_free(evbase_audio_pipe);
  tid_audio_pipe = 0;
}

/**
 * Create the pipelist of streamed audio named pipes to subsequently watch
 * @returns pointer to the head of the created pipelist
 * @note  For Music Assistant, the pipelist created will always contain one entry for the
 *        named pipe which will receive streamed PCM audio.
 * @note  The function is a hangover from the OwnTone pipe.c design. It should be reviewed to
 *        determine if the construct of a pipelist can be eliminated from this module to simplify
 *        the implementation.
 */
static struct pipe *
pipelist_create(void)
{
  struct pipe *head;
  struct pipe *pipe;

  DPRINTF(E_DBG, L_FIFO, "%s:Adding %s to the pipelist\n", __func__, mass_named_pipes.audio_pipe);
  head = NULL;
  pipe = pipe_create(mass_named_pipes.audio_pipe, 1, PIPE_PCM, pipe_read_cb);
  pipelist_add(&head, pipe);

  return head;
}

/**
 * Listener callback function. Creates our pipelist to watch, starts the mass_aud thread and 
 * asynchronously calls pipe_watch_update with the newly created pipelist.
 * @param event_mask  Event mask not used within this function
 * @param ctx         Context not used within this function
 */
static void
pipe_listener_cb(short event_mask, void *ctx)
{
  union pipe_arg *cmdarg;

  cmdarg = malloc(sizeof(union pipe_arg));
  if (!cmdarg)
    return;

  cmdarg->pipelist = pipelist_create();
  if (!cmdarg->pipelist)
    {
      DPRINTF(E_INFO, L_FIFO, "%s: No pipelist. Stopping thread.\n", __func__);
      pipe_thread_stop();
      free(cmdarg);
      return;
    }

  if (!tid_audio_pipe)
    pipe_thread_start();
  
  commands_exec_async(cmdbase, pipe_watch_update, cmdarg);
  
}

/* ------------------- Metadata and Command Processing --------------------------------*/
/*                      Thread: mass_cmd                                           */

/**
 * Callback function to report player status to Music Assistant
 * @param fd    File descriptor not used
 * @param what  Not used
 * @param arg   Not used
 */
static void
mass_timer_cb(int fd, short what, void *arg)
{
  struct timespec now;
  uint64_t elapsed_ms = 0;
  uint64_t begin_ms, now_ms = 0;
  struct player_status status;
  int ret;

  ret = player_get_status(&status);
  if (ret < 0) {
    DPRINTF(E_LOG, L_FIFO, "%s:Could not get player status\n", __func__);
    return;
  }

  DPRINTF(E_SPAM, L_FIFO,
    "%s: player status:%s, volume:%d, pos_ms:%" PRIu32 "\n", 
    __func__, play_status_str(status.status), status.volume, status.pos_ms
  );

  if (status.status == PLAY_PLAYING) {
    if (!player_started) {
      player_started = true;
    }
    DPRINTF(E_DBG, L_FIFO, 
      "%s: volume:%d state:%s, position:%" PRIu32 " ms. \n",
      __func__, status.volume, play_status_str(status.status), status.pos_ms
    );
  }
  else if (player_started && status.status == PLAY_PAUSED) {
    if (!player_paused) {
      player_paused = true;
      clock_gettime(CLOCK_REALTIME, &paused_start_ts); // reset paused time
      /* Music Assistant looks for "set pause" or "Pause at" */
      DPRINTF(E_INFO, L_FIFO, "%s: Pause at %" PRIu32 " ms\n",
        __func__, status.pos_ms
      );
    }
    else {
      clock_gettime(CLOCK_REALTIME, &now);
      begin_ms = (uint64_t)paused_start_ts.tv_sec * 1000 + (uint64_t)(paused_start_ts.tv_nsec / 1000000);
      now_ms   = (uint64_t)now.tv_sec * 1000 + (uint64_t)(now.tv_nsec / 1000000);
      elapsed_ms = now_ms - begin_ms;
      DPRINTF(E_SPAM, L_FIFO, 
        "%s: paused milliseconds:%" PRIu64 " ms at position %" PRIu32 "\n", 
        __func__, elapsed_ms, status.pos_ms
      );

    }
  }
  else if (player_started && status.status == PLAY_STOPPED) {
    DPRINTF(E_DBG, L_FIFO, "%s:Time to exit gracefully\n", __func__);
    exit(0);
  }
  else { // this state can happen when audio has not yet been received on the named pipe
    DPRINTF(E_DBG, L_FIFO, "%s:Player %sstarted. status:%s\n", __func__,
      player_started ? "" : "not ", play_status_str(status.status)
    );
    // reset all
    player_started = false;
    player_paused = false;
    elapsed_ms = 0;
  }
}

/**
 * Sets the stop flag
 * @note  This function runs in the mass_cmd thread and shares the stop flag with the
 *        mass_aud thread. It therefore updates the flag within a mutex lock.
 */
static void
self_stop(void)
{
  pthread_mutex_lock(&audio_command_lock);
  stop_flag = true;
  pthread_mutex_unlock(&audio_command_lock);
}

/**
 * Sets the pause flag
 * @note  This function runs in the mass_cmd thread and shares the pause flag with the
 *        mass_aud thread. It therefore updates the flag within a mutex lock.
 */
static void
self_pause(void)
{
  pthread_mutex_lock(&audio_command_lock);
  pause_flag = true;
  pthread_mutex_unlock(&audio_command_lock);
}

/**
 * Unsets the pause flag
 * @note  This function runs in the mass_cmd thread and shares the pause flag with the
 *        mass_aud thread. It therefore updates the flag within a mutex lock.
 */
static void
self_resume(void)
{
  pthread_mutex_lock(&audio_command_lock);
  pause_flag = false;
  pthread_mutex_unlock(&audio_command_lock);
}

/**
 * Deletes the metadata/command pipe
 * @param arg Not used
 * @note  This function deletes the metadata/command read event, frees
 *        memory allocated for the pipe data structures, closes and deletes
 *        the tmpfile used for artwork.
 */
static void
pipe_metadata_watch_del(void *arg)
{
  if (!pipe_metadata.pipe)
    return;

  if (pipe_metadata.evbuf) evbuffer_free(pipe_metadata.evbuf);
  watch_del(pipe_metadata.pipe);
  pipe_free(pipe_metadata.pipe);
  pipe_metadata.pipe = NULL;

  pict_tmpfile_close(pipe_metadata.prepared.pict_tmpfile_fd, pipe_metadata.prepared.pict_tmpfile_path);
  pipe_metadata.prepared.pict_tmpfile_fd = -1;
}

/**
 * Sends the PIN to complete pairing
 */
static void
mass_speaker_authorize(void)
{
  struct player_speaker_info spk;

  DPRINTF(E_DBG, L_FIFO, "%s:Calling player_speaker_get_byindex(&spk, 0)\n", __func__);
  player_speaker_get_byindex(&spk, 0); // We only ever have one speaker for Music Assistant
  DPRINTF(E_DBG, L_FIFO, "%s:speaker name:%s, index:%" PRIu32 ", id:%" PRIu64 ", output_type:%s, requires_auth:%s, formats:0x%0x\n", 
    __func__, spk.name, spk.index, spk.id, spk.output_type, spk.requires_auth ? "yes" : "no",
    spk.supported_formats
  );
  
  if (!spk.requires_auth) {
    return;
  }
  player_speaker_authorize(spk.id, ap_device_info.pin);
}

/**
 * Read and process command/metadata from the named pipe
 * @param fd    Not used
 * @param event Not used
 * @param arg   Not used
 */
static void
pipe_metadata_read_cb(evutil_socket_t fd, short event, void *arg)
{
  enum pipe_metadata_msg message;
  size_t len;
  struct player_status status;
  int ret;

  ret = evbuffer_read(pipe_metadata.evbuf, pipe_metadata.pipe->fd, PIPE_READ_MAX);
  if (ret < 0)
    {
      if (errno != EAGAIN)
	pipe_metadata_watch_del(NULL);
      return;
    }
  else if (ret == 0)
    {
      // Reset the pipe
      ret = watch_reset_metadata(pipe_metadata.pipe);
      if (ret < 0)
	return;
      goto readd;
    }

  len = evbuffer_get_length(pipe_metadata.evbuf);
  if (len > PIPE_METADATA_BUFLEN_MAX)
    {
      DPRINTF(E_LOG, L_FIFO, "Buffer for command pipe '%s' is full, discarding %zu bytes\n", pipe_metadata.pipe->path, len);
      evbuffer_drain(pipe_metadata.evbuf, len);
      goto readd;
    }
  
  DPRINTF(E_SPAM, L_FIFO, "%s:Received %zu bytes of metadata\n", __func__, len);

  // .parsed is shared with the input thread (see metadata_get), so use mutex.
  // Note that this means _parse() must not do anything that could cause a
  // deadlock (e.g. make a sync call to the player thread).
  pthread_mutex_lock(&pipe_metadata.prepared.lock);
  ret = pipe_metadata_parse(&message, &pipe_metadata.prepared, pipe_metadata.evbuf);
  pthread_mutex_unlock(&pipe_metadata.prepared.lock);
  if (ret < 0) {
    DPRINTF(E_LOG, L_FIFO, "Error parsing incoming data on command pipe '%s', will stop reading\n", pipe_metadata.pipe->path);
    pipe_metadata_watch_del(NULL);
    return;
  }

  DPRINTF(E_SPAM, L_FIFO, "%s:Parsed command pipe message mask: 0x%x\n", __func__, message);

  ret = player_get_status(&status);
  if (ret != COMMAND_END) {
    DPRINTF(E_LOG, L_FIFO, "%s: Unable to obtain player status\n", __func__);
  }
  if (message & (PIPE_METADATA_MSG_METADATA | PIPE_METADATA_MSG_PICTURE)) {
    pipe_metadata.is_new = 1; // Trigger notification to player in playback loop
    DPRINTF(E_SPAM, L_FIFO, 
      "%s:Triggered notification to player in the playback loop of new metadata available (message=0x%x)\n", 
      __func__, message
    );
  }
  if (message & PIPE_METADATA_MSG_VOLUME) {
    DPRINTF(E_SPAM, L_FIFO, "%s:Setting volume from command pipe to %d\n", __func__, pipe_metadata.prepared.volume);
    player_volume_set(pipe_metadata.prepared.volume);
  }
  if (message & PIPE_METADATA_MSG_PIN) {
    DPRINTF(E_DBG, L_FIFO, "%s:Setting PIN from command pipe to %s\n", __func__, pipe_metadata.prepared.pin);
    strncpy(ap_device_info.pin, pipe_metadata.prepared.pin, sizeof(ap_device_info.pin) - 1);
    mass_speaker_authorize();
    free(pipe_metadata.prepared.pin);

  }
  if (message & PIPE_METADATA_MSG_FLUSH) {
    DPRINTF(E_DBG, L_FIFO, 
      "%s:FLUSH:Flushing playback from command pipe. Current player status is %s\n", 
      __func__, play_status_str(status.status)
    );
    player_playback_flush(); // results in FLUSH to the airplay device
  }
  if (message & PIPE_METADATA_MSG_PAUSE) {
    DPRINTF(E_DBG, L_FIFO, 
      "%s:PAUSE:Pausing playback from command pipe. Current player status is %s, %" PRIu32 "ms\n",
      __func__, play_status_str(status.status), status.pos_ms
    );
    // We check the current state before confirming what input action to undertake (if any)
    if (status.status == PLAY_PLAYING) {
      self_pause();
      // Report status to Music Assistant
      DPRINTF(E_INFO, L_FIFO, "%s:Pause at %" PRIu32 "\n", __func__, status.pos_ms);
    }
    else {
      DPRINTF(E_WARN, L_FIFO, "%s:Command received to PAUSE playback, but current state is %s. Ignoring command.\n",
        __func__, play_status_str(status.status)
      );
    }
  }
  if (message & PIPE_METADATA_MSG_PLAY) {
    DPRINTF(E_DBG, L_FIFO, 
      "%s:PLAY:(Re)starting playback from command pipe. Current player status is %s, %" PRIu32 "ms\n",
      __func__, play_status_str(status.status), status.pos_ms
    );
    if (status.status != PLAY_PLAYING) {
      self_resume();
      // Report status to Music Assistant
      DPRINTF(E_INFO, L_FIFO, "%s:Restarted at %" PRIu32 "\n", __func__, status.pos_ms);
    }
    else {
      DPRINTF(E_WARN, L_FIFO, "%s:Command received to PLAY, but current state is %s. Ignoring command.\n",
        __func__, play_status_str(status.status)
      );
    }
  }
  if (message & PIPE_METADATA_MSG_STOP) {
    DPRINTF(E_DBG, L_FIFO, "%s:STOP:Stopping playback from command pipe command\n", __func__);
    self_stop();
    input_flush(NULL); // we don't care about losing data for the input_buffer on stop.
    // Report status to Music Assistant
    DPRINTF(E_INFO, L_FIFO, "%s:Stop at %" PRIu32 "\n", __func__, status.pos_ms);
  }

 readd:
  if (pipe_metadata.pipe && pipe_metadata.pipe->ev) {
    DPRINTF(E_SPAM, L_FIFO, "%s:Re-adding event for command pipe '%s'\n", __func__, pipe_metadata.pipe->path);
    event_add(pipe_metadata.pipe->ev, NULL);
  }
  else {
    DPRINTF(E_DBG, L_FIFO, "%s:command pipe '%s' no longer valid, not re-adding event\n",
      __func__, pipe_metadata.pipe->path
    );
  }
}


/**
 * Add a watch event to the command/metadata named pipe and set the callback
 * @param path  pathname for the command/metadata named pipe
 * @returns 0 on success, -1 on failure
 */
static int
pipe_metadata_watch_add(const char *path)
{
  int ret;

  pipe_metadata_watch_del(NULL); // Just in case we somehow already have a metadata pipe open

  pipe_metadata.pipe = pipe_create(path, 0, PIPE_METADATA, pipe_metadata_read_cb);
  pipe_metadata.evbuf = evbuffer_new();

  ret = watch_add(pipe_metadata.pipe, evbase_command_pipe);
  if (ret < 0) {
    evbuffer_free(pipe_metadata.evbuf);
    pipe_free(pipe_metadata.pipe);
    pipe_metadata.pipe = NULL;
    return ret;
  }
  
  return 0;
}

/**
 * Spawn the mass_cmd thread, setup the command/metadata events and dispatch the 
 * command pipe event loop
 * @param arg Not used. Can be NULL
 * @note This function does not return unless the command pipe event loop is broken.
 */
static void *
mass_command_thread_run(void *arg)
{
  char my_thread[32];

  thread_setname("mass_cmd");
  thread_getnametid(my_thread, sizeof(my_thread));
  pipe_metadata_watch_add(mass_named_pipes.metadata_pipe);
  // Create a persistent event timer to monitor and report playback status for logging and debugging purposes
  mass_timer_event = event_new(evbase_command_pipe, -1, EV_PERSIST | EV_TIMEOUT, mass_timer_cb, NULL);
  evtimer_add(mass_timer_event, &mass_tv);
  DPRINTF(E_DBG, L_FIFO, "%s:About to launch command pipe event loop in thread %s\n", __func__, my_thread);
  event_base_dispatch(evbase_command_pipe);

  pthread_exit(NULL);
}

/**
 * Stops the mass_cmd thread and ensures housekeeping is performed.
 */
static void
command_pipe_thread_stop(void)
{
  // int ret; 

  if (!tid_command_pipe)
    return;

  pipe_metadata_watch_del(NULL);

  event_base_loopbreak(evbase_command_pipe);
  event_free(mass_timer_event);
  event_base_free(evbase_command_pipe);
  tid_command_pipe = 0;
}


/* --------------------------- PIPE INPUT INTERFACE ------------------------- */
/*                                Thread: input                               */

/**
 * Input definition callback function to setup the mass (Music Assistant) module.
 * Called by the input module.
 * 
 * @note
 * We read raw audio data either from stdin or from a named pipe. If the raw audio data
 * does not need demuxing, then it is passed directly to the input module as is. 24-bit
 * quality requires demuxing to get the data into the correct sample format of s32.
 * 
 * @param source  Input source to be setup
 * @returns 0 on success, -1 on failure
 */
static int
setup(struct input_source *source)
{
  struct mass_ctx *ctx;
  int fd;
#if DEMUX_TRANSCODE_DECODE | DEMUX_TRANSCODE
    struct transcode_decode_setup_args decode_args = {};
#endif
#if DEMUX_TRANSCODE
    struct transcode_encode_setup_args encode_args = {};
#endif

  CHECK_NULL(L_FIFO, ctx = calloc(1, sizeof(struct mass_ctx)));

  fd = pipe_open(source->path, 0);
  if (fd < 0) {
    return -1;
  }
  ctx->pipe = pipe_create(source->path, source->id, PIPE_PCM, NULL);
  ctx->pipe->fd = fd;

  CHECK_NULL(L_FIFO, source->evbuf = evbuffer_new());

  if (demux_required(&ap_device_info.quality)) {
    // create an evbuffer for raw audio input to be read into
    CHECK_NULL(L_FIFO, ctx->evbuf = evbuffer_new());
#if DEMUX_TRANSCODE_DECODE | DEMUX_TRANSCODE
    decode_args.quality = &ap_device_info.quality;
    decode_args.profile = quality_to_xcode(&ap_device_info.quality);
    CHECK_NULL(L_FIFO, decode_args.evbuf_io = calloc(1, sizeof(struct transcode_evbuf_io)));
    decode_args.evbuf_io->evbuf = ctx->evbuf;
    decode_args.evbuf_io->seekfn = NULL;
#endif
#if DEMUX_TRANSCODE_DECODE
    ctx->decode_ctx = transcode_decode_setup(decode_args);
    if (!ctx->decode_ctx) {
      free(decode_args.evbuf_io);
      return -1;
    }
#endif
#if DEMUX_TRANSCODE
    encode_args.quality = &ap_device_info.quality;
    encode_args.profile = quality_to_xcode(&ap_device_info.quality);
    ctx->transcode_ctx = transcode_setup(decode_args, encode_args);
    if (!ctx->transcode_ctx) {
      free(decode_args.evbuf_io);
      return -1;
    }
#endif
#if DEMUX_TRANSCODE_DECODE | DEMUX_TRANSCODE
    free(decode_args.evbuf_io);
#endif
  } else {
    // We point the mass_ctx evbuffer directly to the source evbuffer
    ctx->evbuf = source->evbuf;
  }

  source->input_ctx = ctx;
  DPRINTF(E_DBG, L_FIFO, "%s:source->evbuf:%p, source->input_ctx->evbuf:%p\n", __func__, source->evbuf, ctx->evbuf);

  // In the Music Assistant use case, we stream input at the same quality as we want playback
  // therefore, our input quality will be the same as our output quality
  // NOTE: If transcode() can be made to work, then it opens up possibility of accepting different
  // quality and/or codec as raw input stream - but the utility of this is questionable.
  source->quality = ap_device_info.quality;

  return 0;
}

/**
 * Input definition callback function called when input is stopped.
 * @param source  Input source to stop
 * @returns 0
 */
static int
stop(struct input_source *source)
{
  struct mass_ctx *ctx = source->input_ctx;

  if (demux_required(&source->quality)) {
#if DEMUX_TRANSCODE_DECODE
    transcode_decode_cleanup(&ctx->decode_ctx);
#elif DEMUX_TRANSCODE
    transcode_cleanup(&ctx->transcode_ctx);
#endif
    evbuffer_free(ctx->evbuf);
  }

  if (source->evbuf) {
    evbuffer_free(source->evbuf);
  }

  if (ctx) {
    free(ctx);
  }

  source->input_ctx = NULL;
  source->evbuf = NULL;

  return 0;
}

/**
 * Input definition callback function triggered on each iteration of the playback loop
 * @param source  The input source to obtain audio data for
 * @returns 0 on success, -1 on failure
 * @note  We check (inside a mutex lock) if the player is paused, and if not, then we 
 *        transcode the raw pcm audio input and pass it to the input module via the source->evbuffer.
 *        Whilst 16-bit and 32-bit raw pcm audio input does not need to be transcoded, it is necessary
 *        to demux 24-bit raw pcm audio input in order to get the data into 32-bit sample format
 *        a.k.a 24-in-32.
 *        If the player is paused or there is no data to read, we wait for a period by 
 *        calling input_wait() and return.
 * 
 */
static int
play(struct input_source *source)
{
  struct mass_ctx *ctx = source->input_ctx;
  short flags;
  int ret;
  static size_t read_count = 0;
  bool demux_flag = demux_required(&source->quality);
#if DEBUG_MASS
  static size_t bytes_read = 0;
#endif

  pthread_mutex_lock(&audio_command_lock);
  if (pause_flag) { // PAUSE command received
    pthread_mutex_unlock(&audio_command_lock);
    input_wait();
    return 0; // loop
  }
  if (stop_flag) { // STOP command received
    input_write(source->evbuf, NULL, INPUT_FLAG_EOF);
    stop(source);
    pthread_mutex_unlock(&audio_command_lock);
    DPRINTF(E_INFO, L_FIFO, "%s:STOP command initiated shutdown\n", __func__);
    return -1;
  }
  pthread_mutex_unlock(&audio_command_lock);

  ret = evbuffer_read(ctx->evbuf, ctx->pipe->fd, PIPE_READ_MAX);
  if (demux_flag && ret > 0) {
    // We have raw audio data that requires demuxing
#if DEMUX_LOCAL
    int demuxed_bytes = demux_to_24_in_32(source->evbuf, ctx->evbuf);
    if (demuxed_bytes < 0) {
      DPRINTF(E_WARN, L_FIFO, "%s:Error demuxing. Playback will not work.\n", __func__);
      ret = demuxed_bytes;
    }
#elif DEMUX_TRANSCODE_DECODE
    transcode_frame *frame;
    int decode_ret;
    while ((decode_ret = transcode_decode(&frame, ctx->decode_ctx)) != 0) {
      if (decode_ret < 0) {
        DPRINTF(E_LOG, L_FIFO, "%s:Error decoding raw audio input data.\n", __func__);
        input_write(NULL, NULL, INPUT_FLAG_ERROR);
        stop(source);
        return -1;
      }
      DPRINTF(E_DBG, L_FIFO, "%s:transcode_decode() returned %d. sizeof(frame):%zu\n", __func__, decode_ret, sizeof(frame));
      evbuffer_add(source->evbuf, frame, decode_ret);
    }
#elif DEMUX_TRANSCODE
    int want_bytes = ret * 32 / source->quality.bits_per_sample;
    int transcode_ret = transcode(source->evbuf, NULL, ctx->transcode_ctx, want_bytes);
    DPRINTF(E_DBG, L_FIFO, "%s:%d raw bytes, wanted %d transcoded bytes, transcoded %d bytes\n", __func__, ret, want_bytes, transcode_ret);
    if (transcode_ret > 0 && transcode_ret != want_bytes) {
      DPRINTF(E_WARN, L_FIFO, "%s:Unexpected transcoding mismatch. From %d raw bytes, expected to transcode %d bytes, but actually decoded %d bytes.\n",
        __func__, ret, want_bytes,transcode_ret
      );
    }
    ret = transcode_ret;
#endif
  }
  if (ret == 0) {
    input_write(source->evbuf, NULL, INPUT_FLAG_EOF); // Autostop
    stop(source);
    DPRINTF(E_INFO, L_FIFO, "%s:end of stream reached\n", __func__);
    return -1;
  }
  else if ((ret < 0) && ((errno == EAGAIN) || (errno == EWOULDBLOCK))) {
    input_wait();
    return 0; // Loop
  }
  else if (ret < 0) {
    DPRINTF(E_LOG, L_FIFO, "Could not read from pipe '%s': %s\n", source->path, strerror(errno));
    input_write(NULL, NULL, INPUT_FLAG_ERROR);
    stop(source);
    return -1;
  }

  // Update Music Assistant that playback is commencing
  if (read_count == 0) {
    DPRINTF(E_INFO, L_FIFO, "%s:Starting at 0ms\n", __func__);
  }

  read_count++;
#if DEBUG_MASS
  bytes_read += ret;
#endif

  flags = (pipe_metadata.is_new ? INPUT_FLAG_METADATA : 0);
  pipe_metadata.is_new = 0;
  if (read_count == 1 && ap_device_info.start_ts.tv_sec != 0) {
    // We want to control the time of playback of the first audio packet
    flags |= INPUT_FLAG_SYNC;
  }

#if DEBUG_MASS
  DPRINTF(E_DBG, L_FIFO, "%s:chunk_size:%d read_count:%zu total readbytes:%zu to input\n", __func__, ret, read_count, bytes_read);
#endif
  input_write(source->evbuf, &source->quality, flags);

  return 0;
}

/**
 * Input definition callback function to obtain metadata
 * @param metadata  Pointer to the metadata structure to return the metadata in
 * @param source    The input source to obtain metadata for
 * @returns 0
 * @note  Transfers the prepared metadata to the caller (input module) inside a mutex lock
 *        and nulls the prepared metadata structure (including pointers) so the next set of 
 *        metadata can be prepared from the metadata/command named pipe in due course.
 */
static int
metadata_get(struct input_metadata *metadata, struct input_source *source)
{
  pthread_mutex_lock(&pipe_metadata.prepared.lock);

  *metadata = pipe_metadata.prepared.input_metadata;

  // Ownership transferred to caller, null all pointers in the struct
  memset(&pipe_metadata.prepared.input_metadata, 0, sizeof(struct input_metadata));

  pthread_mutex_unlock(&pipe_metadata.prepared.lock);

  return 0;
}

/**
 * Input definition callback function to obtain timespec for playback of first audio packet
 * in the data chunk passed to input
 * @param metadata  Pointer to the timespec structure
 * @param source    The input source to obtain timespec for
 * @returns 0
 */
static int
ts_get(struct timespec *ts, struct input_source *source)
{
  *ts = ap_device_info.start_ts;
  return 0;
}

/* ---------------------------------------------------------------------------------------*/
/*                                   Thread: main                                         */

/**
 * Initialise the metadata/command pipe module and spawn the mass_cmd thread
 * @returns 0 on success, -1 on failure
 */
static int
command_pipe_init(void)
{
  int ret;

  evbase_command_pipe = event_base_new();
  ret = pthread_create(&tid_command_pipe, NULL, mass_command_thread_run, NULL);
  if (ret !=0) {
    DPRINTF(E_LOG, L_FIFO, "%s:Unable to create command thread. %s\n", __func__, strerror(errno));
    return -1;
  }
  return 0;
}

/**
 * De-initialises the metadata/command pipe module by stopping the mass_cmd
 * thread.
 */
static void
command_pipe_deinit()
{
  DPRINTF(E_DBG, L_FIFO, "%s\n", __func__);
  command_pipe_thread_stop();
}

/**
 * Initialise the mass (Music Assistant) module.
 * @returns 0 on success, -1 on failure
 * @note  Initialisation establishes the required mutexes, audio quality data, 
 *        spawns the mass_aud and mass_cmd threads.
 * @todo  Expand the range of audio qualities supported and eliminate it from configuration
 */
int
mass_init(void)
{
  CHECK_ERR(L_FIFO, mutex_init(&pipe_metadata.prepared.lock));
  CHECK_ERR(L_FIFO, mutex_init(&audio_command_lock));

  pipe_metadata.prepared.pict_tmpfile_fd = -1;

  pipe_listener_cb(0, NULL); // We will be in the pipe thread once this returns
  CHECK_ERR(L_FIFO, listener_add(pipe_listener_cb, LISTENER_DATABASE, NULL));
  
  command_pipe_init();

  return 0;
}

/**
 * De-initialise the mass (Music Assistant) module.
 */
void
mass_deinit(void)
{
  DPRINTF(E_DBG, L_FIFO, "%s\n", __func__);
  command_pipe_deinit();

  listener_remove(pipe_listener_cb);
  pipe_thread_stop();

  CHECK_ERR(L_FIFO, pthread_mutex_destroy(&pipe_metadata.prepared.lock));
  CHECK_ERR(L_FIFO, pthread_mutex_destroy(&audio_command_lock));
}

/**
 * Input definition of callback functions
 */
struct input_definition input_pipe =
{
  .name = "mass",
  .type = INPUT_TYPE_PIPE,
  .disabled = 0,
  .setup = setup,
  .play = play,
  .stop = stop,
  .metadata_get = metadata_get,
  .ts_get = ts_get,
  .init = mass_init,
  .deinit = mass_deinit,
};
