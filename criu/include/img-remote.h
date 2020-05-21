#include <limits.h>
#include <stdbool.h>

#include <stdint.h>
#include "common/list.h"
#include <pthread.h>
#include <semaphore.h>

#ifndef IMAGE_REMOTE_H
#define	IMAGE_REMOTE_H

#define FINISH 0
#define PARENT_IMG "parent"
#define NULL_SNAPSHOT_ID 0
#define DEFAULT_CACHE_SOCKET "img-cache.sock"
#define DEFAULT_PROXY_SOCKET "img-proxy.sock"

/* Called by restore to get the fd correspondent to a particular path.  This call
 * will block until the connection is received.
 */
int read_remote_image_connection(char *snapshot_id, char *path);

/* Called by dump to create a socket connection to the restore side. The socket
 * fd is returned for further writing operations.
 */
int write_remote_image_connection(char *snapshot_id, char *path, int flags);

/* Called by dump/restore when everything is dumped/restored. This function
 * creates a new connection with a special control name. The receiver side uses
 * it to ack that no more files are coming.
 */
int finish_remote_dump(void);
int finish_remote_restore(void);

/* Reads (discards) 'len' bytes from fd. This is used to emulate the function
 * lseek, which is used to advance the file needle.
 */
int skip_remote_bytes(int fd, unsigned long len);

/* To support iterative migration, the concept of snapshot_id is introduced
 * (only when remote migration is enabled). Each image is tagged with one
 * snapshot_id. The snapshot_id is the image directory used for the operation
 * that creates the image (either predump or dump). Images stored in memory
 * (both in Image Proxy and Image Cache) are identified by their name and
 * snapshot_id. Snapshot_ids are ordered so that we can find parent pagemaps
 * (that will be used when restoring the process).
 */

/* Sets the current snapshot_id */
void init_snapshot_id(char *ns);

/* Returns the snapshot_id index representing the current snapshot_id. This
 * index represents the hierarchy position. For example: images tagged with
 * the snapshot_id with index 1 are more recent than the images tagged with
 * the snapshot_id with index 0.
 */
int get_curr_snapshot_id_idx(void);

/* Returns the snapshot_id associated with the snapshot_id index. */
char *get_snapshot_id_from_idx(int idx);

/* Pushes the current snapshot_id into the snapshot_id hierarchy (into the Image
 * Proxy and Image Cache).
 */
int push_snapshot_id(void);

/* Returns the snapshot id index that precedes the current snapshot_id. */
int get_curr_parent_snapshot_id_idx(void);

#endif
