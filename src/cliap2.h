#ifndef __CLIAP2_H__
#define __CLIAP2_H__

#include <stdint.h>
#include <time.h>

// NTP timing macros (from cliraop/libraop)
// Convert milliseconds to NTP 64-bit timestamp format
#define MS2NTP(ms) (((((uint64_t)(ms)) << 22) / 1000) << 10)
// Convert NTP 64-bit timestamp to milliseconds
#define NTP2MS(ntp) ((((ntp) >> 10) * 1000L) >> 22)
// Convert timestamp (samples) to NTP format given sample rate
#define TS2NTP(ts, rate) (((((uint64_t)(ts)) << 16) / (rate)) << 16)
// Convert NTP timestamp to samples given sample rate
#define NTP2TS(ntp, rate) ((((ntp) >> 16) * (rate)) >> 16)
// Extract NTP seconds and fraction for logging
#define RAOP_SEC(ntp) ((uint32_t)((ntp) >> 32))
#define RAOP_FRAC(ntp) ((uint32_t)(ntp))

// NTP epoch is 1900-01-01, Unix epoch is 1970-01-01
// Difference in seconds: 2208988800
#define NTP_EPOCH_DELTA 0x83aa7e80

typedef struct ap2_device_info
{
  const char *name;
  const char *hostname;
  const char *address;
  int port;
  struct keyval *txt;
  uint64_t ntpstart;
  uint32_t wait;
  uint32_t latency;
  int volume;
} ap2_device_info_t;

// Global device info (set from command line in cliap2.c)
extern ap2_device_info_t ap2_device_info;

// Global audio configuration
extern int g_sample_rate;
extern int g_bits_per_sample;
extern int g_use_alac;  // 1 = ALAC encoding, 0 = raw PCM
extern int g_alac_24bit;  // 1 = use 24-bit ALAC, 0 = use 16-bit ALAC

// Global RTP start position (calculated from NTP start time)
// Set by mass.c during initialization, used by airplay.c when creating rtp_session
extern uint32_t g_rtp_start_pos;

// NTP timing utility function
// Returns current time as NTP timestamp (64-bit: upper 32 bits = seconds since 1900, lower 32 bits = fraction)
uint64_t cliap2_get_ntp(void);

#endif /* !__CLIAP2_H__ */