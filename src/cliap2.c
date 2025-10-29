/*
 * Pieces from owntone-server:
 * Copyright (C) 2009-2011 Julien BLACHE <jb@jblache.org>
 *
 * Pieces from mt-daapd:
 * Copyright (C) 2003 Ron Pedde (ron@pedde.com)
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
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <limits.h>
#include <grp.h>
#include <stdint.h>

#ifdef HAVE_SIGNALFD
# include <sys/signalfd.h>
#else
# include <sys/time.h>
# include <sys/event.h>
#endif

#include <getopt.h>
#include <event2/event.h>
#include <event2/thread.h>
#include <libavutil/avutil.h>
#include <libavutil/log.h>
#include <libavformat/avformat.h>
#include <libavfilter/avfilter.h>
#include <curl/curl.h>

#include <pthread.h>
#include <gcrypt.h>

#include "library.h"
#include "logger.h"
#include "misc.h"
#include "player.h"
#include "worker.h"
#include "outputs/rtp_common.h"
#include "wrappers.h"
#include "cliap2.h"
#include "mass.h"

#define TESTRUN_PIPE "/tmp/testrun.pipe"

// NTP timestamp definitions
#define FRAC             4294967296. // 2^32 as a double
#define NTP_EPOCH_DELTA  0x83aa7e80  // 2208988800 - that's 1970 - 1900 in seconds

struct event_base *evbase_main;
static struct event *sig_event;
static int main_exit;
ap2_device_info_t ap2_device_info;
char* gnamed_pipe = NULL;

// Global audio configuration for mass module
// Defaults: 44.1kHz/16-bit ALAC (known working configuration)
int g_sample_rate = 44100;
int g_bits_per_sample = 16;
int g_use_alac = 1;  // Default to ALAC encoding
int g_alac_24bit = 0;  // TODO: 24-bit ALAC doesn't work yet, needs proper encoder implementation

// Global RTP start position (calculated from NTP start time in mass.c)
uint32_t g_rtp_start_pos = 0;

static inline void
timespec_to_ntp(struct timespec *ts, struct ntp_timestamp *ns)
{
  /* Seconds since NTP Epoch (1900-01-01) */
  ns->sec = ts->tv_sec + NTP_EPOCH_DELTA;

  ns->frac = (uint32_t)((double)ts->tv_nsec * 1e-9 * FRAC);
}

static inline int
timing_get_clock_ntp(struct ntp_timestamp *ns)
{
  struct timespec ts;
  int ret;

  ret = clock_gettime(CLOCK_MONOTONIC, &ts);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_AIRPLAY, "Couldn't get clock: %s\n", strerror(errno));

      return -1;
    }

  timespec_to_ntp(&ts, ns);

  return 0;
}

// Get current NTP time as a 64-bit timestamp
// Upper 32 bits = seconds since 1900, lower 32 bits = fraction
// Based on raopcl_get_ntp() from libraop
uint64_t
cliap2_get_ntp(void)
{
  struct timespec ts;
  int ret;

  ret = clock_gettime(CLOCK_REALTIME, &ts);
  if (ret < 0)
    {
      DPRINTF(E_LOG, L_MAIN, "Couldn't get clock for NTP: %s\n", strerror(errno));
      return 0;
    }

  // Convert to NTP epoch (1900-01-01)
  uint32_t seconds = ts.tv_sec + NTP_EPOCH_DELTA;
  uint32_t fraction = (uint32_t)(((double)ts.tv_nsec * 1e-9) * FRAC);

  return ((uint64_t)seconds << 32) | fraction;
}

static void
ntptime(void)
{
  struct ntp_timestamp ns;
  uint64_t t;

  timing_get_clock_ntp(&ns);
  t = ((uint64_t)ns.sec << 32) | ns.frac;

  printf("%" PRIu64 "\n", t);
}

static void
version(void)
{
  fprintf(stdout, "%s %s\n", PACKAGE_NAME, PACKAGE_VERSION);
}

static void
usage(char *program)
{
  version();
  printf("\n");
  printf("Usage: %s [options] <player_hostname>\n\n", program);
  printf("Options:\n");
  printf("  --loglevel <number>       Log level (0-5, default: 1)\n");
  printf("  --logdomains <dom,dom..>  Log domains\n");
  printf("  --name <name>             Name of the AirPlay 2 device (default: cliap2)\n");
  printf("  --hostname <hostname>     Hostname/IP of AirPlay 2 device (required)\n");
  printf("  --address <address>       IP address to bind to (default: 0.0.0.0)\n");
  printf("  --port <port>             Port number to use (default: 7000)\n");
  printf("  --txt <txt>               TXT key-vals for mDNS in format \"key=value\" \"key=value\"\n");
  printf("  --pipe <path>             Path to named pipe for audio input (required)\n");
  printf("  --ntp                     Print current NTP time and exit\n");
  printf("  --ntpstart <ntp>          NTP time to start playback\n");
  printf("  --wait <ms>               Additional time to wait after NTP time in ms\n");
  printf("  --latency <ms>            Latency to apply in ms\n");
  printf("  --volume <0-100>          Initial volume (default: 75)\n");
  printf("  --alac24bit               Use 48kHz/24-bit ALAC (experimental, doesn't work yet)\n");
  printf("  --check                   Print check info and exit\n");
  printf("  --debug <level>           Debug level 0-9 (default: 3)\n");
  printf("  -v, --version             Display version information and exit\n");
  printf("  -h, --help                Display this help and exit\n");
  printf("\n");
  printf("Available log domains:\n");
  logger_domains();
  printf("\n");
}


#ifdef HAVE_SIGNALFD
static void
signal_signalfd_cb(int fd, short event, void *arg)
{
  struct signalfd_siginfo info;
  int status;

  while (read(fd, &info, sizeof(struct signalfd_siginfo)) == sizeof(struct signalfd_siginfo))
    {
      switch (info.ssi_signo)
	{
	  case SIGCHLD:
	    DPRINTF(E_LOG, L_MAIN, "Got SIGCHLD\n");

	    while (waitpid(-1, &status, WNOHANG) > 0)
	      /* Nothing. */ ;
	    break;

	  case SIGINT:
	  case SIGTERM:
	    DPRINTF(E_LOG, L_MAIN, "Got SIGTERM or SIGINT\n");

	    main_exit = 1;
	    break;

	  case SIGHUP:
	    DPRINTF(E_LOG, L_MAIN, "Got SIGHUP\n");

	    if (!main_exit)
	      logger_reinit();
	    break;
	}
    }

  if (main_exit)
    event_base_loopbreak(evbase_main);
  else
    event_add(sig_event, NULL);
}

#else

static void
signal_kqueue_cb(int fd, short event, void *arg)
{
  struct timespec ts;
  struct kevent ke;
  int status;

  ts.tv_sec = 0;
  ts.tv_nsec = 0;

  while (kevent(fd, NULL, 0, &ke, 1, &ts) > 0)
    {
      switch (ke.ident)
	{
	  case SIGCHLD:
	    DPRINTF(E_LOG, L_MAIN, "Got SIGCHLD\n");

	    while (waitpid(-1, &status, WNOHANG) > 0)
	      /* Nothing. */ ;
	    break;

	  case SIGINT:
	  case SIGTERM:
	    DPRINTF(E_LOG, L_MAIN, "Got SIGTERM or SIGINT\n");

	    main_exit = 1;
	    break;

	  case SIGHUP:
	    DPRINTF(E_LOG, L_MAIN, "Got SIGHUP\n");

	    if (!main_exit)
	      logger_reinit();
	    break;
	}
    }

  if (main_exit)
    event_base_loopbreak(evbase_main);
  else
    event_add(sig_event, NULL);
}
#endif

#if (LIBAVCODEC_VERSION_MAJOR < 58) || ((LIBAVCODEC_VERSION_MAJOR == 58) && (LIBAVCODEC_VERSION_MINOR < 18))
static int
ffmpeg_lockmgr(void **pmutex, enum AVLockOp op)
{
  switch (op)
    {
      case AV_LOCK_CREATE:
	*pmutex = malloc(sizeof(pthread_mutex_t));
	if (!*pmutex)
	  return 1;
        CHECK_ERR(L_MAIN, mutex_init(*pmutex));
	return 0;

      case AV_LOCK_OBTAIN:
        CHECK_ERR(L_MAIN, pthread_mutex_lock(*pmutex));
	return 0;

      case AV_LOCK_RELEASE:
        CHECK_ERR(L_MAIN, pthread_mutex_unlock(*pmutex));
	return 0;

      case AV_LOCK_DESTROY:
	CHECK_ERR(L_MAIN, pthread_mutex_destroy(*pmutex));
	free(*pmutex);
        *pmutex = NULL;

	return 0;
    }

  return 1;
}
#endif

// Parses a string of "key=value" "key=value" into a keyval struct
static int
parse_keyval(const char *str, struct keyval *kv)
{
  char *key;
  char *value;
  char *s;
  char *outer_token, *inner_token;
  char *outer_saveptr, *inner_saveptr;

  s = (char *)str;
  if (*s != '"') {
    DPRINTF(E_FATAL, L_MAIN, "Keyval string must start with a double quote (\"), not with '%c':%d\n", *s, *s);
    return -1;
  }
  s++; // Skip opening quote

  // Tokenize the main string by double quotes
  outer_token = strtok_r(s, "\"", &outer_saveptr);
  while (outer_token != NULL) {
    DPRINTF(E_SPAM, L_MAIN, "keyval pair: %s\n", outer_token);
    // For each keyval pair, tokenize by =
    inner_token = strtok_r(outer_token, "=", &inner_saveptr);
    for (int i=0; inner_token != NULL; i++) {
      DPRINTF(E_SPAM, L_MAIN, "  item[%d]: %s\n", i, inner_token);
      switch (i) {
        case 0:
          key = inner_token;
          break;
        case 1:
          value = inner_token;
          DPRINTF(E_DBG, L_MAIN, "Adding keyval: %s=%s\n", key, value);
          keyval_add(kv, key, value);
          break;
        default:
          DPRINTF(E_FATAL, L_MAIN, "Keyval pair '%s' has too many '=' characters\n", outer_token);
          return -1;        
      }
      inner_token = strtok_r(NULL, "=", &inner_saveptr);
    }
    outer_token = strtok_r(NULL, "\"", &outer_saveptr);
    outer_token = strtok_r(NULL, "\"", &outer_saveptr);
  }

  return 0;
}

// Check for valid named pipe.
// @param name the filename of the named pipe
// @returns 0 on success, -1 on failure
static
int check_pipe(const char *pipe_path)
{
  struct stat st;

  // Check if the file exists and get its information
  if (stat(pipe_path, &st) == 0) {
      // File exists, now check if it's a FIFO (named pipe)
      if (S_ISFIFO(st.st_mode)) {
          DPRINTF(E_DBG, L_MAIN, "%s:Named pipe '%s' exists.\n", __func__, pipe_path);
          return 0;
      } 
      else {
          DPRINTF(E_FATAL, L_MAIN, "%s:File '%s' exists, but it is not a named pipe.\n", __func__, pipe_path);
          return -1;
      }
  } 
  else {
      // File does not exist or an error occurred
      if (errno == ENOENT) {
          DPRINTF(E_FATAL, L_MAIN, "%s:Named pipe '%s' does not exist.\n", __func__, pipe_path);
      } 
      else {
          DPRINTF(E_FATAL, L_MAIN, "%s:Error checking for named pipe %s. %s\n", __func__, pipe_path, strerror(errno));
      }
      return -1;
  }

  return 0;
}

// Check for valid named pipe(s).
// @param name the filename of the audio streaming named pipe
// @returns 0 on success, -1 on failure
static
int check_pipes(const char *pipe_path)
{
  if (check_pipe(pipe_path) == 0) {
    int ret;
    char *metadata_path = NULL;

    asprintf(&metadata_path, "%s.metadata", pipe_path);
    ret = check_pipe(metadata_path);
    free(metadata_path);
    if (ret < 0) {
      // Metadata pipe is optional - just warn
      DPRINTF(E_WARN, L_MAIN, "%s: Metadata pipe not found - continuing without it\n", __func__);
    }
    return 0;  // Success even if metadata pipe missing
  }
  return -1;
}

// Create named pipe.
// @param name the filename of the named pipe
// @returns 0 on success, -1 on failure
static
int create_pipe(const char *pipe_path)
{
  struct stat st;

  // Check if the file exists and get its information
  if (stat(pipe_path, &st) == 0) {
      // File exists, now check if it's a FIFO (named pipe)
      if (S_ISFIFO(st.st_mode)) {
          DPRINTF(E_DBG, L_MAIN, "%s:Named pipe '%s' exists.\n", __func__, pipe_path);
          return 0;
      } 
      else {
          DPRINTF(E_FATAL, L_MAIN, "%s:File '%s' exists, but it is not a named pipe.\n", __func__, pipe_path);
          return -1;
      }
  } 
  else {
      // File does not exist, so lets create it
      if (mkfifo(pipe_path, 0666) < 0) {
        DPRINTF(E_FATAL, L_MAIN, "%s:Error creating named pipe %s. %s\n", __func__, pipe_path, strerror(errno));
        return -1;
      }
  }

  return 0;
}

// Create named pipe(s) for testrun purposes
// @param name the filename of the audio streaming named pipe
// @returns 0 on success, -1 on failure
static
int create_pipes(const char *pipe_path)
{
  if (create_pipe(pipe_path) == 0) {
    int ret;
    char *metadata_path = NULL;

    asprintf(&metadata_path, "%s.metadata", pipe_path);
    ret = create_pipe(metadata_path);
    free(metadata_path);
    return ret;
  }
  return -1;
}

// Remove named pipe.
// @param name the filename of the named pipe
// @returns 0 on success, -1 on failure
static
int remove_pipe(const char *pipe_path)
{
  struct stat st;
  int ret;

  // Check if the file exists and get its information
  if (stat(pipe_path, &st) == 0) {
      // File exists, now check if it's a FIFO (named pipe)
      if (S_ISFIFO(st.st_mode)) {
          DPRINTF(E_DBG, L_MAIN, "%s:Named pipe '%s' exists.\n", __func__, pipe_path);
          ret = unlink(pipe_path);
          if (ret != 0) {
            DPRINTF(E_LOG, L_MAIN, "%s:Cannot removed named pipe %s. %s\n", __func__, pipe_path, strerror(errno));
            return ret;
          }
      } 
      else {
          DPRINTF(E_FATAL, L_MAIN, "%s:File '%s' exists, but it is not a named pipe.\n", __func__, pipe_path);
          return -1;
      }
  } 

  return 0;
}

// Remove named pipes created for testrun purposes
// @param name the filename of the audio streaming named pipe
// @returns 0 on success, -1 on failure
static
int remove_pipes(const char *pipe_path)
{
  int audio_error, metadata_error = 0;
  char *metadata_path = NULL;

  audio_error = remove_pipe(pipe_path);

  asprintf(&metadata_path, "%s.metadata", pipe_path);
  metadata_error = remove_pipe(metadata_path);
  free(metadata_path);

  if (audio_error < 0 || metadata_error < 0) {
    return -1;
  }
  return 0;
}

int
main(int argc, char **argv)
{
  int option;
  bool background = false;
  bool testrun = false;
  bool mdns_no_rsp = true;
  bool mdns_no_daap = true;
  bool mdns_no_cname = true;
  bool mdns_no_web = true;
  bool mdns_no_mpd = true;
  int loglevel = -1;
  char *logdomains = NULL;
  char *logfile = NULL;
  char *logformat = NULL;
  char **buildopts;
  const char *av_version;
  const char *gcry_version;
  sigset_t sigs;
  int sigfd;
#ifdef HAVE_KQUEUE
  struct kevent ke_sigs[4];
#endif
  int i;
  int ret;
  int port = -1;
  const char *name = NULL;
  const char *hostname = NULL;
  const char *address = NULL;
  const char *txt = NULL;

  uint64_t ntpstart = 0;
  uint32_t wait = 0;
  uint32_t latency = 0;
  int volume = 0;
  int sample_rate = 48000;  // Default to 48000 Hz
  int debug_level = 3;
  struct keyval *txt_kv = NULL;

  struct option option_map[] = {
    { "loglevel",      1, NULL, 500 },
    { "logdomains",    1, NULL, 501 },
    { "name",          1, NULL, 503 },
    { "hostname",      1, NULL, 504 },
    { "address",       1, NULL, 505 },
    { "port",          1, NULL, 506 },
    { "txt",           1, NULL, 507 },
    { "ntp",           0, NULL, 508 },
    { "ntpstart",      1, NULL, 509 },
    { "wait",          1, NULL, 510 },
    { "latency",       1, NULL, 511 },
    { "volume",        1, NULL, 512 },
    { "version",       0, NULL, 513 },
    { "testrun",       0, NULL, 514 }, // Used for CI, not documented to user
    { "pipe",          1, NULL, 515 },
    { "check",         0, NULL, 518 },
    { "debug",         1, NULL, 519 },
    { "help",          0, NULL, 520 },
    { "alac24bit",     0, NULL, 522 },

    { NULL,            0, NULL, 0   }
  };

  while ((option = getopt_long(argc, argv, "", option_map, NULL)) != -1) {
      switch (option) {
      case 514: // testrun
        testrun = true;
        break;

      case 500: // loglevel
        ret = safe_atoi32(optarg, &option);
        if (ret < 0)
          fprintf(stderr, "Error: loglevel must be an integer in '--loglevel %s'\n", optarg);
        else
          loglevel = option;
        break;

      case 501: // logdomains
        logdomains = optarg;
        break;

      case 513: // version
        version();
        return EXIT_SUCCESS;
        break;

      case 503: // name
        name = optarg;
        break;

      case 504: // hostname
        hostname = optarg;
        break;

      case 505:  // address
        address = optarg;
        break;

      case 506: // port
        ret = safe_atoi32(optarg, &option);
        if (ret < 0)
          fprintf(stderr, "Error: port must be an integer in '--port %s'\n", optarg);
        else
          port = option;
        break;

      case 507: // txt
        txt = optarg;
        break;

      case 508: // ntp
        // output ntp time to stdout and exit
        ntptime();
        return EXIT_SUCCESS;

      case 509:
        // ntpstart
        ret = safe_atou64(optarg, (uint64_t *)&ntpstart);
        if (ret < 0) {
          fprintf(stderr, "Error: ntpstart must be an unsigned 64-bit integer in '--ntpstart %s'\n", optarg);
          exit(EXIT_FAILURE);
        }
        break;
      
      case 510: // wait
        ret = safe_atou32(optarg, &wait);
        if (ret < 0) {
          fprintf(stderr, "Error: wait must be an integer in '--wait %s'\n", optarg);
          exit(EXIT_FAILURE);
        }
        break;

      case 511: // latency
        ret = safe_atou32(optarg, &latency);
        if (ret < 0) {
          fprintf(stderr, "Error: latency must be an integer in '--latency %s'\n", optarg);
          exit(EXIT_FAILURE);
        }
        break;
      
      case 512: // volume
        ret = safe_atoi32(optarg, &volume);
        if (ret < 0) {
          fprintf(stderr, "Error: volume must be an integer in '--volume %s'\n", optarg);
          exit(EXIT_FAILURE);
        }
        break;

      case 515: // named pipe filename
        gnamed_pipe = optarg;
        break;

      case 518: // check
        printf("%s check\n", PACKAGE_NAME);
        return EXIT_SUCCESS;

      case 519: // debug
        ret = safe_atoi32(optarg, &debug_level);
        if (ret < 0 || debug_level < 0 || debug_level > 9) {
          fprintf(stderr, "Error: debug level must be 0-9 in '--debug %s'\n", optarg);
          return EXIT_FAILURE;
        }
        // Map debug level to loglevel if not already set
        if (loglevel < 0) {
          if (debug_level == 0) loglevel = E_FATAL;
          else if (debug_level <= 2) loglevel = E_LOG;
          else if (debug_level <= 4) loglevel = E_INFO;
          else if (debug_level <= 6) loglevel = E_DBG;
          else loglevel = E_SPAM;
        }
        break;

      case 520: // help
        usage(argv[0]);
        return EXIT_SUCCESS;

      case 522: // alac24bit
        // TODO: 24-bit ALAC encoding doesn't work yet - this flag is experimental
        // Need to implement proper 24-bit ALAC encoder or use alternative library
        g_alac_24bit = 1;
        g_sample_rate = 48000;  // Use 48kHz with 24-bit
        g_bits_per_sample = 32;  // Use 32-bit for pipe compatibility
        break;

        default:
      case '?':
        usage(argv[0]);
        return EXIT_FAILURE;
        break;
	  }
  }

  // Check that mandatory arguments have been supplied
  // Only hostname and pipe are truly required for basic operation
  if (!testrun &&
      (hostname == (char *)NULL ||
       gnamed_pipe == (char*)NULL)
     ) {
      fprintf(stderr, "Error: --hostname and --pipe are required\n");
      usage(argv[0]);
      return EXIT_FAILURE;
  }

  // Set sensible defaults for optional arguments
  if (port == -1) port = 7000;  // Default AirPlay port
  if (name == NULL) name = "cliap2";  // Default device name
  if (address == NULL) address = "0.0.0.0";  // Bind to all interfaces
  if (txt == NULL) txt = "";  // Empty TXT record is fine
  if (ntpstart == 0) {
    // If not specified, start immediately (get current NTP time)
    struct ntp_timestamp ntp;
    timing_get_clock_ntp(&ntp);
    ntpstart = (((uint64_t)ntp.sec) << 32) | ntp.frac;
  }
  if (volume == 0) volume = 75;  // Default volume 75%

  // Set global audio configuration for mass module
  g_sample_rate = sample_rate;
  // g_bits_per_sample is set via --alac24bit flag (16-bit default, 32-bit for 24-bit ALAC)

  ret = logger_init(NULL, NULL, (loglevel < 0) ? E_LOG : loglevel, NULL);
  if (ret != 0) {
    fprintf(stderr, "Could not initialize log facility\n");

    return EXIT_FAILURE;
  }
  // logger_detach();  // Eliminate logging to stderr

  logger_deinit();

  /* Reinit log facility with defaults */
  if (loglevel < 0)
    loglevel = E_LOG;
  if (!logformat)
    logformat = "default";
  logfile = NULL;  // No logfile by default for CLI usage

  ret = logger_init(logfile, logdomains, loglevel, logformat);
  if (ret != 0) {
    fprintf(stderr, "Could not reinitialize log facility\n");
    return EXIT_FAILURE;
  }
  // logger_detach();  // Eliminate logging to stderr

  if (testrun) {
    ret = create_pipes(TESTRUN_PIPE);
    if (ret != 0) {
      remove_pipes(TESTRUN_PIPE);
      return EXIT_FAILURE;
    }
    gnamed_pipe = TESTRUN_PIPE;
  }
  else {
    // Check that named pipe exists for audio streaming. Metadata one too?
    ret = check_pipes(gnamed_pipe);
    if (ret < 0) {
      return EXIT_FAILURE;
    }

    CHECK_NULL(L_MAIN, txt_kv = keyval_alloc());

    // Only parse txt if it's not empty
    if (txt != NULL && txt[0] != '\0') {
      ret = parse_keyval(txt, txt_kv);
      if (ret != 0){
        DPRINTF(E_FATAL, L_MAIN,
          "Error: txt keyvals must be in format \"key=value\" \"key=value\" format in '--txt %s'\n",
          txt);
        goto txt_fail;
      }
    }
    ap2_device_info.name = name;
    ap2_device_info.hostname = hostname;
    ap2_device_info.address = address;
    ap2_device_info.port = port;
    ap2_device_info.txt = txt_kv;
    ap2_device_info.ntpstart = ntpstart;
    ap2_device_info.wait = wait;
    ap2_device_info.latency = latency;
    ap2_device_info.volume = volume;
  }

  /* Set up libevent logging callback */
  event_set_log_callback(logger_libevent);

  if (testrun) {
    DPRINTF(E_LOG, L_MAIN, "%s version %s test run\n", PACKAGE, VERSION);
  }
  else {
    DPRINTF(E_LOG, L_MAIN, "%s version %s taking off\n", PACKAGE, VERSION);
  }

#if HAVE_DECL_AV_VERSION_INFO
  av_version = av_version_info();
#else
  av_version = "(unknown version)";
#endif

#ifdef HAVE_FFMPEG
  DPRINTF(E_INFO, L_MAIN, "Initialized with ffmpeg %s\n", av_version);
#else
  DPRINTF(E_INFO, L_MAIN, "Initialized with libav %s\n", av_version);
#endif

// The following was deprecated with ffmpeg 4.0 = avcodec 58.18, avformat 58.12, avfilter 7.16
#if (LIBAVCODEC_VERSION_MAJOR < 58) || ((LIBAVCODEC_VERSION_MAJOR == 58) && (LIBAVCODEC_VERSION_MINOR < 18))
  ret = av_lockmgr_register(ffmpeg_lockmgr);
  if (ret < 0)
    {
      DPRINTF(E_FATAL, L_MAIN, "Could not register ffmpeg lock manager callback\n");

      ret = EXIT_FAILURE;
      goto ffmpeg_init_fail;
    }
#endif
#if (LIBAVFORMAT_VERSION_MAJOR < 58) || ((LIBAVFORMAT_VERSION_MAJOR == 58) && (LIBAVFORMAT_VERSION_MINOR < 12))
  av_register_all();
#endif
#if (LIBAVFILTER_VERSION_MAJOR < 7) || ((LIBAVFILTER_VERSION_MAJOR == 7) && (LIBAVFILTER_VERSION_MINOR < 16))
  avfilter_register_all();
#endif

#if HAVE_DECL_AVFORMAT_NETWORK_INIT
  avformat_network_init();
#endif
  av_log_set_callback(logger_ffmpeg);

  /* Initialize libcurl */
  curl_global_init(CURL_GLOBAL_DEFAULT);

  gcry_version = gcry_check_version(GCRYPT_VERSION);
  if (!gcry_version)
    {
      DPRINTF(E_FATAL, L_MAIN, "libgcrypt version mismatch\n");

      ret = EXIT_FAILURE;
      goto gcrypt_init_fail;
    }

  /* We aren't handling anything sensitive, so give up on secure
   * memory, which is a scarce system resource.
   */
  gcry_control(GCRYCTL_DISABLE_SECMEM, 0);

  gcry_control(GCRYCTL_INITIALIZATION_FINISHED, 0);

  DPRINTF(E_DBG, L_MAIN, "Initialized with gcrypt %s\n", gcry_version);

  /* Block signals for all threads except the main one */
  sigemptyset(&sigs);
  sigaddset(&sigs, SIGINT);
  sigaddset(&sigs, SIGHUP);
  sigaddset(&sigs, SIGCHLD);
  sigaddset(&sigs, SIGTERM);
  sigaddset(&sigs, SIGPIPE);
  ret = pthread_sigmask(SIG_BLOCK, &sigs, NULL);
  if (ret != 0)
    {
      DPRINTF(E_LOG, L_MAIN, "Error setting signal set\n");

      ret = EXIT_FAILURE;
      goto signal_block_fail;
    }

  /* Initialize event base (after forking) */
  CHECK_NULL(L_MAIN, evbase_main = event_base_new());

  CHECK_ERR(L_MAIN, evthread_use_pthreads());


  /* Spawn worker thread */
  ret = worker_init();
  if (ret != 0)
    {
      DPRINTF(E_FATAL, L_MAIN, "Worker thread failed to start\n");

      ret = EXIT_FAILURE;
      goto worker_fail;
    }

  /* Spawn player thread */
  ret = player_init();
  if (ret != 0)
    {
      DPRINTF(E_FATAL, L_MAIN, "Player thread failed to start\n");

      ret = EXIT_FAILURE;
      goto player_fail;
    }

#ifdef HAVE_SIGNALFD
  /* Set up signal fd */
  sigfd = signalfd(-1, &sigs, SFD_NONBLOCK | SFD_CLOEXEC);
  if (sigfd < 0)
    {
      DPRINTF(E_FATAL, L_MAIN, "Could not setup signalfd: %s\n", strerror(errno));

      ret = EXIT_FAILURE;
      goto signalfd_fail;
    }

  sig_event = event_new(evbase_main, sigfd, EV_READ, signal_signalfd_cb, NULL);
#else
  sigfd = kqueue();
  if (sigfd < 0)
    {
      DPRINTF(E_FATAL, L_MAIN, "Could not setup kqueue: %s\n", strerror(errno));

      ret = EXIT_FAILURE;
      goto signalfd_fail;
    }

  EV_SET(&ke_sigs[0], SIGINT, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
  EV_SET(&ke_sigs[1], SIGTERM, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
  EV_SET(&ke_sigs[2], SIGHUP, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
  EV_SET(&ke_sigs[3], SIGCHLD, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);

  ret = kevent(sigfd, ke_sigs, 4, NULL, 0, NULL);
  if (ret < 0)
    {
      DPRINTF(E_FATAL, L_MAIN, "Could not register signal events: %s\n", strerror(errno));

      ret = EXIT_FAILURE;
      goto signalfd_fail;
    }

  sig_event = event_new(evbase_main, sigfd, EV_READ, signal_kqueue_cb, NULL);
#endif
  if (!sig_event)
    {
      DPRINTF(E_FATAL, L_MAIN, "Could not create signal event\n");

      ret = EXIT_FAILURE;
      goto sig_event_fail;
    }

  event_add(sig_event, NULL);

  /* Run the loop */
  if (!testrun)
    event_base_dispatch(evbase_main);
  else {
    if (remove_pipes(TESTRUN_PIPE) == 0) {
      fprintf(stdout, "%s check\n", PACKAGE);
    }
    else {
      fprintf(stdout, "%s fail\n", PACKAGE);
    }
  }

  DPRINTF(E_LOG, L_MAIN, "Stopping gracefully\n");
  ret = EXIT_SUCCESS;

  event_free(sig_event);

 sig_event_fail:
 signalfd_fail:
  DPRINTF(E_LOG, L_MAIN, "Player deinit\n");
  player_deinit();

 player_fail:
  DPRINTF(E_LOG, L_MAIN, "Worker deinit\n");
  worker_deinit();

 worker_fail:
  db_deinit();
  event_base_free(evbase_main);

 signal_block_fail:
 gcrypt_init_fail:
  curl_global_cleanup();
#if HAVE_DECL_AVFORMAT_NETWORK_INIT
  avformat_network_deinit();
#endif

#if (LIBAVCODEC_VERSION_MAJOR < 58) || ((LIBAVCODEC_VERSION_MAJOR == 58) && (LIBAVCODEC_VERSION_MINOR < 18))
  av_lockmgr_register(NULL);
 ffmpeg_init_fail:
#endif

 txt_fail:
  if (txt_kv) keyval_clear(txt_kv);

  DPRINTF(E_LOG, L_MAIN, "Exiting.\n");
  logger_deinit();

  return ret;
}
