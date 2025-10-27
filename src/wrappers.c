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

#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "wrappers.h"
#include "cliap2.h"

#include "logger.h"
#include "outputs.h"
#include "db.h"
#include "conffile.h"

#define AIRPLAY_SERVICE_TYPE "_airplay._tcp"

extern ap2_device_info_t ap2_device_info;
extern char* gnamed_pipe;

/*
 * Wrappers for db.c
 * We need to make these functions do something, because the data returned by them
 * controls the behaviour of the calling functions.
 */

// Emulate the db_queue database table with a memory bound linked list
// Almost certainly this will have at most one member.
typedef struct db_queue {
    struct db_queue_item item;
    struct db_queue *next;
} db_queue_t;

db_queue_t *queue = NULL; // Always points to the head

// Function to create a new node - it still needs to be placed in the linked list
// after creation
static struct
db_queue* db_queue_create_node(struct db_queue_item *data) {
    db_queue_t* newNode = (db_queue_t*)calloc(1, sizeof(db_queue_t));
    if (newNode == NULL) {
        DPRINTF(E_FATAL, L_DB, "db_queue_create_node():queue_node:Memory allocation failed\n");
        return (db_queue_t *)NULL;
    }
    newNode->item.id = data->id;
    newNode->item.file_id = data->file_id;
    newNode->item.pos = data->pos;
    newNode->item.shuffle_pos = data->shuffle_pos;
    newNode->item.data_kind = data->data_kind;
    newNode->item.media_kind = data->media_kind;
    newNode->item.path = data->path;
    newNode->item.bitrate = data->bitrate;
    newNode->item.samplerate = data->samplerate;
    newNode->item.channels = data->channels;
    newNode->next = NULL;
    return newNode;
}

static struct
db_queue* db_queue_insert_atend(struct db_queue_item *data) {
    db_queue_t* newNode = db_queue_create_node(data);
    if (queue == NULL) {
        // queue is empty, new node is the head
        queue = newNode;
        return newNode; // If list is empty, new node is the head
    }
    db_queue_t* current = queue;
    while (current->next != NULL) {
        current = current->next;
    }
    current->next = newNode;
    return queue;
}

struct db_queue_item *
db_queue_fetch_byitemid(uint32_t item_id)
{
    struct db_queue_item *ret = NULL;
    db_queue_t *q = queue; // start at the head

    while (q) {
        if (q->item.id == item_id) {
            ret = &q->item;
            break;
        }
        q = q->next;
    }
    return ret; 
}

struct db_queue_item *
db_queue_fetch_next(uint32_t item_id, char shuffle)
{
    struct db_queue_item *ret = NULL;

    DPRINTF(E_LOG, L_DB, "db_queue_fetch_next() not yet fully implemented.\n");
    return ret; 
}

struct db_queue_item *
db_queue_fetch_prev(uint32_t item_id, char shuffle)
{
    struct db_queue_item *ret = NULL;

    DPRINTF(E_LOG, L_DB, "db_queue_fetch_prev() not yet fully implemented.\n");
    return ret; 
}

struct db_queue_item *
db_queue_fetch_bypos(uint32_t pos, char shuffle)
{
    struct db_queue_item *ret = NULL;

    DPRINTF(E_LOG, L_DB, "db_queue_fetch_bypos() not yet fully implemented.\n");
    return ret; 
}

int
db_queue_reshuffle(uint32_t item_id)
{

    DPRINTF(E_LOG, L_DB, "db_queue_reshuffle() net yet fully implemented.\n");
    return 0;
}

int
db_queue_inc_version(void)
{

    DPRINTF(E_LOG, L_DB, "db_queue_inc_version() not yet fully implemented.\n");
    return 0;
}

int
db_queue_delete_byitemid(uint32_t item_id)
{

    DPRINTF(E_DBG, L_DB, "%s(%d)\n", __func__, item_id);

    // find the queue item, then remove it from the linked list and dealloc
    if (queue == NULL) // queue already empty
        return 0;
    
    db_queue_t *prev_node = NULL;
    db_queue_t *node = queue;
    for (node = queue; node->next; node = node->next) {
        // traverse the queue
        if (node->item.id == item_id) {
            // we found the node to delete
            if (prev_node) {
                // remove node from the linked list
                prev_node->next = node->next;
                DPRINTF(E_DBG, L_DB, "%s:Removed node with item id %d from the queue\n", __func__, node->item.id);
                // free memory alloced for node and break from loop
                free(node);
                DPRINTF(E_DBG, L_DB, "%s:Free'd memory for node with item id %d from the queue\n", __func__, item_id);
                break;
            }
        }
        prev_node = node;
    }
    return 0;
}

/*
 * Removes all items from the queue except the item give by 'keep_item_id' (if 'keep_item_id' > 0).
 *
 * @param keep_item_id item-id (e. g. the now playing item) to be left in the queue
 */
int
db_queue_clear(uint32_t keep_item_id)
{
    if (queue == NULL) // queue already empty
        return 0;
    
    db_queue_t *temp = queue;
    db_queue_t *keep = NULL;
    while (temp) {
        db_queue_t *next = temp->next;
        if (temp->item.id == keep_item_id)
            keep = temp;
        else
            free(temp);
        temp = next;
    }
    if (keep)
        queue = keep;
    else
        queue = NULL; // we deleted all items in the queue

    return 0;
}

int
db_queue_item_update(struct db_queue_item *qi)
{

    DPRINTF(E_LOG, L_DB, "db_queue_item_update() not yet fully implemented.\n");
    return 0;
}

/*
 * Adds the files matching the given query to the queue
 *
 * Music Assistant:
 * Adds the file to the local memory "normal" queue
 * 
 * Owntones:
 * The files table is queried with the given parameters and all found files are added to the end of the
 * "normal" queue and the shuffled queue.
 *
 * The function returns -1 on failure (e. g. error reading from database). It wraps all database access
 * in a transaction and performs a rollback if an error occurs, leaving the queue in a consistent state.
 *
 * @param qp Query parameters for the files table
 * @param reshuffle If 1 queue will be reshuffled after adding new items
 * @param item_id The base item id, all items after this will be reshuffled
 * @param position The position in the queue for the new queue item, -1 to add at end of queue
 * @param count If not NULL returns the number of items added to the queue
 * @param new_item_id If not NULL return the queue item id of the first new queue item
 * @return 0 on success, -1 on failure
 */
int
db_queue_add_by_query(struct query_params *qp, char reshuffle, uint32_t item_id, int position, int *count, int *new_item_id)
{
    if (qp->type == Q_ITEMS) {
        DPRINTF(E_DBG, L_DB, "Q_ITEMS. reshuffle:%c, item_id:%d\n", reshuffle, item_id);
        if (position == -1) {
            // Add to end of queue
             struct db_queue_item *item = (struct db_queue_item *)calloc(1, sizeof(struct db_queue_item));
            if (item == NULL) {
                DPRINTF(E_FATAL, L_DB, "%s():Memory allocation failed\n", __func__);
                return -1;
            }
            // put some data in the queue_item first!!
            item->id = item_id + 1;
            item->file_id = 1; // We only expect one item in the queue, so make them all the same
            item->pos = 1;
            item->shuffle_pos = 1;
            item->data_kind = DATA_KIND_PIPE; // this is all we support for the moment
            item->media_kind = MEDIA_KIND_MUSIC; // we only support audio
            item->path = gnamed_pipe;
            item->bitrate = 0; // I don't think value matters for us.
            item->samplerate = cfg_getint(cfg_getsec(cfg, "mass"), "pcm_sample_rate");
            item->channels = 2;
            if (db_queue_insert_atend(item)) {
                if (count) *count = 1;
                if (new_item_id) *new_item_id = item->id;
                return 0;
            }
            else {
                return -1;
            }
        }
        else {
            DPRINTF(E_LOG, L_DB, "%s(). Position %d not yet supported.\n", __func__, position);
            return -1;
        }
    }
    return 0;
}

void
db_file_seek_update(int id, uint32_t seek)
{

    DPRINTF(E_LOG, L_DB, "db_file_seek_update() net yet fully implemented.\n");
    return;
}

struct media_file_info *
db_file_fetch_byid(int id)
{
    struct media_file_info *ret = NULL;

    // This function gets called at time of playback start up to obtain information about
    // where to start playback (in milliseconds) from the seek field of the media_file_info struct.
    return ret; 
}

void
db_file_inc_skipcount(int id)
{

    DPRINTF(E_LOG, L_DB, "db_file_inc_skipcount() not yet fully implemented.\n");
    return;
}

void
db_file_inc_playcount(int id)
{
    DPRINTF(E_LOG, L_DB, "db_file_inc_playcount() not yet fully implemented.\n");
    return;
}

int
db_perthread_init(void)
{
    // It has been tested that it is ok to do nothing
    return 0;
}

// Free memory allocated for our in-memory db items
void
db_deinit(void)
{
    db_queue_clear(0);
    return;
}

void
db_perthread_deinit(void) 
{
    // It has been tested that it is ok to do nothing
    return;
}

int
db_speaker_save(struct output_device *device)
{
    // It has been tested that it is ok to do nothing
    return 0;
}

int
db_speaker_get(struct output_device *device, uint64_t id)
{
    device->id = id;
    device->selected = 1;
    device->volume = ap2_device_info.volume;
    //device->auth_key = ?? // might need to pass this as command line argument for some devices
    device->selected_format = MEDIA_FORMAT_ALAC;

    return 0;
}

int
db_query_start(struct query_params *qp)
{
    DPRINTF(E_LOG, L_DB, "db_query_start() not yet fully implemented.\n");
    return 0;
}

void
db_query_end(struct query_params *qp)
{
    DPRINTF(E_LOG, L_DB, "db_query_end() not yet fully implemented.\n");
    return;
}

int
db_query_fetch_file(struct db_media_file_info *dbmfi, struct query_params *qp)
{

    DPRINTF(E_LOG, L_DB, "db_query_fetch_file() not yet fully implemented.\n");
    return 0;
}

void
free_mfi(struct media_file_info *mfi, int content_only)
{

    DPRINTF(E_LOG, L_DB, "free_mfi() not yet fully implemented.\n");
    return;
}

void
free_queue_item(struct db_queue_item *qi, int content_only)
{
    // owntones behaviour is to free the memory allocated for elements of the queue item
    // that were malloc'ed on creation, and then to free the memory alloc'ed for the queue item
    // if content_only == 0.
    // For mass, we don't need to free any content, because we never malloc'ed for any content
    // but let's re-evaluate once metadata is implemented.
    // If content_only == 0, then remove the item from the queue.

    if (!content_only) {
        DPRINTF(E_DBG, L_DB, "%s:Removing item %d from the queue\n", __func__, qi->id);
        db_queue_delete_byitemid(qi->id);
    }
    return;
}

/*
 * Wrappers for mdns.c
 */
int
mdns_browse(char *type, mdns_browse_cb cb, enum mdns_options flags)
{
    if (!strncmp(AIRPLAY_SERVICE_TYPE, type, strlen(AIRPLAY_SERVICE_TYPE))) {
        cb(ap2_device_info.name,
           AIRPLAY_SERVICE_TYPE,
           "local",
           ap2_device_info.hostname,
           AF_INET,
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