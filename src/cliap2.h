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
  const char *auth_key;
  uint64_t ntpstart;
  uint32_t wait;
  struct timespec start_ts;
  uint32_t latency;
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