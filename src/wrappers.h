#ifndef __WRAPPERS_H__
#define __WRAPPERS_H__

#include "misc.h"
#include "cliap2.h"

/*
 * Wrappers for db.c
 */
struct db_queue_item *
db_queue_fetch_next(uint32_t, char);

int
db_perthread_init(void);

void
db_perthread_deinit(void);

// const char *
// db_scan_kind_label(enum scan_kind scan_kind);

// int
// db_admin_setint64(const char *key, int64_t value);

// void
// db_purge_all(void);

// int
// db_file_update(struct media_file_info *mfi);

// void
// db_purge_cruft_bysource(time_t ref, enum scan_kind scan_kind);

/*
 * Wrappers for artwork.c
 */

/*
 * An artwork source handler must return one of the following:
 *
 *   ART_FMT_XXXX (positive)  An image, see possible formats in artwork.h
 *   ART_E_NONE (zero)        No artwork found
 *   ART_E_ERROR (negative)   An error occurred while searching for artwork
 *   ART_E_ABORT (negative)   Caller should abort artwork search (may be returned by cache)
 */
#define ART_E_NONE 0
#define ART_E_ERROR -1
#define ART_E_ABORT -2

int artwork_read_byurl(struct evbuffer *evbuf, const char *url);

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

int
mdns_browse(char *type, mdns_browse_cb cb, enum mdns_options flags);

#endif /* !__WRAPPERS_H__ */