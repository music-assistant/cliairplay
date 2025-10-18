/*
 * Wrapper functions to emulate owntones functions that are not required for 
 * cliap2. 
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <string.h>

#include "wrappers.h"
#include "cliap2.h"

#include "logger.h"
#include "db.h"
#include "outputs.h"
#include "listener.h"

extern ap2_device_info_t ap2_device_info;

/*
 * Wrappers for db.c
 */
struct db_queue_item *
db_queue_fetch_byitemid(uint32_t item_id)
{
    struct db_queue_item *ret = NULL;
    return ret; 
}

struct db_queue_item *
db_queue_fetch_next(uint32_t item_id, char shuffle)
{
    struct db_queue_item *ret = NULL;
    return ret; 
}

struct db_queue_item *
db_queue_fetch_prev(uint32_t item_id, char shuffle)
{
    struct db_queue_item *ret = NULL;
    return ret; 
}

struct db_queue_item *
db_queue_fetch_bypos(uint32_t pos, char shuffle)
{
    struct db_queue_item *ret = NULL;
    return ret; 
}

int
db_queue_reshuffle(uint32_t item_id)
{
    return 0;
}

int
db_queue_inc_version(void)
{
    return 0;
}

int
db_queue_delete_byitemid(uint32_t item_id)
{
    return 0;
}

int
db_queue_clear(uint32_t keep_item_id)
{
    return 0;
}

int
db_queue_item_update(struct db_queue_item *qi)
{
    return 0;
}

int
db_queue_add_by_query(struct query_params *qp, char reshuffle, uint32_t item_id, int position, int *count, int *new_item_id)
{
    return 0;
}

void
db_file_seek_update(int id, uint32_t seek)
{
    return;
}

struct media_file_info *
db_file_fetch_byid(int id)
{
    struct media_file_info *ret = NULL;
    return ret; 
}

void
db_file_inc_skipcount(int id)
{
    return;
}

void
db_file_inc_playcount(int id)
{
    return;
}

int
db_perthread_init(void)
{
    return 0;
}

void
db_perthread_deinit(void) {
    return;
}

int
db_speaker_save(struct output_device *device)
{
    return 0;
}

int
db_speaker_get(struct output_device *device, uint64_t id)
{
    return 0;
}

int
db_query_start(struct query_params *qp)
{
    return 0;
}

void
db_query_end(struct query_params *qp)
{
    return;
}

int
db_query_fetch_file(struct db_media_file_info *dbmfi, struct query_params *qp)
{
    return 0;
}

void
free_mfi(struct media_file_info *mfi, int content_only)
{
    return;
}

void
free_queue_item(struct db_queue_item *qi, int content_only)
{
    return;
}

/*
 * Wrappers for mdns.c
 */
int
mdns_browse(char *type, mdns_browse_cb cb, enum mdns_options flags)
{
    if (!strncmp("_airplay._tcp", type, strlen("_airplay._tcp"))) {
        DPRINTF(E_DBG, L_MAIN, "mdns_browse called for %s\n", type);
        DPRINTF(E_DBG, L_MAIN, 
            "Our airplay device info: name=%s, type=%s, domain=%s, hostname=%s, family=%d, address=%s, port=%d\n",
            ap2_device_info.name,
            ap2_device_info.type,
            ap2_device_info.domain,
            ap2_device_info.hostname,
            ap2_device_info.family,
            ap2_device_info.address,
            ap2_device_info.port);
        DPRINTF(E_DBG, L_MAIN, "Head name:%s, value:%s\n", 
            ap2_device_info.txt->head->name, 
            ap2_device_info.txt->head->value);
            
        cb(ap2_device_info.name,
           ap2_device_info.type,
           ap2_device_info.domain,
           ap2_device_info.hostname,
           ap2_device_info.family,
           ap2_device_info.address,
           ap2_device_info.port,
           ap2_device_info.txt);
    }
    return 0;
}

/*
 * Wrappers for settings.c
 */
struct settings_category *
settings_category_get(const char *name)
{
    struct settings_category *ret = NULL;
    return ret; 
}

struct settings_option *
settings_option_get(struct settings_category *category, const char *name)
{
    struct settings_option *ret = NULL;
    return ret; 
}

int
settings_option_getint(struct settings_option *option)
{
    return 0;
}

bool
settings_option_getbool(struct settings_option *option)
{
    return false;
}

char *
settings_option_getstr(struct settings_option *option)
{
    char *ret = NULL;
    return ret; 
}

int
settings_option_setint(struct settings_option *option, int value)
{
    return 0;
}

int
settings_option_setbool(struct settings_option *option, bool value)
{
    return 0;
}

/*
 * Wrappers for listener.c
 */
int
listener_add(notify notify_cb, short event_mask, void *ctx)
{
    return 0;
}

int
listener_remove(notify notify_cb)
{
    return 0;
}

void
listener_notify(short event_mask)
{
    return;
}

/*
 * Wrappers for listenbrainz.c
 */
int
listenbrainz_scrobble(int mfi_id)
{
    return 0;
}

/*
 * Wrappers for unused output_definition structs
 */
static int 
output_wrapper_init(void) {
    return 0;
}

static void 
output_wrapper_deinit(void) {
    return;
}

static int
output_wrapper_device_start(struct output_device *device, int callback_id)
{
    return 1;
}

static int
output_wrapper_device_stop(struct output_device *device, int callback_id) {
    return 1;
}

static int
output_wrapper_device_flush(struct output_device *device, int callback_id) {
    return 1;
}

static int
output_wrapper_device_probe(struct output_device *device, int callback_id) {
    return 1;
}

static void
output_wrapper_device_cb_set(struct output_device *device, int callback_id) {
    return;
}

static void
output_wrapper_device_free_extra(struct output_device *device) {
    return;
}

static int
output_wrapper_set_volume_one(struct output_device *device, int callback_id) {
    return 1;
}

static int
output_wrapper_volume_to_pct(struct output_device *device, const char *volume) {
    return 50;
}

static void
output_wrapper_write(struct output_buffer *buffer) {
    return;
}

static void *
output_wrapper_metadata_prepare(struct output_metadata *metadata) {
    return NULL;
}

static void
output_wrapper_metadata_send(struct output_metadata *metadata) {
    return;
}

static void
output_wrapper_metadata_purge(void) {
    return;
}

static int
output_wrapper_device_authorize(struct output_device *device, const char *pin, int callback_id) {
    return 1;
}


struct output_definition output_raop =
{
  .name = "AirPlay 1",
  .type = OUTPUT_TYPE_RAOP,
#ifdef PREFER_AIRPLAY2
  .priority = 2,
#else
  .priority = 1,
#endif
  .disabled = 1,
  .init = output_wrapper_init,
  .deinit = output_wrapper_deinit,
  .device_start = output_wrapper_device_start,
  .device_stop = output_wrapper_device_stop,
  .device_flush = output_wrapper_device_flush,
  .device_probe = output_wrapper_device_probe,
  .device_cb_set = output_wrapper_device_cb_set,
  .device_free_extra = output_wrapper_device_free_extra,
  .device_volume_set = output_wrapper_set_volume_one,
  .device_volume_to_pct = output_wrapper_volume_to_pct,
  .write = output_wrapper_write,
  .metadata_prepare = output_wrapper_metadata_prepare,
  .metadata_send = output_wrapper_metadata_send,
  .metadata_purge = output_wrapper_metadata_purge,
  .device_authorize = output_wrapper_device_authorize,
};

struct output_definition output_streaming =
{
  .name = "streaming",
  .type = OUTPUT_TYPE_STREAMING,
  .priority = 0,
  .disabled = 1,
  .init = output_wrapper_init,
  .deinit = output_wrapper_deinit,
  .write = output_wrapper_write,
  .device_start = output_wrapper_device_start,
  .device_probe = output_wrapper_device_probe,
  .device_stop = output_wrapper_device_stop,
  .metadata_prepare = output_wrapper_metadata_prepare,
  .metadata_send = output_wrapper_metadata_send,
};

struct output_definition output_dummy =
{
  .name = "dummy",
  .type = OUTPUT_TYPE_DUMMY,
  .priority = 99,
  .disabled = 1,
  .init = output_wrapper_init,
  .deinit = output_wrapper_deinit,
  .device_start = output_wrapper_device_start,
  .device_stop = output_wrapper_device_stop,
  .device_flush = output_wrapper_device_flush,
  .device_probe = output_wrapper_device_probe,
  .device_volume_set = output_wrapper_set_volume_one,
  .device_authorize = output_wrapper_device_authorize,
  .device_cb_set = output_wrapper_device_cb_set,
};

struct output_definition output_fifo =
{
  .name = "fifo",
  .type = OUTPUT_TYPE_FIFO,
  .priority = 98,
  .disabled = 1,
  .init = output_wrapper_init,
  .deinit = output_wrapper_deinit,
  .device_start = output_wrapper_device_start,
  .device_stop = output_wrapper_device_stop,
  .device_flush = output_wrapper_device_flush,
  .device_probe = output_wrapper_device_probe,
  .device_volume_set = output_wrapper_set_volume_one,
  .device_cb_set = output_wrapper_device_cb_set,
  .write = output_wrapper_write,
};

struct output_definition output_rcp =
{
  .name = "RCP/SoundBridge",
  .type = OUTPUT_TYPE_RCP,
  .priority = 99,
  .disabled = 1,
  .init = output_wrapper_init,
  .deinit = output_wrapper_deinit,
  .device_start = output_wrapper_device_start,
  .device_stop = output_wrapper_device_stop,
  .device_flush = output_wrapper_device_flush,
  .device_probe = output_wrapper_device_probe,
  .device_volume_set = output_wrapper_set_volume_one,
  .device_cb_set = output_wrapper_device_cb_set,
};

/*
 * Wrappers for artwork.c
 */
/*
 * Get the artwork image for an individual item (track)
 *
 * @out evbuf    Event buffer that will contain the (scaled) image
 * @in  id       The mfi item id
 * @in  max_w    Requested maximum image width (may not be obeyed)
 * @in  max_h    Requested maximum image height (may not be obeyed)
 * @in  format   Requested format (may not be obeyed), 0 for default
 * @return       ART_FMT_* on success, -1 on error or no artwork found
 */
int
artwork_get_item(struct evbuffer *evbuf, int id, int max_w, int max_h, int format)
{
    return -1;
}

bool
artwork_extension_is_artwork(const char *path)
{
    return false;
}

/*
 * Wrappers for dmap_common.c
 */
int
dmap_encode_queue_metadata(struct evbuffer *songlist, struct evbuffer *song, struct db_queue_item *queue_item)
{
    return -1;
}