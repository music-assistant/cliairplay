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

#endif /* !__CLIAP2_H__ */