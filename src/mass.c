/*
 * About mass.c
 * ------------
 * This was copied from pipe.c from the owntones repo and adapted
 * to provide an interface between Music Assistant and the airplay
 * player codebase from owtnones.
 * Raw PCM data is read from a named pipe and streamed to the airplay player
 * Metadata and commands are read from a second named pipe
 * Player status is reported back to Music Assistant on stderr
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
 *
 * About pipe.c
 * --------------
 * This module will read a PCM16 stream from a named pipe and write it to the
 * input buffer. The user may start/stop playback from a pipe by selecting it
 * through a client. If the user has configured pipe_autostart, then pipes in
 * the library will also be watched for data, and playback will start/stop
 * automatically.
 *
 * The module will also look for pipes with a .metadata suffix, and if found,
 * the metadata will be parsed and fed to the player. The metadata must be in
 * the format Shairport uses for this purpose.
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
#include "mass.h"
#include "wrappers.h"

#define MASS_UPDATE_INTERVAL_SEC 1 // every second
#define MASS_MS_TILL_EXIT 5000 // exit after 5 seconds of pause
#define MASS_METADATA_KEYVAL_SEP "="  // Key-value separator in metadata
#define MASS_METADATA_PROGRESS_KEY "PROGRESS"
#define MASS_METADATA_VOLUME_KEY   "VOLUME"
#define MASS_METADATA_ARTWORK_KEY  "ARTWORK"
#define MASS_METADATA_ALBUM_KEY    "ALBUM"
#define MASS_METADATA_TITLE_KEY    "TITLE"
#define MASS_METADATA_ARTIST_KEY   "ARTIST"
#define MASS_METADATA_DURATION_KEY  "DURATION"
#define MASS_METADATA_ACTION_KEY    "ACTION"

/* from cliap2.c */
extern char* gnamed_pipe;
extern struct event_base *evbase_main;

/* from player.c */
extern struct event_base *evbase_player;

 /* mass specific stuff - run the timer in the main thread, so we can initiate program termination */
static struct event *mass_timer_event = NULL;
static struct timeval mass_tv = { MASS_UPDATE_INTERVAL_SEC, 0};
// static struct timespec playback_start_ts = {0, 0};
static struct timespec paused_start_ts = {0, 0};
static bool player_started = false;
static bool player_paused = false;

// Maximum number of pipes to watch for data
#define PIPE_MAX_WATCH 4
// Max number of bytes to read from a pipe at a time
#define PIPE_READ_MAX 65536
// Max number of bytes to buffer from metadata pipes
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
};

struct pipe
{
  int id;               // The mfi id of the pipe
  int fd;               // File descriptor
  bool is_autostarted;  // We autostarted the pipe (and we will autostop)
  char *path;           // Path
  enum pipetype type;   // PCM (audio) or metadata
  event_callback_fn cb; // Callback when there is data to read
  struct event *ev;     // Event for the callback

  struct pipe *next;
};

// struct for storing the data received via a metadata pipe
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
static pthread_t tid_pipe;
static struct event_base *evbase_pipe;
static struct commands_base *cmdbase;

// for timer events
static struct event_base *evbase_timer;

// From config - the sample rate and bps of the pipe input
static int pipe_sample_rate;
static int pipe_bits_per_sample;
// From config - should we watch library pipes for data or only start on request
static int pipe_autostart;
// The mfi id of the pipe autostarted by the pipe thread
static int pipe_autostart_id;

// Global list of pipes we are watching (if watching/autostart is enabled)
static struct pipe *pipe_watch_list;

// Pipe + extra fields that we start watching for metadata after playback starts
static struct pipe_metadata pipe_metadata;

/* NTP timestamp definitions - not nice - these are repeated elsewhare in the owntones codebase */
#define FRAC             4294967296. /* 2^32 as a double */
#define NTP_EPOCH_DELTA  0x83aa7e80  /* 2208988800 - that's 1970 - 1900 in seconds */

struct ntp_stamp
{
  uint32_t sec;
  uint32_t frac;
};

/* -------------------------------- HELPERS --------------------------------- */

// These might be more at home in dmap_common.c
static inline uint32_t
dmap_str2val(const char s[4])
{
  return ((s[0] << 24) | (s[1] << 16) | (s[2] << 8) | (s[3] << 0));
}

static void
dmap_val2str(char buf[5], uint32_t val)
{
  buf[0] = (val >> 24) & 0xff;
  buf[1] = (val >> 16) & 0xff;
  buf[2] = (val >> 8) & 0xff;
  buf[3] = val & 0xff;
  buf[4] = 0;
}

static const char *pipetype_str(enum pipetype type)
{
  switch (type)
    {
    case PIPE_PCM:
      return "PCM";
    case PIPE_METADATA:
      return "Metadata";
    default:
      return "Unknown";
    }
}

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

static inline void
timespec_to_ntp(struct timespec *ts, struct ntp_stamp *ns)
{
  // Seconds since NTP Epoch (1900-01-01)
  ns->sec = ts->tv_sec + NTP_EPOCH_DELTA;

  ns->frac = (uint32_t)((double)ts->tv_nsec * 1e-9 * FRAC);
}

static inline int
timing_get_clock_ntp(struct ntp_stamp *ns)
{
  struct timespec ts;
  int ret;

  ret = clock_gettime(CLOCK_REALTIME, &ts);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Couldn't get clock: %s\n", strerror(errno));

      return -1;
    }

  timespec_to_ntp(&ts, ns);

  return 0;
}

/* -------------------------------------------------------------------------*/
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

static void
pipe_free(struct pipe *pipe)
{
  free(pipe->path);
  free(pipe);
}

static int
pipe_open(const char *path, bool silent)
{
  struct stat sb;
  int fd;

  DPRINTF(E_DBG, L_PLAYER, "(Re)opening pipe: '%s'\n", path);

  fd = open(path, O_RDONLY | O_NONBLOCK);
  if (fd < 0)
    {
      DPRINTF(E_LOG, L_PLAYER, "Could not open pipe for reading '%s': %s\n", path, strerror(errno));
      goto error;
    }

  if (fstat(fd, &sb) < 0)
    {
      if (!silent)
	DPRINTF(E_LOG, L_PLAYER, "Could not fstat() '%s': %s\n", path, strerror(errno));
      goto error;
    }

  if (!S_ISFIFO(sb.st_mode))
    {
      DPRINTF(E_LOG, L_PLAYER, "Source type is pipe, but path is not a fifo: %s\n", path);
      goto error;
    }

  return fd;

 error:
  if (fd >= 0)
    close(fd);

  return -1;
}

static void
pipe_close(int fd)
{
  if (fd >= 0)
    close(fd);
}

static int
watch_add(struct pipe *pipe)
{
  pipe->fd = pipe_open(pipe->path, 0);
  if (pipe->fd < 0)
    return -1;

  pipe->ev = event_new(evbase_pipe, pipe->fd, EV_READ, pipe->cb, pipe);
  if (!pipe->ev)
    {
      DPRINTF(E_LOG, L_PLAYER, "Could not watch pipe for new data '%s'\n", pipe->path);
      pipe_close(pipe->fd);
      return -1;
    }

  event_add(pipe->ev, NULL);

  return 0;
}

static void
watch_del(struct pipe *pipe)
{
  if (pipe->ev)
    event_free(pipe->ev);

  pipe_close(pipe->fd);

  pipe->fd = -1;
}

// If a read on pipe returns 0 it is an EOF, and we must close it and reopen it
// for renewed watching. The event will be freed and reallocated by this.
static int
watch_reset(struct pipe *pipe)
{
  if (!pipe)
    return -1;

  watch_del(pipe);

  return watch_add(pipe);
}

static void
pipelist_add(struct pipe **list, struct pipe *pipe)
{
  pipe->next = *list;
  *list = pipe;
}

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

static void
pict_tmpfile_close(int fd, const char *path)
{
  if (fd < 0)
    return;

  close(fd);
  unlink(path);
}

// Opens a tmpfile to store metadata artwork in. *ext is the extension to use
// for the tmpfile, eg .jpg or .png. Extension cannot be longer than
// PIPE_TMPFILE_TEMPLATE_EXTLEN. If fd is set (non-negative) then the file is
// closed first and deleted (using unlink, so path must be valid). The path
// buffer will be updated with the new tmpfile, and the fd is returned.
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

static int
parse_progress(struct pipe_metadata_prepared *prepared, char *progress)
{
  struct input_metadata *m = &prepared->input_metadata;
  char *s;
  char *ptr;
  // Below must be signed to avoid casting in the calculations of pos_ms/len_ms
  int64_t start;
  int64_t pos;
  int64_t end;

  if (!(s = strtok_r(progress, "/", &ptr)))
    goto error;
  safe_atoi64(s, &start);

  if (!(s = strtok_r(NULL, "/", &ptr)))
    goto error;
  safe_atoi64(s, &pos);

  if (!(s = strtok_r(NULL, "/", &ptr)))
    goto error;
  safe_atoi64(s, &end);

  if (!start || !pos || !end)
    goto error;

  // Note that negative positions are allowed and supported. A negative position
  // of e.g. -1000 means that the track will start in one second.
  m->pos_is_updated = true;
  m->pos_ms = (pos - start) * 1000 / pipe_sample_rate;
  m->len_ms = (end > start) ? (end - start) * 1000 / pipe_sample_rate : 0;

  DPRINTF(E_DBG, L_PLAYER, "Received Shairport metadata progress: %" PRIi64 "/%" PRIi64 "/%" PRIi64 " => %d/%u ms\n", start, pos, end, m->pos_ms, m->len_ms);

  return 0;

 error:
  DPRINTF(E_LOG, L_PLAYER, "Received unexpected Shairport metadata progress: %s\n", progress);
  return -1;
}

static int
parse_volume(struct pipe_metadata_prepared *prepared, const char *volume)
{
  char *volume_next;
  float airplay_volume;
  int local_volume;

  errno = 0;
  airplay_volume = strtof(volume, &volume_next);

  if ((errno == ERANGE) || (volume == volume_next))
    {
      DPRINTF(E_LOG, L_PLAYER, "Invalid Shairport airplay volume in string (%s): %s\n", volume,
	      (errno == ERANGE ? strerror(errno) : "First token is not a number."));
      goto error;
    }

  if (strcmp(volume_next, ",0.00,0.00,0.00") != 0)
    {
      DPRINTF(E_DBG, L_PLAYER, "Not applying Shairport airplay volume while software volume control is enabled (%s)\n", volume);
      goto error; // Not strictly an error but goes through same flow
    }

  if (((int) airplay_volume) == -144)
    {
      DPRINTF(E_DBG, L_PLAYER, "Applying Shairport airplay volume ('mute', value: %.2f)\n", airplay_volume);
      prepared->volume = 0;
    }
  else if (airplay_volume >= -30.0 && airplay_volume <= 0.0)
    {
      local_volume = (int)(100.0 + (airplay_volume / 30.0 * 100.0));

      DPRINTF(E_DBG, L_PLAYER, "Applying Shairport airplay volume (percent: %d, value: %.2f)\n", local_volume, airplay_volume);
      prepared->volume = local_volume;
    }
  else
    {
      DPRINTF(E_LOG, L_PLAYER, "Shairport airplay volume out of range (-144.0, [-30.0 - 0.0]): %.2f\n", airplay_volume);
      goto error;
    }

  return 0;

 error:
  return -1;
}

/* Retrieves artwork from a URL and writes the artwork to a tmpfile
 * associated with the named pipes used for streaming and metadata.
 * The tmpfile path is stored in prepared->pict_tmpfile_path and can be used by the airplay output module.
 * The artwork URL is freed.
 * 
 * @out ?
 * @in prepared   Prepared metadata struct containing the artwork URL
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
    DPRINTF(E_LOG, L_PLAYER, "Incomplete write of artwork to '%s' (%zd/%d)\n",
      prepared->pict_tmpfile_path, ret, artwork_image_size
    );
    goto error;
  }

  DPRINTF(E_DBG, L_PLAYER, "Wrote pipe artwork to '%s'\n", prepared->pict_tmpfile_path);

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

static int
parse_picture(struct pipe_metadata_prepared *prepared, uint8_t *data, int data_len)
{
  struct input_metadata *m = &prepared->input_metadata;
  const char *ext;
  ssize_t ret;

  free(m->artwork_url);
  m->artwork_url = NULL;

  if (data_len < 2 || data_len > PIPE_PICTURE_SIZE_MAX)
    {
      DPRINTF(E_WARN, L_PLAYER, "Unsupported picture size (%d) from Shairport metadata pipe\n", data_len);
      goto error;
    }

  if (data[0] == 0xff && data[1] == 0xd8)
    ext = ".jpg";
  else if (data[0] == 0x89 && data[1] == 0x50)
    ext = ".png";
  else
    {
      DPRINTF(E_LOG, L_PLAYER, "Unsupported picture format from Shairport metadata pipe\n");
      goto error;
    }

  prepared->pict_tmpfile_fd = pict_tmpfile_recreate(prepared->pict_tmpfile_path, sizeof(prepared->pict_tmpfile_path), prepared->pict_tmpfile_fd, ext);
  if (prepared->pict_tmpfile_fd < 0)
    {
      DPRINTF(E_LOG, L_PLAYER, "Could not open tmpfile for pipe artwork '%s': %s\n", prepared->pict_tmpfile_path, strerror(errno));
      goto error;
    }

  ret = write(prepared->pict_tmpfile_fd, data, data_len);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_PLAYER, "Error writing artwork from metadata pipe to '%s': %s\n", prepared->pict_tmpfile_path, strerror(errno));
      goto error;
    }
  else if (ret != data_len)
    {
      DPRINTF(E_LOG, L_PLAYER, "Incomplete write of artwork to '%s' (%zd/%d)\n", prepared->pict_tmpfile_path, ret, data_len);
      goto error;
    }

  DPRINTF(E_DBG, L_PLAYER, "Wrote pipe artwork to '%s'\n", prepared->pict_tmpfile_path);

  m->artwork_url = safe_asprintf("file:%s", prepared->pict_tmpfile_path);

  return 9;

 error:
  return -1;
}

static void
log_incoming(int severity, const char *msg, uint32_t type, uint32_t code, int data_len)
{
  char typestr[5];
  char codestr[5];

  dmap_val2str(typestr, type);
  dmap_val2str(codestr, code);

  DPRINTF(severity, L_PLAYER, "%s (type=%s, code=%s, len=%d)\n", msg, typestr, codestr, data_len);
}

/* Example of xml item:

<item><type>73736e63</type><code>6d647374</code><length>9</length>
<data encoding="base64">
NDE5OTg3OTU0</data></item>
*/
static int
parse_item_xml(uint32_t *type, uint32_t *code, uint8_t **data, int *data_len, const char *item)
{
  xml_node *xml;
  const char *s;

//  DPRINTF(E_DBG, L_PLAYER, "Got pipe metadata item: '%s'\n", item);

  xml = xml_from_string(item);
  if (!xml)
    {
      DPRINTF(E_LOG, L_PLAYER, "Could not parse pipe metadata item: %s\n", item);
      goto error;
    }

  *type = 0;
  if ((s = xml_get_val(xml, "item/type")))
    sscanf(s, "%8x", type);

  *code = 0;
  if ((s = xml_get_val(xml, "item/code")))
    sscanf(s, "%8x", code);

  if (*type == 0 || *code == 0)
    {
      DPRINTF(E_LOG, L_PLAYER, "No type (%d) or code (%d) in pipe metadata: %s\n", *type, *code, item);
      goto error;
    }

  *data = NULL;
  *data_len = 0;
  if ((s = xml_get_val(xml, "item/data")))
    {
      *data = b64_decode(data_len, s);
      if (*data == NULL)
	{
	  DPRINTF(E_LOG, L_PLAYER, "Base64 decode of '%s' failed\n", s);
	  goto error;
	}
    }

  log_incoming(E_SPAM, "Read Shairport metadata", *type, *code, *data_len);

  xml_free(xml);
  return 0;

 error:
  xml_free(xml);
  return -1;
}

static
void extract_key_value(const char *input_string, char **key, char **value) {
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

// Parse one metadata item from Music Assistant
// TODO: @bradkeifer - check memory management to ensure key and value are freed properly
static int
parse_mass_item(enum pipe_metadata_msg *out_msg, struct pipe_metadata_prepared *prepared, const char *item)
{
  struct input_metadata *m = &prepared->input_metadata;
  uint32_t type;
  uint32_t code;
  uint8_t *data;
  int data_len;
  char **dstptr;
  enum pipe_metadata_msg message;
  int ret;
  char *key, *value = NULL;
  int duration_sec = 0;

  extract_key_value(item, &key, &value);
  if (!key || !value) {
      DPRINTF(E_LOG, L_PLAYER, "%s:Invalid key-value pair in Music Assistant metadata: '%s'\n", __func__, item);
      if (key) free(key);
      if (value) free(value);
      return -1;
  }

  DPRINTF(E_DBG, L_PLAYER, "%s:Parsed Music Assistant metadata key='%s' value='%s'\n", __func__, key, value);

  if (!strncmp(key,MASS_METADATA_ALBUM_KEY, strlen(MASS_METADATA_ALBUM_KEY))) {
      message = PIPE_METADATA_MSG_PARTIAL_METADATA;
      prepared->input_metadata.album = value; // we should not free value now. The consumer must free it later.
      free(key);
  }
  else if (!strncmp(key,MASS_METADATA_ARTIST_KEY, strlen(MASS_METADATA_ARTIST_KEY))) {
      message = PIPE_METADATA_MSG_PARTIAL_METADATA;
      prepared->input_metadata.artist = value; // we should not free value now. The consumer must free it later.
      free(key);
  }
  else if (!strncmp(key,MASS_METADATA_TITLE_KEY, strlen(MASS_METADATA_TITLE_KEY))) {
      message = PIPE_METADATA_MSG_PARTIAL_METADATA;
      prepared->input_metadata.title = value; // we should not free value now. The consumer must free it later.
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
      ret = safe_atoi32(value, &prepared->input_metadata.pos_ms);
      if (ret < 0) {
          DPRINTF(E_LOG, L_PLAYER, "%s:Invalid progress value in Music Assistant metadata: '%s'\n", __func__, value);
          free(key);
          free(value);
          return -1;
      }
      prepared->input_metadata.pos_is_updated = true; // not sure if this is appropriate
      free(key);
      free(value);
  }
  else if (!strncmp(key,MASS_METADATA_ARTWORK_KEY, strlen(MASS_METADATA_ARTWORK_KEY))) {
      message = PIPE_METADATA_MSG_PARTIAL_METADATA;
      prepared->input_metadata.artwork_url = value; // we should not free value now. The consumer must free it later.
      free(key);
      ret = parse_artwork_url(prepared);
      if (ret < 0) {
          DPRINTF(E_LOG, L_PLAYER, "%s:Invalid artwork URL in Music Assistant metadata: '%s'\n", __func__, value);
          return -1;
      }
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
    DPRINTF(E_DBG, L_PLAYER, "%s:Parsed Music Assistant volume: %d\n", __func__, prepared->volume);
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

static int
parse_item(enum pipe_metadata_msg *out_msg, struct pipe_metadata_prepared *prepared, const char *item)
{
  struct input_metadata *m = &prepared->input_metadata;
  uint32_t type;
  uint32_t code;
  uint8_t *data;
  int data_len;
  char **dstptr;
  enum pipe_metadata_msg message;
  int ret;

  ret = parse_item_xml(&type, &code, &data, &data_len, item);
  if (ret < 0)
    return -1;

  dstptr = NULL;
  message = PIPE_METADATA_MSG_METADATA;

  if (code == dmap_str2val("asal"))
    dstptr = &m->album;
  else if (code == dmap_str2val("asar"))
    dstptr = &m->artist;
  else if (code == dmap_str2val("minm"))
    dstptr = &m->title;
  else if (code == dmap_str2val("asgn"))
    dstptr = &m->genre;
  else if (code == dmap_str2val("prgr"))
    message = PIPE_METADATA_MSG_PROGRESS;
  else if (code == dmap_str2val("pvol"))
    message = PIPE_METADATA_MSG_VOLUME;
  else if (code == dmap_str2val("PICT"))
    message = PIPE_METADATA_MSG_PICTURE;
  else if (code == dmap_str2val("pfls"))
    message = PIPE_METADATA_MSG_FLUSH;
  else
    goto ignore;

  if (message != PIPE_METADATA_MSG_FLUSH && (!data || data_len == 0))
    {
      log_incoming(E_DBG, "Missing or pending Shairport metadata payload", type, code, data_len);
      goto ignore;
    }

  ret = 0;
  if (message == PIPE_METADATA_MSG_PROGRESS)
    ret = parse_progress(prepared, (char *)data);
  else if (message == PIPE_METADATA_MSG_VOLUME)
    ret = parse_volume(prepared, (char *)data);
  else if (message == PIPE_METADATA_MSG_PICTURE)
    ret= parse_picture(prepared, data, data_len);
  else if (dstptr)
    swap_pointers(dstptr, (char **)&data);

  if (ret < 0)
    goto ignore;

  log_incoming(E_DBG, "Applying Shairport metadata", type, code, data_len);

  *out_msg = message;
  free(data);
  return 0;

 ignore:
  *out_msg = 0;
  free(data);
  return 0;
}

// Music Assistant terminates commands/metadata with a newline. This extracts
// one item from the evbuffer, or NULL if there is no complete item.
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

// Parses the content of the evbuf into a parsed struct. The first arg is
// a bitmask describing all the item types that were found, e.g.
// PIPE_METADATA_MSG_VOLUME | PIPE_METADATA_MSG_METADATA. Returns -1 if the
// evbuf could not be parsed.
static int
pipe_metadata_parse(enum pipe_metadata_msg *out_msg, struct pipe_metadata_prepared *prepared, struct evbuffer *evbuf)
{
  enum pipe_metadata_msg message;
  char *item;
  int ret;

  *out_msg = 0;
  while ((item = extract_item(evbuf))) {
    DPRINTF(E_DBG, L_PLAYER, "%s:Parsed pipe metadata item: '%s'\n", __func__, item);
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
/*                                 Thread: pipe                               */

// Some data arrived on a pipe we watch - let's autostart playback
static void
pipe_read_cb(evutil_socket_t fd, short event, void *arg)
{
  struct pipe *pipe = arg;
  struct player_status status;
  int ret;

  ret = player_get_status(&status);
  if (status.id == pipe->id)
    {
      DPRINTF(E_INFO, L_PLAYER, "Pipe '%s' already playing\n", pipe->path);
      return; // We are already playing the pipe
    }
  else if (ret < 0)
    {
      DPRINTF(E_LOG, L_PLAYER, "Pipe autostart of '%s' failed because state of player is unknown\n", pipe->path);
      return;
    }

  DPRINTF(E_INFO, L_PLAYER, "Autostarting pipe '%s' (fd %d)\n", pipe->path, fd);

  player_playback_stop();

  DPRINTF(E_DBG, L_PLAYER, "player_playback_start_byid(%d)\n", pipe->id);
  ret = player_playback_start_byid(pipe->id);
  if (ret < 0) {
    DPRINTF(E_LOG, L_PLAYER, "Autostarting pipe '%s' (fd %d) failed.\n", pipe->path, fd);
    return;
  }

  /* Music Assistant looks for "restarting w/o pause" */
  DPRINTF(E_INFO, L_PLAYER, "%s: restarting w/o pause\n", __func__);

  pipe_autostart_id = pipe->id;
}

static enum command_state
pipe_watch_reset(void *arg, int *retval)
{
  union pipe_arg *cmdarg = arg;
  struct pipe *pipe;

  pipe_autostart_id = 0;

  pipe = pipelist_find(pipe_watch_list, cmdarg->id);
  if (!pipe)
    return COMMAND_END;

  *retval = watch_reset(pipe);

  return COMMAND_END;
}

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
	  DPRINTF(E_DBG, L_PLAYER, "Pipe watch deleted: '%s'\n", pipe->path);
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
	  DPRINTF(E_DBG, L_PLAYER, "Pipe watch added: '%s'\n", pipe->path);
	  watch_add(pipe);
	  pipelist_add(&pipe_watch_list, pipe); // Changes pipe->next
	}
      else
	{
	  DPRINTF(E_DBG, L_PLAYER, "Pipe watch exists: '%s'\n", pipe->path);
	  pipe_free(pipe);
	}
    }

  *retval = 0;
  return COMMAND_END;
}

static void *
pipe_thread_run(void *arg)
{
  char my_thread[32];

  thread_getnametid(my_thread, sizeof(my_thread));
  DPRINTF(E_DBG, L_PLAYER, "%s:About to launch pipe event loop in thread %s\n", __func__, my_thread);
  event_base_dispatch(evbase_pipe);

  pthread_exit(NULL);
}


/* --------------------------- METADATA PIPE HANDLING ----------------------- */
/*                                Thread: worker                              */

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

// Some metadata arrived on a pipe we watch
static void
pipe_metadata_read_cb(evutil_socket_t fd, short event, void *arg)
{
  enum pipe_metadata_msg message;
  size_t len;
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
      ret = watch_reset(pipe_metadata.pipe);
      if (ret < 0)
	return;
      goto readd;
    }

  len = evbuffer_get_length(pipe_metadata.evbuf);
  if (len > PIPE_METADATA_BUFLEN_MAX)
    {
      DPRINTF(E_LOG, L_PLAYER, "Buffer for metadata pipe '%s' is full, discarding %zu bytes\n", pipe_metadata.pipe->path, len);
      evbuffer_drain(pipe_metadata.evbuf, len);
      goto readd;
    }
  
  DPRINTF(E_DBG, L_PLAYER, "%s:Received %d bytes of metadata\n", __func__, len);

  // .parsed is shared with the input thread (see metadata_get), so use mutex.
  // Note that this means _parse() must not do anything that could cause a
  // deadlock (e.g. make a sync call to the player thread).
  pthread_mutex_lock(&pipe_metadata.prepared.lock);
  ret = pipe_metadata_parse(&message, &pipe_metadata.prepared, pipe_metadata.evbuf);
  pthread_mutex_unlock(&pipe_metadata.prepared.lock);
  if (ret < 0) {
    DPRINTF(E_LOG, L_PLAYER, "Error parsing incoming data on metadata pipe '%s', will stop reading\n", pipe_metadata.pipe->path);
    pipe_metadata_watch_del(NULL);
    return;
  }

  DPRINTF(E_DBG, L_PLAYER, "%s:Parsed metadata pipe message mask: 0x%x\n", __func__, message);

  // We are receiving progress updates from Music Assistant before we receive metadata, and that may be a problem.
  if (message & (PIPE_METADATA_MSG_METADATA | PIPE_METADATA_MSG_PROGRESS | PIPE_METADATA_MSG_PICTURE)) {
    pipe_metadata.is_new = 1; // Trigger notification to player in playback loop
    DPRINTF(E_DBG, L_PLAYER, 
      "%s:Triggered notification to player in the playback loop of new metadata available (message=0x%x)\n", 
      __func__, message
    );
  }
  if (message & PIPE_METADATA_MSG_VOLUME) {
    DPRINTF(E_DBG, L_PLAYER, "%s:Setting volume from metadata pipe to %d\n", __func__, pipe_metadata.prepared.volume);
    player_volume_set(pipe_metadata.prepared.volume);
  }
  if (message & PIPE_METADATA_MSG_FLUSH) {
    DPRINTF(E_DBG, L_PLAYER, "%s:Flushing playback from metadata pipe command\n", __func__);
    player_playback_flush();
  }
  if (message & PIPE_METADATA_MSG_PAUSE) {
    DPRINTF(E_DBG, L_PLAYER, "%s:Pausing playback from metadata pipe command\n", __func__);
    // Cannot call player_playback_pause() from this thread - not sure why, but comment to that effect
    // in player.c
    worker_execute((void *)player_playback_pause, NULL, 0, 0); // get the worker thread to pause the player
  }
  if (message & PIPE_METADATA_MSG_PLAY) {
    DPRINTF(E_DBG, L_PLAYER, "%s:(Re)starting playback from metadata pipe command\n", __func__);
    player_playback_start();
  }
  if (message & PIPE_METADATA_MSG_STOP) {
    DPRINTF(E_DBG, L_PLAYER, "%s:Stopping playback from metadata pipe command\n", __func__);
    player_playback_stop();
  }

 readd:
  if (pipe_metadata.pipe && pipe_metadata.pipe->ev) {
    DPRINTF(E_DBG, L_PLAYER, "%s:Re-adding event for metadata pipe '%s'\n", __func__, pipe_metadata.pipe->path);
    event_add(pipe_metadata.pipe->ev, NULL);
  }
  else {
    DPRINTF(E_DBG, L_PLAYER, "%s:Metadata pipe '%s' no longer valid, not re-adding event\n",
      __func__, pipe_metadata.pipe->path
    );
  }
}

static void
pipe_metadata_watch_add(void *arg)
{
  char *base_path = arg;
  char path[PATH_MAX];
  int ret;

  ret = snprintf(path, sizeof(path), "%s.metadata", base_path);
  if ((ret < 0) || (ret > sizeof(path)))
    return;

  pipe_metadata_watch_del(NULL); // Just in case we somehow already have a metadata pipe open

  pipe_metadata.pipe = pipe_create(path, 0, PIPE_METADATA, pipe_metadata_read_cb);
  pipe_metadata.evbuf = evbuffer_new();

  DPRINTF(E_DBG, L_PLAYER, "%s: watch_add for %s\n", __func__, path);
  ret = watch_add(pipe_metadata.pipe);
  if (ret < 0)
    {
      evbuffer_free(pipe_metadata.evbuf);
      pipe_free(pipe_metadata.pipe);
      pipe_metadata.pipe = NULL;
      return;
    }
}


/* ----------------------- PIPE WATCH THREAD START/STOP --------------------- */
/*                             Thread: filescanner                            */

static void
pipe_thread_start(void)
{
  CHECK_NULL(L_PLAYER, evbase_pipe = event_base_new());
  CHECK_NULL(L_PLAYER, cmdbase = commands_base_new(evbase_pipe, NULL));
  CHECK_ERR(L_PLAYER, pthread_create(&tid_pipe, NULL, pipe_thread_run, NULL));

  thread_setname(tid_pipe, "pipe");
}

static void
pipe_thread_stop(void)
{
  int ret;

  if (!tid_pipe)
    return;

  commands_exec_sync(cmdbase, pipe_watch_update, NULL, NULL);
  commands_base_destroy(cmdbase);

  ret = pthread_join(tid_pipe, NULL);
  if (ret != 0)
    DPRINTF(E_LOG, L_PLAYER, "Could not join pipe thread: %s\n", strerror(errno));

  event_base_free(evbase_pipe);
  tid_pipe = 0;
}

// For Music Assistant we will treat stdin as a PCM pipe
static struct pipe *
pipelist_create(void)
{
  struct query_params qp;
  struct db_media_file_info dbmfi;
  struct pipe *head;
  struct pipe *pipe;
  char filter[32];
  int id;
  int ret;

  DPRINTF(E_DBG, L_PLAYER, "Adding %s to the pipelist\n", gnamed_pipe);
  head = NULL;
  pipe = pipe_create(gnamed_pipe, 1, PIPE_PCM, pipe_read_cb);
  pipelist_add(&head, pipe);

  return head;
}

// Queries the db to see if any pipes are present in the library. If so, starts
// the pipe thread to watch the pipes. If no pipes in library, it will shut down
// the pipe thread.
// For Music Assistant integration, we will always have at least one pipe (stdin)
// which is handled by pipelist_create(), so we should always get a pipelist.
static void
pipe_listener_cb(short event_mask, void *ctx)
{
  union pipe_arg *cmdarg;

  DPRINTF(E_DBG, L_PLAYER, "%s()\n", __func__);

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

  if (!tid_pipe)
    pipe_thread_start();

  commands_exec_async(cmdbase, pipe_watch_update, cmdarg);
}


/* --------------------------- PIPE INPUT INTERFACE ------------------------- */
/*                                Thread: input                               */

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
  pipe->is_autostarted = (source->id == pipe_autostart_id);

  worker_execute(pipe_metadata_watch_add, source->path, strlen(source->path) + 1, 0);

  source->input_ctx = pipe;

  source->quality.sample_rate = pipe_sample_rate;
  source->quality.bits_per_sample = pipe_bits_per_sample;
  source->quality.channels = 2;

  return 0;
}

static int
stop(struct input_source *source)
{
  struct pipe *pipe = source->input_ctx;
  union pipe_arg *cmdarg;

  DPRINTF(E_DBG, L_PLAYER, "Stopping pipe\n");

  if (source->evbuf)
    evbuffer_free(source->evbuf);

  pipe_close(pipe->fd);

  // Reset the pipe and start watching it again for new data. Must be async or
  // we will deadlock from the stop in pipe_read_cb().
  if (pipe_autostart && (cmdarg = malloc(sizeof(union pipe_arg))))
    {
      cmdarg->id = pipe->id;
      commands_exec_async(cmdbase, pipe_watch_reset, cmdarg);
    }

  if (pipe_metadata.pipe)
    worker_execute(pipe_metadata_watch_del, NULL, 0, 0);

  pipe_free(pipe);

  source->input_ctx = NULL;
  source->evbuf = NULL;

  return 0;
}

static int
play(struct input_source *source)
{
  struct pipe *pipe = source->input_ctx;
  short flags;
  int ret;

  ret = evbuffer_read(source->evbuf, pipe->fd, PIPE_READ_MAX);
  if ((ret == 0) && (pipe->is_autostarted))
    {
      input_write(source->evbuf, NULL, INPUT_FLAG_EOF); // Autostop
      stop(source);
      return -1;
    }
  else if ((ret == 0) || ((ret < 0) && (errno == EAGAIN)))
    {
      input_wait();
      return 0; // Loop
    }
  else if (ret < 0)
    {
      DPRINTF(E_LOG, L_PLAYER, "Could not read from pipe '%s': %s\n", source->path, strerror(errno));
      input_write(NULL, NULL, INPUT_FLAG_ERROR);
      stop(source);
      return -1;
    }

  flags = (pipe_metadata.is_new ? INPUT_FLAG_METADATA : 0);
  pipe_metadata.is_new = 0;

  input_write(source->evbuf, &source->quality, flags);

  return 0;
}

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

// Thread: main

// Listener for player events
static void
mass_player_listener_cb(short event_mask, void *ctx)
{
  struct player_status status;
  int ret;

  ret = player_get_status(&status);
  if (ret < 0) {
    DPRINTF(E_LOG, L_PLAYER, "%s: could not get player status\n", __func__);
    return;
  }

  DPRINTF(E_DBG, L_PLAYER, "%s: player status:%s\n", __func__, play_status_str(status.status));

}

static void
mass_timer_cb(int fd, short what, void *arg)
{
  struct timespec now;
  uint64_t elapsed_ms = 0;
  uint64_t begin_ms, now_ms, playing_ms = 0;
  struct player_status status;
  struct ntp_stamp ntp_stamp;
  int ret;

  ret = player_get_status(&status);
  if (ret < 0) {
    DPRINTF(E_LOG, L_PLAYER, "%s(): could not get player status\n", __func__);
    return;
  }

  ret = timing_get_clock_ntp(&ntp_stamp);
  if (ret < 0) {
      DPRINTF(E_LOG, L_AIRPLAY, "Couldn't get current ntp timestamp\n");
      return;
  }

  DPRINTF(E_DBG, L_PLAYER,
    "%s(): player status:%s, volume:%d, pos_ms:%" PRIu32 ", ntp:%" PRIu32 ".%.10" PRIu32 "\n", 
    __func__, play_status_str(status.status), status.volume, status.pos_ms,
    ntp_stamp.sec, ntp_stamp.frac
  );

  if (status.status == PLAY_PLAYING) {
    if (!player_started) {
      player_started = true;
    }
    DPRINTF(E_INFO, L_PLAYER, 
      "%s(): elapsed milliseconds:%" PRIu64 " ms. volume:%d state:%s\n",
      __func__, status.pos_ms, status.volume, play_status_str(status.status)
    );
  }
  else if (player_started && status.status == PLAY_PAUSED) {
    if (!player_paused) {
      player_paused = true;
      clock_gettime(CLOCK_REALTIME, &paused_start_ts); // reset paused time
      /* Music Assistant looks for "set pause" or "Pause at" */
      DPRINTF(E_INFO, L_PLAYER, 
        "%s(): Pause at %d ms, starting paused timer\n", __func__, status.pos_ms
      );
    }
    else {
      clock_gettime(CLOCK_REALTIME, &now);
      begin_ms = (uint64_t)paused_start_ts.tv_sec * 1000 + (uint64_t)(paused_start_ts.tv_nsec / 1000000);
      now_ms   = (uint64_t)now.tv_sec * 1000 + (uint64_t)(now.tv_nsec / 1000000);
      elapsed_ms = now_ms - begin_ms;
      DPRINTF(E_INFO, L_PLAYER, 
        "%s(): paused milliseconds:%" PRIu64 " ms\n", __func__, elapsed_ms
      );

    }
  }
  else { // should not happen, but guard just in case
    DPRINTF(E_WARN, L_PLAYER, "%s():%s: Not playing or paused\n", __func__, play_status_str(status.status));
    // reset all
    player_started = false;
    player_paused = false;
    elapsed_ms = 0;
    playing_ms = 0;
  }
}

int
mass_init(void)
{
  DPRINTF(E_DBG, L_PLAYER, "mass_init()\n");

  CHECK_ERR(L_PLAYER, mutex_init(&pipe_metadata.prepared.lock));

  pipe_metadata.prepared.pict_tmpfile_fd = -1;

  if (!tid_pipe) {
    // main thread
    // Create a persistent event timer in the main event loop to monitor and report playback status
    mass_timer_event = event_new(evbase_main, -1, EV_PERSIST | EV_TIMEOUT, mass_timer_cb, NULL);
    DPRINTF(E_DBG, L_PLAYER, 
      "%s:Activating persistent event timer with timeval %d sec, %d usec\n,",
      __func__, mass_tv.tv_sec, mass_tv.tv_usec
    );
    evtimer_add(mass_timer_event, &mass_tv);
  }

  pipe_autostart = cfg_getbool(cfg_getsec(cfg, "mass"), "autostart");
  if (pipe_autostart)
    {
      pipe_listener_cb(0, NULL); // We will noe be in the pipe thread once this returns
      CHECK_ERR(L_PLAYER, listener_add(pipe_listener_cb, LISTENER_DATABASE, NULL));
    }

  pipe_sample_rate = cfg_getint(cfg_getsec(cfg, "mass"), "pcm_sample_rate");
  if (pipe_sample_rate != 44100 && pipe_sample_rate != 48000 && pipe_sample_rate != 88200 && pipe_sample_rate != 96000)
    {
      DPRINTF(E_FATAL, L_PLAYER, "The configuration of pcm_sample_rate is invalid: %d\n", pipe_sample_rate);
      return -1;
    }

  pipe_bits_per_sample = cfg_getint(cfg_getsec(cfg, "mass"), "pcm_bits_per_sample");
  if (pipe_bits_per_sample != 16 && pipe_bits_per_sample != 32)
    {
      DPRINTF(E_FATAL, L_PLAYER, "The configuration of pipe_bits_per_sample is invalid: %d\n", pipe_bits_per_sample);
      return -1;
    }

  return 0;
}

void
mass_deinit(void)
{
  if (pipe_autostart)
    {
      listener_remove(pipe_listener_cb);
      pipe_thread_stop();
    }

  CHECK_ERR(L_PLAYER, pthread_mutex_destroy(&pipe_metadata.prepared.lock));

  // work out where to locate timer_delete - ie. which thread.
  event_free(mass_timer_event);
}

struct input_definition input_pipe =
{
  .name = "pipe",
  .type = INPUT_TYPE_PIPE,
  .disabled = 0,
  .setup = setup,
  .play = play,
  .stop = stop,
  .metadata_get = metadata_get,
  .init = mass_init,
  .deinit = mass_deinit,
};
