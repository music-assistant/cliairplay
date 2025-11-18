#ifndef __CLIAP2_H__
#define __CLIAP2_H__

#define METADATA_NAMED_PIPE_DEFAULT_SUFFIX ".metadata"

typedef struct ap2_device_info
{
  const char *name;
  const char *hostname;
  const char *address;
  int port;
  struct keyval *txt;
  char pin[5];
  const char *auth_key;
  struct timespec start_ts; // if non-zero, the time for commencement of playback of first packet in OwnTone time basis (i.e. CLOCK_REALTIME)
  int volume;
} ap2_device_info_t;

typedef struct mass_named_pipes
{
  char *audio_pipe; // receives raw pcm audio to be streamed
  char *metadata_pipe; // receives metadata and commands
} mass_named_pipes_t;

/* NTP timestamp definitions */
#define FRAC             4294967296. /* 2^32 as a double */
#define NTP_EPOCH_DELTA  0x83aa7e80  /* 2208988800 - that's 1970 - 1900 in seconds */

#endif /* !__CLIAP2_H__ */