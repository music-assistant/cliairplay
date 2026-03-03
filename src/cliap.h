#ifndef __CLIAP_H__
#define __CLIAP_H__

#include "misc.h"

#define METADATA_NAMED_PIPE_DEFAULT_SUFFIX ".metadata"

typedef enum Version {
  RAOP = 1,
  AIRPLAY2
} Version_t;

typedef struct ap_device_info
{
  const char *name;
  const char *hostname;
  const char *address;
  int port;
  Version_t version;
  struct keyval *txt;
  char pin[5];
  char *auth_key;
  struct timespec start_ts; // if non-zero, the time for commencement of playback of first packet in OwnTone time basis (i.e. CLOCK_MONOTONIC)
  int volume;
  uint64_t latency_ms; // output buffer duration, inclusive of DAC latency
  char *password; // unencryptd device password
  struct media_quality quality; // quality of audio output
} ap_device_info_t;

typedef struct mass_named_pipes
{
  char *audio_pipe; // receives raw pcm audio to be streamed
  char *metadata_pipe; // receives metadata and commands
} mass_named_pipes_t;

/* NTP timestamp definitions */
#define FRAC             4294967296. /* 2^32 as a double */
#define NTP_EPOCH_DELTA  0x83aa7e80  /* 2208988800 - that's 1970 - 1900 in seconds */

#endif /* !__CLIAP_H__ */