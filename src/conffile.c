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

#include <confuse.h>

#include "logger.h"
#include "misc.h"
#include "conffile.h"


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
    CFG_BOOL("speaker_autoselect", cfg_false, CFGF_NONE),
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
    CFG_END()
  };

/* Music Assistant section structure */
static cfg_opt_t sec_mass[] =
  {
    CFG_BOOL("autostart", cfg_true, CFGF_NONE),
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
    CFG_END()
  };

/* FIFO section structure */
static cfg_opt_t sec_fifo[] =
  {
    CFG_STR("nickname", "fifo", CFGF_NONE),
    CFG_STR("path", NULL, CFGF_NONE),
    CFG_END()
  };

/* streaming section structure */
static cfg_opt_t sec_streaming[] =
  {
    CFG_INT("sample_rate", 44100, CFGF_NONE),
    CFG_INT("bit_rate", 192, CFGF_NONE),
    CFG_INT("icy_metaint", 16384, CFGF_NONE),
    CFG_END()
  };

/* Config file structure */
static cfg_opt_t toplvl_cfg[] =
  {
    CFG_SEC("general", sec_general, CFGF_NONE),
    CFG_SEC("mass", sec_mass, CFGF_NONE),
    // CFG_SEC("library", sec_library, CFGF_NONE),
    // CFG_SEC("audio", sec_audio, CFGF_NONE),
    // CFG_SEC("alsa", sec_alsa, CFGF_MULTI | CFGF_TITLE),
    CFG_SEC("airplay_shared", sec_airplay_shared, CFGF_NONE),
    CFG_SEC("airplay", sec_airplay, CFGF_MULTI | CFGF_TITLE),
    // CFG_SEC("chromecast", sec_chromecast, CFGF_MULTI | CFGF_TITLE),
    CFG_SEC("fifo", sec_fifo, CFGF_NONE),
    // CFG_SEC("rcp", sec_rcp, CFGF_MULTI | CFGF_TITLE),
    // CFG_SEC("spotify", sec_spotify, CFGF_NONE),
    // CFG_SEC("sqlite", sec_sqlite, CFGF_NONE),
    // CFG_SEC("mpd", sec_mpd, CFGF_NONE),
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

// Makes sure cache_dir ends with a slash
static int
sanitize_cache_dir(cfg_t *general)
{
  char *dir;
  const char *s;
  char *appended;
  size_t len;

  dir = cfg_getstr(general, "cache_dir");
  len = strlen(dir);

  s = strrchr(dir, '/');
  if (s && (s + 1 == dir + len))
    return 0;

  appended = safe_asprintf("%s/", dir);

  cfg_setstr(general, "cache_dir", appended);

  free(appended);

  return 0;
}

static int
conffile_expand_libname(cfg_t *lib)
{
  char *libname;
  char *hostname;
  char *s;
  char *d;
  char *expanded;
  struct utsname sysinfo;
  size_t len;
  size_t olen;
  size_t hostlen;
  size_t verlen;
  int ret;

  libname = cfg_getstr(lib, "name");
  olen = strlen(libname);

  /* Fast path */
  s = strchr(libname, '%');
  if (!s)
    {
      libhash = murmur_hash64(libname, olen, 0);
      return 0;
    }

  /* Grab what we need */
  ret = uname(&sysinfo);
  if (ret != 0)
    {
      DPRINTF(E_WARN, L_CONF, "Could not get system name: %s\n", strerror(errno));
      hostname = "Unknown host";
    }
  else
    hostname = sysinfo.nodename;

  hostlen = strlen(hostname);
  verlen = strlen(VERSION);

  /* Compute expanded size */
  len = olen;
  s = libname;
  while (*s)
    {
      if (*s == '%')
	{
	  s++;

	  switch (*s)
	    {
	      case 'h':
		len += hostlen;
		break;

	      case 'v':
		len += verlen;
		break;
	    }
	}
      s++;
    }

  expanded = (char *)malloc(len + 1);
  if (!expanded)
    {
      DPRINTF(E_FATAL, L_CONF, "Out of memory\n");

      return -1;
    }
  memset(expanded, 0, len + 1);

  /* Do the actual expansion */
  s = libname;
  d = expanded;
  while (*s)
    {
      if (*s == '%')
	{
	  s++;

	  switch (*s)
	    {
	      case 'h':
		strcat(d, hostname);
		d += hostlen;
		break;

	      case 'v':
		strcat(d, VERSION);
		d += verlen;
		break;
	    }

	  s++;
	}
      else
	{
	  *d = *s;

	  s++;
	  d++;
	}
    }

  cfg_setstr(lib, "name", expanded);

  libhash = murmur_hash64(expanded, strlen(expanded), 0);

  free(expanded);

  return 0;
}

int
conffile_load(char *file)
{
  cfg_t *lib;
  struct passwd *pw;
  char *runas;
  int ret;

  cfg = cfg_init(toplvl_cfg, CFGF_NONE);

  cfg_set_error_function(cfg, logger_confuse);

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
