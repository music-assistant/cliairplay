#ifndef __CLIAP2_H__
#define __CLIAP2_H__

typedef struct ap2_device_info
{
  const char *name;
  const char *hostname;
  const char *address;
  int port;
  struct keyval *txt;
  uint64_t ntpstart;
  uint32_t wait;
  struct timespec start_ts;
  uint32_t latency;
  int volume;
} ap2_device_info_t;

#endif /* !__CLIAP2_H__ */