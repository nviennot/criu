#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/un.h>
#include <unistd.h>

#include "cr_options.h"
#include "img-remote.h"
#include "image.h"
#include "images/remote-image.pb-c.h"
#include "protobuf.h"
#include "servicefd.h"
#include "string.h"
#include "xmalloc.h"
#include "common/scm.h"

#define EPOLL_MAX_EVENTS 50

#define strflags(f) ((f) == O_RDONLY ? "read" : \
		     (f) == O_APPEND ? "append" : "write")

// List of snapshots (useful when doing incremental restores/dumps)
static LIST_HEAD(snapshot_head);

// Snapshot id (setup at launch time by dump or restore).
static char *snapshot_id;

// True if restoring, false if dumping
bool restoring = true;

/* A snapshot is a dump or pre-dump operation. Each snapshot is identified by an
 * ID which corresponds to the working directory specified by the user.
 */
struct snapshot {
	char snapshot_id[PATH_MAX];
	struct list_head l;
};

static struct snapshot *new_snapshot(char *snapshot_id)
{
	struct snapshot *s = xmalloc(sizeof(struct snapshot));

	if (!s)
		return NULL;

	strncpy(s->snapshot_id, snapshot_id, PATH_MAX - 1);
	s->snapshot_id[PATH_MAX - 1]= '\0';
	return s;
}

static void add_snapshot(struct snapshot *snapshot)
{
	list_add_tail(&(snapshot->l), &snapshot_head);
}

static int setup_UNIX_client_socket(char *path)
{
	struct sockaddr_un addr;
	int sockfd = socket(AF_UNIX, SOCK_STREAM, 0);

	if (sockfd < 0) {
		pr_perror("Unable to open local image socket");
		return -1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, path, sizeof(addr.sun_path)-1);

	if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		pr_perror("Unable to connect to local socket: %s", path);
		close(sockfd);
		return -1;
	}

	return sockfd;
}

/*
 * Using a pipe for image file transfers instead of the UNIX socket can
 * greatly improve performance. Data can be further spliced() by the proxy
 * to transfer data with minimal overhead.
 * Transfer rates of up to 15GB/s can be seen with this technique.
 */
#define SEND_FD_FOR_READ	0
#define SEND_FD_FOR_WRITE	1
#define RECV_FD			2
static int upgrade_to_pipe(int sockfd, int mode)
{
	/*
	 * If the other end of the pipe closes, the kernel will want to kill
	 * us with a SIGPIPE. These signal must be ignored, which we do in
	 * crtools.c:main() with signal(SIGPIPE, SIG_IGN).
	 */

	int ret = -1;

	if (mode == RECV_FD) {
		ret = recv_fd(sockfd);
	} else {
		// mode is either 0 (SEND_FD_FOR_READ) or 1 (SEND_FD_FOR_WRITE)
		int direction = mode;
		int other_direction = 1 - direction;
		int fds[2];

		if (pipe(fds) < 0) {
			pr_perror("Unable to create pipe");
			goto err;
		}

		if (send_fd(sockfd, NULL, 0, fds[other_direction]) < 0)
			close(fds[direction]);
		else
			ret = fds[direction];

		close(fds[other_direction]);
	}

err:
	close(sockfd);
	return ret;
}

static int64_t pb_write_obj(int fd, void *obj, int type)
{
	struct cr_img img;

	img._x.fd = fd;
	bfd_setraw(&img._x);
	return pb_write_one(&img, obj, type);
}

static int64_t pb_read_obj(int fd, void **pobj, int type)
{
	struct cr_img img;

	img._x.fd = fd;
	bfd_setraw(&img._x);
	return do_pb_read_one(&img, pobj, type, true);
}

static int64_t write_header(int fd, char *snapshot_id, char *path,
	int flags)
{
	LocalImageEntry li = LOCAL_IMAGE_ENTRY__INIT;

	li.name = path;
	li.snapshot_id = snapshot_id;
	li.open_mode = flags;
	return pb_write_obj(fd, &li, PB_LOCAL_IMAGE);
}

static int64_t read_reply_header(int fd, int *error)
{
	LocalImageReplyEntry *lir;
	int ret = pb_read_obj(fd, (void **)&lir, PB_LOCAL_IMAGE_REPLY);

	if (ret > 0)
		*error = lir->error;
	free(lir);
	return ret;
}

int read_remote_image_connection(char *snapshot_id, char *path)
{
	int error = 0;
	int sockfd = setup_UNIX_client_socket(restoring ? DEFAULT_CACHE_SOCKET: DEFAULT_PROXY_SOCKET);

	if (sockfd < 0) {
		pr_err("Error opening local connection for %s:%s\n",
				path, snapshot_id);
		return -1;
	}

	if (write_header(sockfd, snapshot_id, path, O_RDONLY) < 0) {
		pr_err("Error writing header for %s:%s\n", path, snapshot_id);
		return -1;
	}

	if (read_reply_header(sockfd, &error) < 0) {
		pr_err("Error reading reply header for %s:%s\n",
				path, snapshot_id);
		return -1;
	}

	if (!error) {
		if (snapshot_id != NULL_SNAPSHOT_ID)
			return upgrade_to_pipe(sockfd, SEND_FD_FOR_READ);
		return sockfd;
	}

	if (error == ENOENT) {
		pr_info("Image does not exist (%s:%s)\n", path, snapshot_id);
		close(sockfd);
		return -ENOENT;
	}
	pr_err("Unexpected error returned: %d (%s:%s)\n",
			error, path, snapshot_id);
	close(sockfd);
	return -1;
}

int write_remote_image_connection(char *snapshot_id, char *path, int flags)
{
	int sockfd = setup_UNIX_client_socket(DEFAULT_PROXY_SOCKET);

	if (sockfd < 0)
		return -1;

	if (write_header(sockfd, snapshot_id, path, flags) < 0) {
		pr_err("Error writing header for %s:%s\n", path, snapshot_id);
		return -1;
	}

	if (snapshot_id != NULL_SNAPSHOT_ID)
		return upgrade_to_pipe(sockfd, SEND_FD_FOR_WRITE);

	return sockfd;
}

int finish_remote_dump(void)
{
	int fd;
	pr_info("Dump side is calling finish\n");

	fd = write_remote_image_connection(NULL_SNAPSHOT_ID, FINISH, O_WRONLY);
	if (fd == -1) {
		pr_err("Unable to open finish dump connection");
		return -1;
	}

	close(fd);
	return 0;
}

int finish_remote_restore(void)
{
	int fd;
	pr_info("Restore side is calling finish\n");

	fd = read_remote_image_connection(NULL_SNAPSHOT_ID, FINISH);
	if (fd == -1) {
		pr_err("Unable to open finish restore connection\n");
		return -1;
	}

	close(fd);
	return 0;
}

int skip_remote_bytes(int fd, unsigned long len)
{
	static char buf[4096];
	int n = 0;
	unsigned long curr = 0;

	for (; curr < len; ) {
		n = read(fd, buf, min(len - curr, (unsigned long)4096));
		if (n == 0) {
			pr_perror("Unexpected end of stream (skipping %lx/%lx bytes)",
				curr, len);
			return -1;
		} else if (n > 0) {
			curr += n;
		} else {
			pr_perror("Error while skipping bytes from stream (%lx/%lx)",
				curr, len);
			return -1;
		}
	}

	if (curr != len) {
		pr_err("Unable to skip the current number of bytes: %lx instead of %lx\n",
			curr, len);
		return -1;
	}
	return 0;
}

static int pull_snapshot_ids(void)
{
	int n, sockfd;
	SnapshotIdEntry *ls;
	struct snapshot *s;

	sockfd = read_remote_image_connection(NULL_SNAPSHOT_ID, PARENT_IMG);

	if (sockfd < 0)
		return -1;

	while (1) {
		n = pb_read_obj(sockfd, (void **)&ls, PB_SNAPSHOT_ID);
		if (!n) {
			close(sockfd);
			return 0;
		} else if (n < 0) {
			pr_err("Unable to read remote snapshot ids\n");
			close(sockfd);
			return -1;
		}

		s = new_snapshot(ls->snapshot_id);
		if (!s) {
			close(sockfd);
			return -1;
		}
		add_snapshot(s);
		pr_info("[read_snapshot ids] parent = %s\n", ls->snapshot_id);
	}
}

int push_snapshot_id(void)
{
	int n;
	SnapshotIdEntry rn = SNAPSHOT_ID_ENTRY__INIT;
	int sockfd;

	restoring = false;

	sockfd = write_remote_image_connection(NULL_SNAPSHOT_ID, PARENT_IMG, O_APPEND);
	if (sockfd < 0) {
		pr_err("Unable to open snapshot id push connection\n");
		return -1;
	}

	rn.snapshot_id = xmalloc(sizeof(char) * PATH_MAX);
	if (!rn.snapshot_id) {
		close(sockfd);
		return -1;
	}
	strlcpy(rn.snapshot_id, snapshot_id, PATH_MAX);

	n = pb_write_obj(sockfd, &rn, PB_SNAPSHOT_ID);

	xfree(rn.snapshot_id);
	close(sockfd);
	return n;
}

void init_snapshot_id(char *si)
{
	snapshot_id = si;
}

int get_curr_snapshot_id_idx(void)
{
	struct snapshot *si;
	int idx = 0;

	if (list_empty(&snapshot_head)) {
		if (pull_snapshot_ids() < 0)
			return -1;
	}

	list_for_each_entry(si, &snapshot_head, l) {
		if (!strncmp(si->snapshot_id, snapshot_id, PATH_MAX))
			return idx;
		idx++;
	}

	pr_err("Error, could not find current snapshot id (%s) fd\n",
		snapshot_id);
	return -1;
}

char *get_snapshot_id_from_idx(int idx)
{
	struct snapshot *si;

	if (list_empty(&snapshot_head)) {
		if (pull_snapshot_ids() < 0)
			return NULL;
	}

	/* Note: if idx is the service fd then we need the current
	 * snapshot_id idx. Else we need a parent snapshot_id idx.
	 */
	if (idx == get_service_fd(IMG_FD_OFF))
		idx = get_curr_snapshot_id_idx();

	list_for_each_entry(si, &snapshot_head, l) {
		if (!idx)
			return si->snapshot_id;
		idx--;
	}

	pr_err("Error, could not find snapshot id for idx %d\n", idx);
	return NULL;
}

int get_curr_parent_snapshot_id_idx(void)
{
	return get_curr_snapshot_id_idx() - 1;
}
