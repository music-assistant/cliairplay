#ifndef __CLIAP2_H__
#define __CLIAP2_H__

#define METADATA_NAMED_PIPE_DEFAULT_SUFFIX ".metadata"

/*
 * Below explanation is from libraop raop_client.h
 *
 * RAOP players have a latency which is usually 11025 frames.
 *
 * The precise time at the DAC is the time at the client plus the latency, so when
 * setting a start time, we must anticipate by the latency if we want the first
 * frame to be *exactly* played at that NTP value.
 * 
 * For Music Assistant, we will simply subtract 250ms (11025 frames at 44100 sample rate)
 * This approach may need to change when we implement higher quality streams
 */
#define DAC_LATENCY_TS {0, 250e6}
#define DAC_LATENCY_MS 250

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
  struct timespec session_establishment_latency_ts; // anticipated duration of the RTSP session establishment process
  struct timespec process_started_ts;  // the time the process started in OwnTone time basis
} ap2_device_info_t;

typedef struct mass_named_pipes
{
  char *audio_pipe; // receives raw pcm audio to be streamed
  char *metadata_pipe; // receives metadata and commands
} mass_named_pipes_t;

/* NTP timestamp definitions */
#define FRAC             4294967296. /* 2^32 as a double */
#define NTP_EPOCH_DELTA  0x83aa7e80  /* 2208988800 - that's 1970 - 1900 in seconds */

uint64_t get_output_buffer_ms(void);
void get_output_buffer_ts(struct timespec *ts);

#endif /* !__CLIAP2_H__ */