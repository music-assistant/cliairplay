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

#include "artwork.h"
#include "cliap2.h"
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

/* from cliap2.c */
extern ap2_device_info_t ap2_device_info;
extern mass_named_pipes_t mass_named_pipes;

 /* mass specific stuff */
static struct event *mass_timer_event = NULL;
static struct timeval mass_tv = { MASS_UPDATE_INTERVAL_SEC, 0};
// static struct timespec playback_start_ts = {0, 0};
static struct timespec paused_start_ts = {0, 0};
static bool player_started = false;
static bool player_paused = false;
static int pipe_id = 0; // make a global of the id of our audio named pipe
static pthread_mutex_t pause_lock;
static bool pause_flag = false; // we control when to pause and (re)commence reading from the audio pipe

// Maximum number of pipes to watch for data
#define PIPE_MAX_WATCH 4
// Max number of bytes to read from the audio pipe at a time
#define PIPE_READ_MAX 65536
// Max number of bytes to buffer from the command/metadata pipe
#define PIPE_METADATA_BUFLEN_MAX 1048576
// Ignore pictures with larger size than this
#define PIPE_PICTURE_SIZE_MAX 1048576
// Where we store pictures for the artwork module to read
#define PIPE_TMPFILE_TEMPLATE "/tmp/" PACKAGE_NAME ".XXXXXX.ext"
#define PIPE_TMPFILE_TEMPLATE_EXTLEN 4

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

// From config - the sample rate and bps of the pipe input
static int pipe_sample_rate;
static int pipe_bits_per_sample;

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

  CHECK_NULL(L_PLAYER, pipe = calloc(1, sizeof(struct pipe)));
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

/** Opens a file for reading and validates it is a FIFO pipe
 * @param path    the pathname of the file to open
 * @param silent  suppress fstat error if it occurs
 * @returns the file descriptor on success, -1 on failure
 */
static int
pipe_open(const char *path, bool silent)
{
  struct stat sb;
  int fd;

  DPRINTF(E_SPAM, L_PLAYER, "(Re)opening pipe: '%s'\n", path);

  fd = open(path, O_RDONLY | O_NONBLOCK);
  if (fd < 0) {
    DPRINTF(E_LOG, L_PLAYER, "Could not open pipe for reading '%s': %s\n", path, strerror(errno));
    goto error;
  }

  if (fstat(fd, &sb) < 0) {
    if (!silent)
      DPRINTF(E_LOG, L_PLAYER, "Could not fstat() '%s': %s\n", path, strerror(errno));
    goto error;
  }

  if (!S_ISFIFO(sb.st_mode)) {
    DPRINTF(E_LOG, L_PLAYER, "Source type is pipe, but path is not a fifo: %s\n", path);
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
 */
static void
pipe_close(int fd)
{
  if (fd >= 0)
    close(fd);
}

/** Add a libevent read event for the pipe
 * @param pipe    the pipe to watch for data to read
 * @param evbase  the event base to add the event to
 * @returns 0 on success, -1 on failure
 */
static int
watch_add(struct pipe *pipe, struct event_base *evbase)
{
  pipe->fd = pipe_open(pipe->path, 0);
  if (pipe->fd < 0)
    return -1;

  pipe->ev = event_new(evbase, pipe->fd, EV_READ, pipe->cb, pipe);
  if (!pipe->ev)
    {
      DPRINTF(E_LOG, L_PLAYER, "Could not watch pipe for new data '%s'\n", pipe->path);
      pipe_close(pipe->fd);
      return -1;
    }

  event_add(pipe->ev, NULL);

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
      DPRINTF(E_LOG, L_PLAYER, "Invalid extension provided to pict_tmpfile_recreate: '%s'\n", ext);
      return -1;
    }

  if (path_size < sizeof(PIPE_TMPFILE_TEMPLATE))
    {
      DPRINTF(E_LOG, L_PLAYER, "Invalid path buffer provided to pict_tmpfile_recreate\n");
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

  CHECK_NULL(L_PLAYER, raw = evbuffer_new());

  format = artwork_read_byurl(raw, m->artwork_url);
  if (format <= 0) {
    DPRINTF(E_LOG, L_PLAYER, "Could not read artwork from URL '%s'\n", m->artwork_url);
    goto error;
  }

  artwork_image_size = evbuffer_get_length(raw);
  artwork_image = malloc(artwork_image_size);
  if (!artwork_image) {
    DPRINTF(E_LOG, L_PLAYER, "Could not allocate memory for artwork from URL '%s'\n", m->artwork_url);
    goto error;
  }
  ret = evbuffer_remove(raw, artwork_image, artwork_image_size);
  if (ret < 0 || (size_t)ret != artwork_image_size) {
    DPRINTF(E_LOG, L_PLAYER, "Could not extract artwork from evbuffer for URL '%s'\n", m->artwork_url);
    goto error;
  }
  evbuffer_free(raw);
  raw = NULL;

  if (format == ART_FMT_JPEG)
    ext = ".jpg";
  else if (format == ART_FMT_PNG)
    ext = ".png";
  else {
    DPRINTF(E_LOG, L_PLAYER, "Unsupported picture format from artwork URL '%s'\n", m->artwork_url);
    goto error;
  }

  free(m->artwork_url);
  m->artwork_url = NULL;

  prepared->pict_tmpfile_fd = pict_tmpfile_recreate(
    prepared->pict_tmpfile_path, sizeof(prepared->pict_tmpfile_path), prepared->pict_tmpfile_fd, ext
  );
  if (prepared->pict_tmpfile_fd < 0) {
    DPRINTF(E_LOG, L_PLAYER, "Could not open tmpfile for pipe artwork '%s': %s\n",
      prepared->pict_tmpfile_path, strerror(errno)
    );
    goto error;
  }

  ret = write(prepared->pict_tmpfile_fd, artwork_image, artwork_image_size);
  if (ret < 0) {
    DPRINTF(E_LOG, L_PLAYER, "Error writing artwork from metadata pipe to '%s': %s\n",
      prepared->pict_tmpfile_path, strerror(errno)
    );
    goto error;
  }
  else if (ret != artwork_image_size) {
    DPRINTF(E_LOG, L_PLAYER, "Incomplete write of artwork to '%s' (%zd/%ld)\n",
      prepared->pict_tmpfile_path, ret, artwork_image_size
    );
    goto error;
  }

  DPRINTF(E_SPAM, L_PLAYER, "Wrote pipe artwork to '%s'\n", prepared->pict_tmpfile_path);

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
      DPRINTF(E_LOG, L_PLAYER, "%s:Invalid key-value pair in Music Assistant metadata: '%s'\n", __func__, item);
      if (key) free(key);
      if (value) free(value);
      return -1;
  }

  DPRINTF(E_SPAM, L_PLAYER, "%s:Parsed Music Assistant metadata key='%s' value='%s'\n", __func__, key, value);

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
          DPRINTF(E_LOG, L_PLAYER, "%s:Invalid duration value in Music Assistant metadata: '%s'\n", __func__, value);
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
          DPRINTF(E_LOG, L_PLAYER, "%s:Invalid progress value in Music Assistant metadata: '%s'\n", __func__, value);
          free(key);
          free(value);
          return -1;
      }
      DPRINTF(E_DBG, L_PLAYER, "%s:Progress metadata value of %s s received and ignored.\n", __func__, value);
      // prepared->input_metadata.pos_ms = progress_sec * 1000;
      // prepared->input_metadata.pos_is_updated = true; // not sure if this is appropriate
      free(key);
      free(value);
  }
  else if (!strncmp(key,MASS_METADATA_ARTWORK_KEY, strlen(MASS_METADATA_ARTWORK_KEY))) {
      message = PIPE_METADATA_MSG_PARTIAL_METADATA;
      prepared->input_metadata.artwork_url = value; // The consumer must free value
      free(key);
      ret = parse_artwork_url(prepared);
      if (ret < 0) {
          DPRINTF(E_LOG, L_PLAYER, "%s:Invalid artwork URL in Music Assistant metadata: '%s'\n", __func__, value);
          return -1;
      }
      // message = PIPE_METADATA_MSG_PICTURE;
  }
  else if (!strncmp(key,MASS_METADATA_VOLUME_KEY, strlen(MASS_METADATA_VOLUME_KEY))) {
    message = PIPE_METADATA_MSG_VOLUME;
    ret = safe_atoi32(value, &prepared->volume);
    if (ret < 0) {
        DPRINTF(E_LOG, L_PLAYER, "%s:Invalid volume value in Music Assistant metadata: '%s'\n", __func__, value);
        free(key);
        free(value);
        return -1;
    }
    free(key);
    free(value);
    DPRINTF(E_SPAM, L_PLAYER, "%s:Parsed Music Assistant volume: %d\n", __func__, prepared->volume);
  }
  else if (!strncmp(key,MASS_METADATA_PIN_KEY, strlen(MASS_METADATA_PIN_KEY))) {
    message = PIPE_METADATA_MSG_PIN;
    uint32_t pin;
    ret = safe_atou32(value, &pin);
    if (ret < 0 || ret > 9999) { // PIN's limited to 4 digits
        DPRINTF(E_LOG, L_PLAYER, "%s:Invalid PIN value in Music Assistant metadata: '%s'\n", __func__, value);
        free(key);
        free(value);
        return -1;
    }
    ret = asprintf(&prepared->pin, "%.4u", pin);
    free(key);
    free(value);
    DPRINTF(E_SPAM, L_PLAYER, "%s:Parsed Music Assistant PIN: %.4s\n", __func__, prepared->pin);
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
          DPRINTF(E_LOG, L_PLAYER, "%s:Unsupported action value in Music Assistant metadata: '%s'\n", __func__, value);
          free(key);
          free(value);
          return -1;
      }
  }
  else {
      DPRINTF(E_LOG, L_PLAYER, "%s:Unknown key in Music Assistant metadata: '%s=%s'\n", __func__, key, value);
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
    DPRINTF(E_SPAM, L_PLAYER, "%s:Parsed pipe metadata item: '%s'\n", __func__, item);
    ret = parse_mass_item(&message, prepared, item);
    free(item);
    if (ret < 0) {
      DPRINTF(E_LOG, L_PLAYER, "%s:parse_mass_item() failed to parse Music Assistant metadata item\n", __func__);
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

  DPRINTF(E_DBG, L_PLAYER, "%s\n", __func__);

  // Initial read - can maybe use this to undertake any other required initialisation tasks
  if (pipe_id == 0) {
    pipe_id = pipe->id;
    DPRINTF(E_DBG, L_PLAYER, "%s:Initialised global pipe_id to %d\n", __func__, pipe_id);
  }

  ret = player_get_status(&status);
  if (status.id == pipe->id) {
    DPRINTF(E_INFO, L_PLAYER, "%s:Pipe '%s' already playing with status %s\n", 
      __func__, pipe->path, play_status_str(status.status)
    );
    return; // We are already playing the pipe
  }
  else if (ret < 0) {
    DPRINTF(E_LOG, L_PLAYER, 
      "%s:Playback start for audio from '%s' failed because state of player is unknown\n", 
      __func__, pipe->path
    );
    return;
  }

  player_playback_stop(); // Not sure this is a good idea. Will flush data from the input buffer

  DPRINTF(E_SPAM, L_PLAYER, "%s:player_playback_start_byid(%d)\n", __func__, pipe->id);
  ret = player_playback_start_byid(pipe->id);
  if (ret < 0) {
    DPRINTF(E_LOG, L_PLAYER, "%s:Starting playback for data from pipe '%s' (fd %d) failed.\n", 
      __func__, pipe->path, fd
    );
    return;
  }

  /* Music Assistant looks for "restarting w/o pause" */
  DPRINTF(E_INFO, L_PLAYER, "%s: restarting w/o pause\n", __func__);

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
	  DPRINTF(E_SPAM, L_PLAYER, "Pipe watch deleted: '%s'\n", pipe->path);
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
	  DPRINTF(E_LOG, L_PLAYER, "Max open pipes reached (%d), will not watch '%s'\n", PIPE_MAX_WATCH, pipe->path);
	  pipe_free(pipe);
	  continue;
	}

      if (!pipelist_find(pipe_watch_list, pipe->id))
	{
	  watch_add(pipe, evbase_audio_pipe);
	  pipelist_add(&pipe_watch_list, pipe); // Changes pipe->next
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
 * Sets the thread name and launches the event loop for audio data from a named pipe.
 * Exits the thread upon break from the event loop.
 */
static void *
pipe_thread_run(void *arg)
{
  char my_thread[32];

  thread_setname("mass_aud");
  thread_getnametid(my_thread, sizeof(my_thread));
  DPRINTF(E_DBG, L_PLAYER, "%s:About to launch pipe event loop in thread %s\n", __func__, my_thread);
  event_base_dispatch(evbase_audio_pipe);

  pthread_exit(NULL);
}

/* ----------------------- PIPE WATCH THREAD START/STOP --------------------- */
/*                             Thread: mass_aud                            */

/**
 * Establish event and commands base for the mass_aud thread and then create the thread.
 */
static void
pipe_thread_start(void)
{

  DPRINTF(E_DBG, L_PLAYER, "%s\n", __func__);

  CHECK_NULL(L_PLAYER, evbase_audio_pipe = event_base_new());
  CHECK_NULL(L_PLAYER, cmdbase = commands_base_new(evbase_audio_pipe, NULL));
  CHECK_ERR(L_PLAYER, pthread_create(&tid_audio_pipe, NULL, pipe_thread_run, NULL));
  
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

  DPRINTF(E_DBG, L_PLAYER, "%s\n", __func__);

  if (!tid_audio_pipe)
    return;

  commands_exec_sync(cmdbase, pipe_watch_update, NULL, NULL);
  commands_base_destroy(cmdbase);

  ret = pthread_join(tid_audio_pipe, NULL);
  if (ret != 0)
    DPRINTF(E_LOG, L_PLAYER, "Could not join pipe thread: %s\n", strerror(errno));

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

  DPRINTF(E_DBG, L_PLAYER, "%s:Adding %s to the pipelist\n", __func__, mass_named_pipes.audio_pipe);
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
      DPRINTF(E_INFO, L_PLAYER, "%s: No pipelist. Stopping thread.\n", __func__);
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
    DPRINTF(E_LOG, L_PLAYER, "%s:Could not get player status\n", __func__);
    return;
  }

  DPRINTF(E_SPAM, L_PLAYER,
    "%s: player status:%s, volume:%d, pos_ms:%" PRIu32 "\n", 
    __func__, play_status_str(status.status), status.volume, status.pos_ms
  );

  if (status.status == PLAY_PLAYING) {
    if (!player_started) {
      player_started = true;
    }
    DPRINTF(E_DBG, L_PLAYER, 
      "%s: volume:%d state:%s, position:%" PRIu32 " ms. \n",
      __func__, status.volume, play_status_str(status.status), status.pos_ms
    );
  }
  else if (player_started && status.status == PLAY_PAUSED) {
    if (!player_paused) {
      player_paused = true;
      clock_gettime(CLOCK_REALTIME, &paused_start_ts); // reset paused time
      /* Music Assistant looks for "set pause" or "Pause at" */
      DPRINTF(E_INFO, L_PLAYER, "%s: Pause at %" PRIu32 " ms\n",
        __func__, status.pos_ms
      );
    }
    else {
      clock_gettime(CLOCK_REALTIME, &now);
      begin_ms = (uint64_t)paused_start_ts.tv_sec * 1000 + (uint64_t)(paused_start_ts.tv_nsec / 1000000);
      now_ms   = (uint64_t)now.tv_sec * 1000 + (uint64_t)(now.tv_nsec / 1000000);
      elapsed_ms = now_ms - begin_ms;
      DPRINTF(E_SPAM, L_PLAYER, 
        "%s: paused milliseconds:%" PRIu64 " ms at position %" PRIu32 "\n", 
        __func__, elapsed_ms, status.pos_ms
      );

    }
  }
  else { // this state can happen when audio has not yet been received on the named pipe
    DPRINTF(E_DBG, L_PLAYER, "%s:Player %sstarted. status:%s\n", __func__,
      player_started ? "" : "not ", play_status_str(status.status)
    );
    // reset all
    player_started = false;
    player_paused = false;
    elapsed_ms = 0;
  }
}

/**
 * Sets the pause flag
 * @note  This function runs in the mass_cmd thread and shares the pause flag with the
 *        mass_aud thread. It therefore updates the flag within a mutex lock.
 */
static void
self_pause(void)
{
  pthread_mutex_lock(&pause_lock);
  pause_flag = true;
  pthread_mutex_unlock(&pause_lock);
}

/**
 * Unsets the pause flag
 * @note  This function runs in the mass_cmd thread and shares the pause flag with the
 *        mass_aud thread. It therefore updates the flag within a mutex lock.
 */
static void
self_resume(void)
{
  pthread_mutex_lock(&pause_lock);
  pause_flag = false;
  pthread_mutex_unlock(&pause_lock);
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

  DPRINTF(E_DBG, L_PLAYER, "%s:Calling player_speaker_get_byindex(&spk, 0)\n", __func__);
  player_speaker_get_byindex(&spk, 0); // We only ever have one speaker for Music Assistant
  DPRINTF(E_DBG, L_PLAYER, "%s:speaker name:%s, index:%" PRIu32 ", id:%" PRIu64 ", output_type:%s, requires_auth:%s, formats:0x%0x\n", 
    __func__, spk.name, spk.index, spk.id, spk.output_type, spk.requires_auth ? "yes" : "no",
    spk.supported_formats
  );
  
  if (!spk.requires_auth) {
    return;
  }
  player_speaker_authorize(spk.id, ap2_device_info.pin);
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
      DPRINTF(E_LOG, L_PLAYER, "Buffer for command pipe '%s' is full, discarding %zu bytes\n", pipe_metadata.pipe->path, len);
      evbuffer_drain(pipe_metadata.evbuf, len);
      goto readd;
    }
  
  DPRINTF(E_SPAM, L_PLAYER, "%s:Received %zu bytes of metadata\n", __func__, len);

  // .parsed is shared with the input thread (see metadata_get), so use mutex.
  // Note that this means _parse() must not do anything that could cause a
  // deadlock (e.g. make a sync call to the player thread).
  pthread_mutex_lock(&pipe_metadata.prepared.lock);
  ret = pipe_metadata_parse(&message, &pipe_metadata.prepared, pipe_metadata.evbuf);
  pthread_mutex_unlock(&pipe_metadata.prepared.lock);
  if (ret < 0) {
    DPRINTF(E_LOG, L_PLAYER, "Error parsing incoming data on command pipe '%s', will stop reading\n", pipe_metadata.pipe->path);
    pipe_metadata_watch_del(NULL);
    return;
  }

  DPRINTF(E_SPAM, L_PLAYER, "%s:Parsed command pipe message mask: 0x%x\n", __func__, message);

  ret = player_get_status(&status);
  if (ret != COMMAND_END) {
    DPRINTF(E_LOG, L_PLAYER, "%s: Unable to obtain player status\n", __func__);
  }
  if (message & (PIPE_METADATA_MSG_METADATA | PIPE_METADATA_MSG_PICTURE)) {
    pipe_metadata.is_new = 1; // Trigger notification to player in playback loop
    DPRINTF(E_SPAM, L_PLAYER, 
      "%s:Triggered notification to player in the playback loop of new metadata available (message=0x%x)\n", 
      __func__, message
    );
  }
  if (message & PIPE_METADATA_MSG_VOLUME) {
    DPRINTF(E_SPAM, L_PLAYER, "%s:Setting volume from command pipe to %d\n", __func__, pipe_metadata.prepared.volume);
    player_volume_set(pipe_metadata.prepared.volume);
  }
  if (message & PIPE_METADATA_MSG_PIN) {
    DPRINTF(E_DBG, L_PLAYER, "%s:Setting PIN from command pipe to %s\n", __func__, pipe_metadata.prepared.pin);
    // We only support AirPlay2 at the moment. The below code will need to be changed if we add support
    // for RAOP.
    // TODO: @bradkeifer - migrate to player_speaker_authorize() re issue #37
    // player_verification_kickoff(&pipe_metadata.prepared.pin, OUTPUT_TYPE_AIRPLAY);
    strncpy(ap2_device_info.pin, pipe_metadata.prepared.pin, sizeof(ap2_device_info.pin) - 1);
    // player_speaker_enumerate(speaker_authorize_cb, &ap2_device_info.pin);
    mass_speaker_authorize();
    free(pipe_metadata.prepared.pin);

  }
  if (message & PIPE_METADATA_MSG_FLUSH) {
    DPRINTF(E_DBG, L_PLAYER, 
      "%s:FLUSH:Flushing playback from command pipe. Current player status is %s\n", 
      __func__, play_status_str(status.status)
    );
    player_playback_flush(); // results in FLUSH to the airplay device
  }
  if (message & PIPE_METADATA_MSG_PAUSE) {
    DPRINTF(E_DBG, L_PLAYER, 
      "%s:PAUSE:Pausing playback from command pipe. Current player status is %s, %" PRIu32 "ms\n",
      __func__, play_status_str(status.status), status.pos_ms
    );
    // We check the current state before confirming what input action to undertake (if any)
    if (status.status == PLAY_PLAYING) {
      self_pause();
      // Report status to Music Assistant
      DPRINTF(E_INFO, L_PLAYER, "%s:Pause at %" PRIu32 "\n", __func__, status.pos_ms);
    }
    else {
      DPRINTF(E_WARN, L_PLAYER, "%s:Command received to PAUSE playback, but current state is %s. Ignoring command.\n",
        __func__, play_status_str(status.status)
      );
    }
  }
  if (message & PIPE_METADATA_MSG_PLAY) {
    DPRINTF(E_DBG, L_PLAYER, 
      "%s:PLAY:(Re)starting playback from command pipe. Current player status is %s, %" PRIu32 "ms\n",
      __func__, play_status_str(status.status), status.pos_ms
    );
    if (status.status != PLAY_PLAYING) {
      self_resume();
      // Report status to Music Assistant
      DPRINTF(E_INFO, L_PLAYER, "%s:Restarted at %" PRIu32 "\n", __func__, status.pos_ms);
    }
    else {
      DPRINTF(E_WARN, L_PLAYER, "%s:Command received to PLAY, but current state is %s. Ignoring command.\n",
        __func__, play_status_str(status.status)
      );
    }
  }
  if (message & PIPE_METADATA_MSG_STOP) {
    DPRINTF(E_DBG, L_PLAYER, "%s:STOP:Stopping playback from command pipe command\n", __func__);
    // We want to gracefully exit when we receive the STOP command. No longer working!
    // Music Assistant is sending a signal to cause graceful exit, so this is ok for the moment.
    if (status.status == PLAY_PLAYING) {
      self_pause();
      input_flush(NULL); // we don't care about losing data for the input_buffer on stop.
      // Report status to Music Assistant
      DPRINTF(E_INFO, L_PLAYER, "%s:Stop at %" PRIu32 "\n", __func__, status.pos_ms);
      // work out a way to initate a graceful exit and call that function here.
    }
    else {
      DPRINTF(E_WARN, L_PLAYER, "%s:Command received to STOP playback, but current state is %s. Ignoring command.\n",
        __func__, play_status_str(status.status)
      );
    }
  }

 readd:
  if (pipe_metadata.pipe && pipe_metadata.pipe->ev) {
    DPRINTF(E_SPAM, L_PLAYER, "%s:Re-adding event for command pipe '%s'\n", __func__, pipe_metadata.pipe->path);
    event_add(pipe_metadata.pipe->ev, NULL);
  }
  else {
    DPRINTF(E_DBG, L_PLAYER, "%s:command pipe '%s' no longer valid, not re-adding event\n",
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
 * @note This function does not return unless the command pipe eent loop is broken.
 */
static void *
command_pipe_thread_run(void *arg)
{
  char my_thread[32];

  thread_setname("mass_cmd");
  thread_getnametid(my_thread, sizeof(my_thread));
  pipe_metadata_watch_add(mass_named_pipes.metadata_pipe);
  // Create a persistent event timer to monitor and report playback status for logging and debugging purposes
  mass_timer_event = event_new(evbase_command_pipe, -1, EV_PERSIST | EV_TIMEOUT, mass_timer_cb, NULL);
  evtimer_add(mass_timer_event, &mass_tv);
  DPRINTF(E_DBG, L_PLAYER, "%s:About to launch command pipe event loop in thread %s\n", __func__, my_thread);
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
 * @param source  Input source to be setup
 * @returns 0 on success, -1 on failure
 */
static int
setup(struct input_source *source)
{
  struct pipe *pipe;
  int fd;

  fd = pipe_open(source->path, 0);
  if (fd < 0)
    return -1;

  CHECK_NULL(L_PLAYER, source->evbuf = evbuffer_new());

  pipe = pipe_create(source->path, source->id, PIPE_PCM, NULL);

  pipe->fd = fd;

  source->input_ctx = pipe;

  source->quality.sample_rate = pipe_sample_rate;
  source->quality.bits_per_sample = pipe_bits_per_sample;
  source->quality.channels = 2;

  return 0;
}

/**
 * Input definition callback function called when input is stopped.
 * @param source  Input source to stop
 * @note  For Music Assistant integration, we never want to close the audio
 *        named pipe until a graceful shutdown is triggered by Music Assistant.
 *        Therfore, the input module sending us a stop is treated as a no-op.
 * @returns 0
 */
static int
stop(struct input_source *source)
{
  return 0;
}

/**
 * Input definition callback function triggered on each iteration of the playback loop
 * @param source  The input source to obtain audio data for
 * @returns 0 on success, -1 on failure
 * @note  We check (inside a mutex lock) if the player is paused, and if not, then we 
 *        read up to PIPE_READ_MAX bytes from the audio named pipe event buffer and
 *        pass this to the input module.
 *        If the player is paused or there is no data to read, we wait for a period by 
 *        calling input_wait() and return.
 * 
 */
static int
play(struct input_source *source)
{
  struct pipe *pipe = source->input_ctx;
  short flags;
  int ret;
  static size_t read_count = 0;
#ifdef DEBUG_INPUT
  static size_t read_bytes = 0;
#endif

  pthread_mutex_lock(&pause_lock);
  if (pause_flag) {
    pthread_mutex_unlock(&pause_lock);
    input_wait();
    return 0; // loop
  }
  pthread_mutex_unlock(&pause_lock);

  ret = evbuffer_read(source->evbuf, pipe->fd, PIPE_READ_MAX); // read from the audio named pipe
  if (ret == 0) {
    input_write(source->evbuf, NULL, INPUT_FLAG_EOF); // Autostop
    stop(source);
    DPRINTF(E_INFO, L_PLAYER, "%s:end of stream reached\n", __func__);
    return -1;
  }
  else if ((ret == 0) || ((ret < 0) && (errno == EAGAIN))) {
    input_wait();
    return 0; // Loop
  }
  else if (ret < 0) {
    DPRINTF(E_LOG, L_PLAYER, "Could not read from pipe '%s': %s\n", source->path, strerror(errno));
    input_write(NULL, NULL, INPUT_FLAG_ERROR);
    stop(source);
    return -1;
  }

  // Update Music Assistant that playback is commencing
  if (read_count == 0) {
    DPRINTF(E_INFO, L_PLAYER, "%s:Starting at 0ms\n", __func__);
  }

  read_count++;
#ifdef DEBUG_INPUT
  read_bytes += ret;
#endif

  flags = (pipe_metadata.is_new ? INPUT_FLAG_METADATA : 0);
  pipe_metadata.is_new = 0;
  if (read_count == 1 && ap2_device_info.start_ts.tv_sec != 0) {
    // We want to control the time of playback of the first audio packet
    flags |= INPUT_FLAG_SYNC;
  }

#ifdef DEBUG_INPUT
  DPRINTF(E_DBG, L_PLAYER, "%s:chunk_size:%d read_count:%zu total readbytes:%zu to input\n", __func__, ret, read_count, read_bytes);
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
  *ts = ap2_device_info.start_ts;
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
  ret = pthread_create(&tid_command_pipe, NULL, command_pipe_thread_run, NULL);
  if (ret !=0) {
    DPRINTF(E_LOG, L_PLAYER, "%s:Unable to create command thread. %s\n", __func__, strerror(errno));
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
  DPRINTF(E_DBG, L_PLAYER, "%s\n", __func__);
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
  // Maybe we can add a call to player_device_add(device) in here somewhere to initiate device connection before
  // audio is streamed to the named pipe. Currently, device connection is initiatied on receipt of data on the 
  // audio named pipe.

  CHECK_ERR(L_PLAYER, mutex_init(&pipe_metadata.prepared.lock));
  CHECK_ERR(L_PLAYER, mutex_init(&pause_lock));

  pipe_metadata.prepared.pict_tmpfile_fd = -1;

  pipe_listener_cb(0, NULL); // We will be in the pipe thread once this returns
  CHECK_ERR(L_PLAYER, listener_add(pipe_listener_cb, LISTENER_DATABASE, NULL));

  pipe_sample_rate = cfg_getint(cfg_getsec(cfg, "mass"), "pcm_sample_rate");
  if (pipe_sample_rate != 44100 && pipe_sample_rate != 48000 && pipe_sample_rate != 88200 && pipe_sample_rate != 96000) {
    DPRINTF(E_FATAL, L_PLAYER, "The configuration of pcm_sample_rate is invalid: %d\n", pipe_sample_rate);
    return -1;
  }

  pipe_bits_per_sample = cfg_getint(cfg_getsec(cfg, "mass"), "pcm_bits_per_sample");
  if (pipe_bits_per_sample != 16 && pipe_bits_per_sample != 32) {
    DPRINTF(E_FATAL, L_PLAYER, "The configuration of pipe_bits_per_sample is invalid: %d\n", pipe_bits_per_sample);
    return -1;
  }
  
  command_pipe_init();

  return 0;
}

/**
 * De-initialise the mass (Music Assistant) module.
 */
void
mass_deinit(void)
{
  DPRINTF(E_DBG, L_PLAYER, "%s\n", __func__);
  command_pipe_deinit();

  listener_remove(pipe_listener_cb);
  pipe_thread_stop();

  CHECK_ERR(L_PLAYER, pthread_mutex_destroy(&pipe_metadata.prepared.lock));
  CHECK_ERR(L_PLAYER, pthread_mutex_destroy(&pause_lock));
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
