/*
 * owntone-server parts:
 * Copyright (C) 2009-2011 Julien BLACHE <jb@jblache.org>
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
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include <sys/types.h>
#include <sys/utsname.h>
#include <pwd.h>

#include <errno.h>
#include <inttypes.h>

#include <confuse.h>

#include "logger.h"
#include "misc.h"
#include "conffile.h"
#include "cliap2.h"

extern ap2_device_info_t ap2_device_info;


/* Forward */
static int cb_loglevel(cfg_t *cfg, cfg_opt_t *opt, const char *value, void *result);

/* general section structure */
static cfg_opt_t sec_general[] =
  {
    CFG_STR("uid", "nobody", CFGF_NONE),
    CFG_STR("logfile", STATEDIR "/log/" PACKAGE ".log", CFGF_NONE),
    CFG_INT_CB("loglevel", E_LOG, CFGF_NONE, &cb_loglevel),
    CFG_STR("logformat", "default", CFGF_NONE),
    CFG_STR_LIST("trusted_networks", "{lan}", CFGF_NONE),
    CFG_BOOL("ipv6", cfg_false, CFGF_NONE),
    CFG_STR("bind_address", NULL, CFGF_NONE),
    CFG_BOOL("speaker_autoselect", cfg_true, CFGF_NONE),
#if defined(__FreeBSD__) || defined(__FreeBSD_kernel__)
    CFG_BOOL("high_resolution_clock", cfg_false, CFGF_NONE),
#else
    CFG_BOOL("high_resolution_clock", cfg_true, CFGF_NONE),
#endif
    // Hidden options
    CFG_STR("allow_origin", "*", CFGF_NONE),
    CFG_STR("user_agent", PACKAGE_NAME "/" PACKAGE_VERSION, CFGF_NONE),
    CFG_BOOL("ssl_verifypeer", cfg_true, CFGF_NONE),
    CFG_BOOL("timer_test", cfg_false, CFGF_NONE),
    CFG_INT("start_buffer_ms", 2250, CFGF_NONE),
    CFG_END()
  };


/* library section structure */
static cfg_opt_t sec_library[] =
  {
    CFG_STR("name", "Music Assistant", CFGF_NONE),
    CFG_INT("port", 3689, CFGF_NONE),
    CFG_STR("password", NULL, CFGF_NONE),
    CFG_STR_LIST("directories", NULL, CFGF_NONE),
    CFG_BOOL("follow_symlinks", cfg_true, CFGF_NONE),
    CFG_STR_LIST("podcasts", NULL, CFGF_NONE),
    CFG_STR_LIST("audiobooks", NULL, CFGF_NONE),
    CFG_STR_LIST("compilations", NULL, CFGF_NONE),
    CFG_STR("compilation_artist", NULL, CFGF_NONE),
    CFG_BOOL("hide_singles", cfg_false, CFGF_NONE),
    CFG_BOOL("radio_playlists", cfg_false, CFGF_NONE),
    CFG_STR("name_library", "Library", CFGF_NONE),
    CFG_STR("name_music", "Music", CFGF_NONE),
    CFG_STR("name_movies", "Movies", CFGF_NONE),
    CFG_STR("name_tvshows", "TV Shows", CFGF_NONE),
    CFG_STR("name_podcasts", "Podcasts", CFGF_NONE),
    CFG_STR("name_audiobooks", "Audiobooks", CFGF_NONE),
    CFG_STR("name_radio", "Radio", CFGF_NONE),
    CFG_STR("name_unknown_title", "Unknown title", CFGF_NONE),
    CFG_STR("name_unknown_artist", "Unknown artist", CFGF_NONE),
    CFG_STR("name_unknown_album", "Unknown album", CFGF_NONE),
    CFG_STR("name_unknown_genre", "Unknown genre", CFGF_NONE),
    CFG_STR("name_unknown_composer", "Unknown composer", CFGF_NONE),
    CFG_STR_LIST("artwork_basenames", "{artwork,cover,Folder}", CFGF_NONE),
    CFG_BOOL("artwork_individual", cfg_false, CFGF_NONE),
    CFG_STR_LIST("artwork_online_sources", NULL, CFGF_NONE),
    CFG_STR_LIST("filetypes_ignore", "{.db,.ini,.db-journal,.pdf,.metadata}", CFGF_NONE),
    CFG_STR_LIST("filepath_ignore", NULL, CFGF_NONE),
    CFG_BOOL("filescan_disable", cfg_false, CFGF_NONE),
    CFG_BOOL("m3u_overrides", cfg_false, CFGF_NONE),
    CFG_BOOL("itunes_overrides", cfg_false, CFGF_NONE),
    CFG_BOOL("itunes_smartpl", cfg_false, CFGF_NONE),
    CFG_STR_LIST("no_decode", NULL, CFGF_NONE),
    CFG_STR_LIST("force_decode", NULL, CFGF_NONE),
    CFG_STR("prefer_format", NULL, CFGF_NONE),
    CFG_BOOL("pipe_autostart", cfg_true, CFGF_NONE),
    CFG_INT("pipe_sample_rate", 44100, CFGF_NONE),
    CFG_INT("pipe_bits_per_sample", 16, CFGF_NONE),
    CFG_BOOL("rating_updates", cfg_false, CFGF_NONE),
    CFG_BOOL("read_rating", cfg_false, CFGF_NONE),
    CFG_BOOL("write_rating", cfg_false, CFGF_NONE),
    CFG_INT("max_rating", 100, CFGF_NONE),
    CFG_BOOL("allow_modifying_stored_playlists", cfg_false, CFGF_NONE),
    CFG_STR("default_playlist_directory", NULL, CFGF_NONE),
    CFG_BOOL("clear_queue_on_stop_disable", cfg_false, CFGF_NONE),
    CFG_BOOL("only_first_genre", cfg_false, CFGF_NONE),
    CFG_STR_LIST("decode_audio_filters", NULL, CFGF_NONE),
    CFG_STR_LIST("decode_video_filters", NULL, CFGF_NONE),
    CFG_END()
  };

  /* Music Assistant section structure */
static cfg_opt_t sec_mass[] =
  {
    CFG_INT("pcm_sample_rate", 44100, CFGF_NONE),
    CFG_INT("pcm_bits_per_sample", 16, CFGF_NONE),

  };


/* AirPlay/ApEx shared section structure */
static cfg_opt_t sec_airplay_shared[] =
  {
    CFG_INT("control_port", 0, CFGF_NONE),
    CFG_INT("timing_port", 0, CFGF_NONE),
    CFG_BOOL("uncompressed_alac", cfg_false, CFGF_NONE),
    CFG_END()
  };

/* AirPlay/ApEx device section structure */
static cfg_opt_t sec_airplay[] =
  {
    CFG_INT("max_volume", 11, CFGF_NONE),
    CFG_BOOL("exclude", cfg_false, CFGF_NONE),
    CFG_BOOL("permanent", cfg_false, CFGF_NONE),
    CFG_BOOL("reconnect", cfg_false, CFGF_NODEFAULT),
    CFG_STR("password", NULL, CFGF_NONE),
    CFG_BOOL("raop_disable", cfg_false, CFGF_NONE),
    CFG_STR("nickname", NULL, CFGF_NONE),
    // Hidden options
    CFG_BOOL("exclusive", cfg_false, CFGF_NONE),
    CFG_END()
  };

/* FIFO section structure */
static cfg_opt_t sec_fifo[] =
  {
    CFG_STR("nickname", "fifo", CFGF_NONE),
    CFG_STR("path", NULL, CFGF_NONE),
    // Hidden options
    CFG_BOOL("exclusive", cfg_false, CFGF_NONE),
    CFG_END()
  };

/* MPD section structure */
static cfg_opt_t sec_mpd[] =
  {
    CFG_INT("port", 6600, CFGF_NONE),
    CFG_INT("http_port", 0, CFGF_NONE),
    CFG_BOOL("enable_httpd_plugin", cfg_false, CFGF_NONE),
    CFG_BOOL("clear_queue_on_stop_disable", cfg_false, CFGF_NODEFAULT | CFGF_DEPRECATED),
    CFG_BOOL("allow_modifying_stored_playlists", cfg_false, CFGF_NODEFAULT | CFGF_DEPRECATED),
    CFG_STR("default_playlist_directory", NULL, CFGF_NODEFAULT | CFGF_DEPRECATED),
    CFG_END()
  };

/* streaming section structure */
static cfg_opt_t sec_streaming[] =
  {
    CFG_INT("sample_rate", 44100, CFGF_NONE),
    CFG_INT("bit_rate", 192, CFGF_NONE),
    CFG_INT("icy_metaint", 16384, CFGF_NONE),
    // Hidden options
    CFG_BOOL("exclusive", cfg_false, CFGF_NONE),
    CFG_END()
  };

/* Config file structure */
static cfg_opt_t toplvl_cfg[] =
  {
    CFG_SEC("general", sec_general, CFGF_NONE),
    CFG_SEC("mass", sec_mass, CFGF_NONE),
    CFG_SEC("library", sec_library, CFGF_NONE),
    // CFG_SEC("audio", sec_audio, CFGF_NONE),
    // CFG_SEC("alsa", sec_alsa, CFGF_MULTI | CFGF_TITLE),
    CFG_SEC("airplay_shared", sec_airplay_shared, CFGF_NONE),
    CFG_SEC("airplay", sec_airplay, CFGF_MULTI | CFGF_TITLE),
    // CFG_SEC("chromecast", sec_chromecast, CFGF_MULTI | CFGF_TITLE),
    CFG_SEC("fifo", sec_fifo, CFGF_NONE),
    // CFG_SEC("rcp", sec_rcp, CFGF_MULTI | CFGF_TITLE),
    // CFG_SEC("spotify", sec_spotify, CFGF_NONE),
    // CFG_SEC("sqlite", sec_sqlite, CFGF_NONE),
    CFG_SEC("mpd", sec_mpd, CFGF_NONE),
    CFG_SEC("streaming", sec_streaming, CFGF_NONE),
    CFG_END()
  };

cfg_t *cfg;
uint64_t libhash;
uid_t runas_uid;
gid_t runas_gid;


static void
logger_confuse(cfg_t *config, const char *format, va_list args)
{
  char fmt[80];

  if (config && config->name && config->line)
    snprintf(fmt, sizeof(fmt), "[%s:%d] %s\n", config->name, config->line, format);
  else
    snprintf(fmt, sizeof(fmt), "%s\n", format);

  DVPRINTF(E_LOG, L_CONF, fmt, args);
}

static int
cb_loglevel(cfg_t *config, cfg_opt_t *opt, const char *value, void *result)
{
  if (strcasecmp(value, "fatal") == 0)
    *(long int *)result = E_FATAL;
  else if (strcasecmp(value, "log") == 0)
    *(long int *)result = E_LOG;
  else if (strcasecmp(value, "warning") == 0)
    *(long int *)result = E_WARN;
  else if (strcasecmp(value, "info") == 0)
    *(long int *)result = E_INFO;
  else if (strcasecmp(value, "debug") == 0)
    *(long int *)result = E_DBG;
  else if (strcasecmp(value, "spam") == 0)
    *(long int *)result = E_SPAM;
  else
    {
      DPRINTF(E_WARN, L_CONF, "Unrecognised loglevel '%s'\n", value);
      /* Default to warning */
      *(long int *)result = 1;
    }

  return 0;
}

int
conffile_load(char *file)
{
  int ret;

  cfg = cfg_init(toplvl_cfg, CFGF_NONE);

  cfg_set_error_function(cfg, logger_confuse);

  if (file) { // makes config file optional
    ret = cfg_parse(cfg, file);

    if (ret == CFG_FILE_ERROR)
      {
        DPRINTF(E_FATAL, L_CONF, "Could not open config file %s\n", file);

        goto out_fail;
      }
    else if (ret == CFG_PARSE_ERROR)
      {
        DPRINTF(E_FATAL, L_CONF, "Parse error in config file %s\n", file);

        goto out_fail;
      }
  }

  // Override defaults using values from cliap2 arguments
  if (ap2_device_info.latency_ms != 0) {
    DPRINTF(E_DBG, L_CONF, "%s:Overriding default start_buffer_ms from %ld ms to %" PRIu64 " ms\n",
      __func__,
      cfg_getint(cfg_getsec(cfg, "general"), "start_buffer_ms"),
      ap2_device_info.latency_ms
    );

    char *buf;
    asprintf(&buf, "general { start_buffer_ms = %" PRIu64 " }", ap2_device_info.latency_ms);
    cfg_parse_buf(cfg, buf);
    DPRINTF(E_DBG, L_CONF, "%s:Parsed \"%s\" to derive new start_buffer_ms value of %ld\n",
      __func__, buf, cfg_getint(cfg_getsec(cfg, "general"), "start_buffer_ms")
    );
    free(buf);
  }

  if (ap2_device_info.password) {
    char *buf;
    asprintf(&buf, "airplay \"%s\" { password = \"%s\" }", ap2_device_info.name, ap2_device_info.password);
    if (cfg_parse_buf(cfg, buf) != 0) {
      DPRINTF(E_LOG, L_CONF, "%s:Error setting password configuration with %s\n", __func__, buf);
      free(buf);
      goto out_fail;
    }
    free(buf);
  }

  return 0;

 out_fail:
  cfg_free(cfg);

  return -1;
}

void
conffile_unload(void)
{
  cfg_free(cfg);
}
