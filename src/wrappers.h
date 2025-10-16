#ifndef __WRAPPERS_H__
#define __WRAPPERS_H__

#include "misc.h"

/*
 * Wrappers for db.c
 */
struct db_queue_item *
db_queue_fetch_next(uint32_t, char);

int
db_perthread_init(void);

void
db_perthread_deinit(void);


/*
 * Wrappers for mdns.c
 */
enum mdns_options
{
  // Test connection to device and only call back if successful
  MDNS_CONNECTION_TEST = (1 << 1),
  // Only browse for ipv4 services
  MDNS_IPV4ONLY        = (1 << 2),
};

typedef void (* mdns_browse_cb)(const char *name, const char *type, const char *domain, const char *hostname, int family, const char *address, int port, struct keyval *txt);

/*
 * Start a service browser, a callback will be made when the service changes state
 * Call only from the main thread!
 *
 * @in  type     Type of service to look for, e.g. "_raop._tcp"
 * @in  flags    See mdns_options (only supported by Avahi implementation)
 * @in  cb       Callback when service state changes (e.g. appears/disappears)
 * @return       0 on success, -1 on error
 */
int
mdns_browse(char *type, mdns_browse_cb cb, enum mdns_options flags);

#endif /* !__WRAPPERS_H__ */