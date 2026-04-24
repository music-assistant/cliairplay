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
  char *auth_key;
  char *password; // unencryptd device password
  int volume; // initial volume
  struct timespec start_ts; // if non-zero, the time for commencement of playback of first packet in OwnTone time basis (i.e. CLOCK_MONOTONIC)
  uint64_t latency_ms; // output buffer duration in milliseconds, inclusive of DAC latency
  int64_t input_write_ms; // Number of milliseconds margin to use to determine timing of initial call to input_write(). Can be negative
  struct timespec pairing_latency; // anticipated duration of the RTSP pairing & session establishment process
} ap2_device_info_t;

typedef struct mass_named_pipes
{
  char *audio_pipe; // receives raw pcm audio to be streamed
  char *metadata_pipe; // receives metadata and commands
} mass_named_pipes_t;

/* NTP timestamp definitions */
#define FRAC             4294967296. /* 2^32 as a double */
#define NTP_EPOCH_DELTA  0x83aa7e80  /* 2208988800 - that's 1970 - 1900 in seconds */

void get_output_buffer_ts(struct timespec *ts);

#endif /* !__CLIAP2_H__ */