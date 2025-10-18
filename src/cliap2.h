#ifndef __CLIAP2_H__
#define __CLIAP2_H__

typedef struct ap2_device_info
{
  const char *name;
  const char *type;
  const char *domain;
  const char *hostname;
  int family;
  const char *address;
  int port;
  struct keyval *txt;
} ap2_device_info_t;

#endif /* !__CLIAP2_H__ */