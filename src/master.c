#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <ctype.h>
#include <appcbase.h>

#include <mloop.h>
#include "socketcan.h"
#include "canopen.h"
#include "canopen/sdo.h"
#include "canopen/sdo_req.h"
#include "canopen/network.h"
#include "canopen/nmt.h"
#include "canopen/heartbeat.h"
#include "canopen/emcy.h"
#include "canopen/eds.h"
#include "frame_fifo.h"
#include "rest.h"

#include "legacy-driver.h"

#define MIN(a,b) ((a) < (b) ? (a) : (b))

#define SDO_FIFO_MAX_LENGTH 1024

#define SDO_TIMEOUT 100 /* ms */
#define HEARTBEAT_PERIOD 10000 /* ms */
#define HEARTBEAT_TIMEOUT 11000 /* ms */

#define for_each_node(index) \
	for(index = CANOPEN_NODEID_MIN; index <= CANOPEN_NODEID_MAX; ++index)

#define for_each_node_reverse(index) \
	for(index = CANOPEN_NODEID_MAX; index >= CANOPEN_NODEID_MIN; --index)

static int socket_ = -1;
static char nodes_seen_[CANOPEN_NODEID_MAX + 1];
/* Note: nodes_seen_[0] is unused */

typedef void (*void_cb_fn)(void*);

enum master_state {
	MASTER_STATE_STARTUP = 0,
	MASTER_STATE_RUNNING
};

static enum master_state master_state_ = MASTER_STATE_STARTUP;

static void* driver_manager_;
pthread_mutex_t driver_manager_lock_ = PTHREAD_MUTEX_INITIALIZER;

static struct mloop* mloop_ = NULL;
static struct mloop_socket* mux_handler_ = NULL;

struct canopen_node;

struct canopen_node {
	void* driver;
	void* master_iface;

	struct frame_fifo frame_fifo;

	int device_type;
	int is_heartbeat_supported;

	uint32_t vendor_id, product_code, revision_number;

	struct mloop_timer* heartbeat_timer;
	struct mloop_timer* ping_timer;

	int is_loading;
};

struct rest_context {
	struct rest_client* client;
	const struct eds_obj* eds_obj;
};

static void* master_iface_init(int nodeid);
static int master_request_sdo(int nodeid, int index, int subindex);
static int master_send_sdo(int nodeid, int index, int subindex,
			   unsigned char* data, size_t size);
static int send_pdo(int nodeid, int type, unsigned char* data, size_t size);
static int master_send_pdo(int nodeid, int n, unsigned char* data, size_t size);
static void unload_legacy_module(int device_type, void* driver);

static struct canopen_node node_[CANOPEN_NODEID_MAX + 1];
/* Note: node_[0] is unsued */

static int is_profiling_on_ = -1;

static inline int is_profiling_on()
{
	if (is_profiling_on_ < 0)
		is_profiling_on_ = getenv("CANOPEN_PROFILE") != NULL;
	return is_profiling_on_;
}

static inline uint64_t gettime_us()
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000LL + ts.tv_nsec / 1000LL;
}

static uint64_t start_time_ = 0;

#define tprintf(fmt, ...) \
	printf("%07llu\t" fmt, gettime_us() - start_time_, ## __VA_ARGS__)

#define profile(fmt, ...) if (is_profiling_on()) tprintf(fmt, ## __VA_ARGS__)

static inline int get_node_id(const struct canopen_node* node)
{
	return ((char*)node - (char*)node_) / sizeof(struct canopen_node);
}

void clean_node_name(char* name, size_t size)
{
	int i, k = 0;

	for (i = 0; i < size && name[i]; ++i)
		if (isalnum(name[i])) {
			int c = name[i];
			name[k++] = c;
		}

	if (i == size)
		name[i-1] = '\0';
	else if (k < i)
		name[k] = '\0';

}

struct sdo_req* sdo_read(int nodeid, int index, int subindex)
{
	struct sdo_req_info info = {
		.type = SDO_REQ_UPLOAD,
		.index = index,
		.subindex = subindex
	};

	struct sdo_req* req = sdo_req_new(&info);
	if (!req)
		return NULL;

	if (sdo_req_start(req, sdo_req_queue_get(nodeid)) < 0)
		goto done;

	sdo_req_wait(req);

	if (req->status != SDO_REQ_OK)
		goto done;

	return req;

done:
	sdo_req_free(req);
	return NULL;
}

static uint32_t sdo_read_u32(int nodeid, int index, int subindex)
{
	uint32_t value = 0;

	struct sdo_req* req = sdo_read(nodeid, index, subindex);
	if (!req)
		return 0;

	if (req->data.index > sizeof(value))
		goto done;

	byteorder2(&value, req->data.data, sizeof(value), req->data.index);

done:
	sdo_req_free(req);
	return value;
}

static inline uint32_t get_device_type(int nodeid)
{
	return sdo_read_u32(nodeid, 0x1000, 0);
}

static inline int node_has_identity(int nodeid)
{
	return !!sdo_read_u32(nodeid, 0x1018, 0);
}

static inline uint32_t get_vendor_id(int nodeid)
{
	return sdo_read_u32(nodeid, 0x1018, 1);
}

static inline uint32_t get_product_code(int nodeid)
{
	return sdo_read_u32(nodeid, 0x1018, 2);
}

static inline uint32_t get_revision_number(int nodeid)
{
	return sdo_read_u32(nodeid, 0x1018, 3);
}

static inline int set_heartbeat_period(int nodeid, uint16_t period)
{
	int rc = -1;

	struct sdo_req_info info = {
		.type = SDO_REQ_DOWNLOAD,
		.index = 0x1017,
		.subindex = 0,
		.dl_data = &period,
		.dl_size = sizeof(period)
	};

	struct sdo_req* req = sdo_req_new(&info);
	if (!req)
		return -1;

	if (sdo_req_start(req, sdo_req_queue_get(nodeid)) < 0)
		goto failure;

	sdo_req_wait(req);
	if (req->status != SDO_REQ_OK)
		goto failure;

	rc = 0;
failure:
	sdo_req_free(req);
	return rc;
}

const char* get_name(int nodeid)
{
	static __thread char name[256];

	struct sdo_req* req = sdo_read(nodeid, 0x1008, 0);
	if (!req)
		goto done;

	memcpy(name, req->data.data, MIN(req->data.index, sizeof(name)));
	clean_node_name(name, sizeof(name));

done:
	sdo_req_free(req);
	return name;
}

static void stop_heartbeat_timer(int nodeid)
{
	struct canopen_node* node = &node_[nodeid];
	struct mloop_timer* timer = node->heartbeat_timer;
	mloop_timer_stop(timer);
}

static void stop_ping_timer(int nodeid)
{
	struct canopen_node* node = &node_[nodeid];
	struct mloop_timer* timer = node->ping_timer;
	mloop_timer_stop(timer);
}

static void unload_driver(int nodeid)
{
	struct canopen_node* node = &node_[nodeid];

	stop_heartbeat_timer(nodeid);

	if (!node->is_heartbeat_supported)
		stop_ping_timer(nodeid);

	unload_legacy_module(node->device_type, node->driver);

	legacy_master_iface_delete(node->master_iface);

	node->driver = NULL;
	node->master_iface = NULL;
	node->device_type = 0;
	node->is_heartbeat_supported = 0;

	net__send_nmt(socket_, NMT_CS_RESET_NODE, nodeid);
}

static void on_heartbeat_timeout(struct mloop_timer* timer)
{
	struct canopen_node* node = mloop_timer_get_context(timer);
	unload_driver(get_node_id(node));
}

static int start_heartbeat_timer(int nodeid)
{
	struct canopen_node* node = &node_[nodeid];
	struct mloop_timer* timer = node->heartbeat_timer;
	return mloop_start_timer(mloop_, timer);
}

static int restart_heartbeat_timer(int nodeid)
{
	stop_heartbeat_timer(nodeid);
	return start_heartbeat_timer(nodeid);
}

static void on_ping_timeout(struct mloop_timer* timer)
{
	struct canopen_node* node = mloop_timer_get_context(timer);
	int nodeid = get_node_id(node);

	struct can_frame cf = { 0 };

	cf.can_id = R_HEARTBEAT + nodeid;
	cf.can_dlc = 1;
	heartbeat_set_state(&cf, 1);

	net_write_frame(socket_, &cf, -1);
}

static int start_ping_timer(int nodeid)
{
	struct canopen_node* node = &node_[nodeid];
	struct mloop_timer* timer = node->ping_timer;
	return mloop_start_timer(mloop_, timer);
}

ssize_t write_frame(const struct can_frame* frame)
{
	return net_write_frame(socket_, frame, -1);
}

static void start_nodeguarding(int nodeid)
{
	struct canopen_node* node = &node_[nodeid];

	if (!node->is_heartbeat_supported)
		start_ping_timer(nodeid);

	start_heartbeat_timer(nodeid);
}

static void* load_legacy_module(const char* name, int device_type,
				void* master_iface)
{
	void* driver = NULL;
	pthread_mutex_lock(&driver_manager_lock_);
	int rc = legacy_driver_manager_create_handler(driver_manager_, name,
						      device_type, master_iface,
						      &driver);
	pthread_mutex_unlock(&driver_manager_lock_);
	return rc >= 0 ? driver : NULL;
}

static void unload_legacy_module(int device_type, void* driver)
{
	pthread_mutex_lock(&driver_manager_lock_);
	legacy_driver_delete_handler(driver_manager_, device_type, driver);
	pthread_mutex_unlock(&driver_manager_lock_);
}

static int load_driver(int nodeid)
{
	struct canopen_node* node = &node_[nodeid];
	if (node->driver)
		unload_driver(nodeid);

	uint32_t device_type = get_device_type(nodeid);
	if (device_type == 0)
		goto failure;

	const char* name = get_name(nodeid);
	if (!name)
		goto failure;

	if (node_has_identity(nodeid)) {
		node->vendor_id = get_vendor_id(nodeid);
		node->product_code = get_product_code(nodeid);
		node->revision_number = get_revision_number(nodeid);
	}

	void* master_iface = master_iface_init(nodeid);
	if (!master_iface)
		goto failure;

	void* driver = load_legacy_module(name, device_type, master_iface);
	if (!driver)
		goto driver_create_failure;

	int is_heartbeat_supported =
		set_heartbeat_period(nodeid, HEARTBEAT_PERIOD) >= 0;

	node->driver = driver;
	node->device_type = device_type;
	node->master_iface = master_iface;
	node->is_heartbeat_supported = is_heartbeat_supported;

	return 0;

driver_create_failure:
	legacy_master_iface_delete(master_iface);

	node->driver = NULL;
	node->master_iface = NULL;
failure:

	return -1;
}

static void initialize_driver(int nodeid)
{
	struct canopen_node* node = &node_[nodeid];
	if (legacy_driver_iface_initialize(node->driver) < 0) {
		unload_legacy_module(node->device_type, node->driver);
		legacy_master_iface_delete(node->master_iface);
		node->driver = NULL;
		node->master_iface = NULL;
		return;
	}

	legacy_driver_iface_process_node_state(node->driver, 1);
}

static void run_net_probe(struct mloop_work* self)
{
	(void)self;

	profile("Probe network...\n");
	net_reset(socket_, nodes_seen_, 100);
	net_probe(socket_, nodes_seen_, 1, 127, 100);
}

static void run_load_driver(struct mloop_work* self)
{
	struct canopen_node* node = mloop_work_get_context(self);
	int nodeid = get_node_id(node);
	load_driver(nodeid);
	node->is_loading = 0;
}

static void on_load_driver_done(struct mloop_work* self)
{
	struct canopen_node* node = mloop_work_get_context(self);
	int nodeid = get_node_id(node);

	if (!node->driver)
		return;

	initialize_driver(nodeid);

	start_nodeguarding(nodeid);

	if (master_state_ > MASTER_STATE_STARTUP)
		net__send_nmt(socket_, NMT_CS_START, nodeid);
}

static int schedule_load_driver(int nodeid)
{
	struct canopen_node* node = &node_[nodeid];
	if (node->is_loading)
		return 0;

	struct mloop_work* work = mloop_work_new();
	if (!work)
		return -1;

	mloop_work_set_context(work, node, NULL);
	mloop_work_set_work_fn(work, run_load_driver);
	mloop_work_set_done_fn(work, on_load_driver_done);

	int rc = mloop_start_work(mloop_, work);

	mloop_work_unref(work);

	node->is_loading = 1;

	return rc;
}

static int handle_bootup(struct canopen_node* node)
{
	if (master_state_ == MASTER_STATE_STARTUP)
		return 0;

	return schedule_load_driver(get_node_id(node));
}

static int handle_emcy(struct canopen_node* node, const struct can_frame* frame)
{
	if (frame->can_dlc == 0)
		return handle_bootup(node);

	if (!node->driver)
		return -1;

	if (frame->can_dlc != 8)
		return -1;

	legacy_driver_iface_process_emr(node->driver,
					emcy_get_code(frame),
					emcy_get_register(frame),
					emcy_get_manufacturer_error(frame));

	return 0;
}

static int handle_heartbeat(struct canopen_node* node,
			     const struct can_frame* frame)
{
	if (!heartbeat_is_valid(frame))
		return -1;

	if (heartbeat_is_bootup(frame))
		return handle_bootup(node);

	if (master_state_ == MASTER_STATE_STARTUP)
		return 0;

	int nodeid = get_node_id(node);

	restart_heartbeat_timer(nodeid);

	/* Make sure the node is in operational state */
	if (heartbeat_get_state(frame) != NMT_STATE_OPERATIONAL)
		net__send_nmt(socket_, NMT_CS_START, nodeid);

	return 0;
}

static void handle_sdo(struct canopen_node* node, const struct can_frame* cf)
{
	int nodeid = get_node_id(node);
	struct sdo_async* sdo_proc = &sdo_req_queue_get(nodeid)->sdo_client;
	sdo_async_feed(sdo_proc, cf);
}

static void handle_not_loaded(struct canopen_node* node,
			      const struct canopen_msg* msg,
			      const struct can_frame* frame)
{
	switch (msg->object)
	{
	case CANOPEN_NMT:
		break; /* TODO: this is illegal */
	case CANOPEN_EMCY:
		handle_emcy(node, frame);
		break;
	case CANOPEN_HEARTBEAT:
		handle_heartbeat(node, frame);
		break;
	case CANOPEN_TSDO:
		handle_sdo(node, frame);
		break;
	default:
		break;
	}
}

static void mux_handler_fn(struct mloop_socket* self)
{
	int fd = mloop_socket_get_fd(self);

	struct can_frame cf;
	struct canopen_msg msg;

	net_read_frame(fd, &cf, -1);

	if (canopen_get_object_type(&msg, &cf) < 0)
		return;

	if (!(CANOPEN_NODEID_MIN <= msg.id && msg.id <= CANOPEN_NODEID_MAX))
		return;

	struct canopen_node* node = &node_[msg.id];
	void* driver = node->driver;

	if (!driver) {
		handle_not_loaded(node, &msg, &cf);
		return;
	}

	switch (msg.object)
	{
	case CANOPEN_NMT:
		break; /* TODO: this is illegal */
	case CANOPEN_TPDO1:
		legacy_driver_iface_process_pdo(driver, 1, cf.data, cf.can_dlc);
		break;
	case CANOPEN_TPDO2:
		legacy_driver_iface_process_pdo(driver, 2, cf.data, cf.can_dlc);
		break;
	case CANOPEN_TPDO3:
		legacy_driver_iface_process_pdo(driver, 3, cf.data, cf.can_dlc);
		break;
	case CANOPEN_TPDO4:
		legacy_driver_iface_process_pdo(driver, 4, cf.data, cf.can_dlc);
		break;
	case CANOPEN_TSDO:
		handle_sdo(node, &cf);
		break;
	case CANOPEN_EMCY:
		handle_emcy(node, &cf);
		break;
	case CANOPEN_HEARTBEAT:
		handle_heartbeat(node, &cf);
		break;
	default:
		break;
	}
}

static int init_multiplexer()
{
	mux_handler_ = mloop_socket_new();
	if (!mux_handler_)
		return -1;

	mloop_socket_set_fd(mux_handler_, socket_);
	mloop_socket_set_callback(mux_handler_, mux_handler_fn);

	return mloop_start_socket(mloop_, mux_handler_);
}

static void run_bootup(struct mloop_work* self)
{
	(void)self;

	int i;

	profile("Load drivers...\n");
	for_each_node(i)
		if (nodes_seen_[i])
			load_driver(i);

	profile("Initialize drivers...\n");
	for_each_node(i)
		if (node_[i].driver)
			initialize_driver(i);
}

static void on_bootup_done(struct mloop_work* self)
{
	int i;

	/* We start each node individually because we don't want to start nodes
	 * that were not properly registered.
	 */
	profile("Start nodes...\n");
	for_each_node_reverse(i)
		if (node_[i].driver)
			net__send_nmt(socket_, NMT_CS_START, i);

	profile("Start node guarding...\n");
	for_each_node(i)
		if (node_[i].driver)
			start_nodeguarding(i);

	profile("Boot-up finished!\n");

	master_state_ = MASTER_STATE_RUNNING;
}

static void on_net_probe_done(struct mloop_work* self)
{
	profile("Initialize multiplexer...\n");
	int rc = init_multiplexer();
	assert(rc == 0);

	struct mloop_work* work = mloop_work_new();
	assert(work);
	mloop_work_set_work_fn(work, run_bootup);
	mloop_work_set_done_fn(work, on_bootup_done);

	rc = mloop_start_work(mloop_, work);
	assert(rc == 0);

	mloop_work_unref(work);
}

static int appbase_dummy()
{
	return 0;
}

static int on_tickermaster_alive()
{
	struct mloop_work* work = mloop_work_new();
	if (!work)
		return -1;

	mloop_work_set_work_fn(work, run_net_probe);
	mloop_work_set_done_fn(work, on_net_probe_done);

	int rc = mloop_start_work(mloop_, work);
	mloop_work_unref(work);

	return rc;
}

static int run_appbase(int* argc, char* argv[])
{
	appcbase_t cb = { 0 };
	cb.queue_timeout_ms = -1;
	cb.usetm = 1;
	cb.initialize = appbase_dummy;
	cb.deinitialize = appbase_dummy;
	cb.on_tickmaster_alive = on_tickermaster_alive;

	if (appbase_cmdline(argc, argv) != 0)
		return 1;

	if (appbase_initialize("canopen", appbase_get_instance(), &cb) != 0)
		return 1;

	mloop_run(mloop_);

	appbase_deinitialize();

	return 0;
}

static int master_set_node_state(int nodeid, int state)
{
	/* not allowed */
	return 0;
}

static inline
struct canopen_node* get_node_from_sdo_queue(const struct sdo_req_queue* queue)
{
	return &node_[queue->nodeid];
}

static void on_master_sdo_request_done(struct sdo_req* req)
{
	struct canopen_node* node = get_node_from_sdo_queue(req->parent);
	void* driver = node->driver;

	if (req->status == SDO_REQ_OK) {
		legacy_driver_iface_process_sdo(driver,
						req->index,
						req->subindex,
						(unsigned char*)req->data.data,
						req->data.index);
	} else {
		legacy_driver_iface_process_sdo(driver,
						req->index,
						req->subindex,
						NULL, 0);
	}

	sdo_req_free(req);
}

static int master_request_sdo(int nodeid, int index, int subindex)
{
	struct sdo_req_info info = {
		.type = SDO_REQ_UPLOAD,
		.index = index,
		.subindex = subindex,
		.on_done = on_master_sdo_request_done
	};

	struct sdo_req* req = sdo_req_new(&info);
	if (!req)
		return -1;

	if (sdo_req_start(req, sdo_req_queue_get(nodeid)) >= 0)
		return 0;

	sdo_req_free(req);
	return -1;
}

static void on_master_sdo_send_done(struct sdo_req* req)
{
	(void)req;
}

static int master_send_sdo(int nodeid, int index, int subindex,
			   unsigned char* data, size_t size)
{
	struct sdo_req_info info = {
		.type = SDO_REQ_DOWNLOAD,
		.index = index,
		.subindex = subindex,
		.dl_data = data,
		.dl_size = size,
		.on_done = on_master_sdo_send_done
	};

	struct sdo_req* req = sdo_req_new(&info);
	if (!req)
		return -1;

	if (sdo_req_start(req, sdo_req_queue_get(nodeid)) >= 0)
		return 0;

	sdo_req_free(req);
	return -1;
}

static int send_pdo(int nodeid, int type, unsigned char* data, size_t size)
{
	struct can_frame cf = { 0 };

	cf.can_id = type + nodeid;
	cf.can_dlc = size;

	if (data)
		memcpy(cf.data, data, size);

	return net_write_frame(socket_, &cf, -1);

}

static int master_send_pdo(int nodeid, int n, unsigned char* data, size_t size)
{
	switch (n) {
	case 1: return send_pdo(nodeid, R_RPDO1, data, size);
	case 2: return send_pdo(nodeid, R_RPDO2, data, size);
	case 3: return send_pdo(nodeid, R_RPDO3, data, size);
	case 4: return send_pdo(nodeid, R_RPDO4, data, size);
	}

	abort();
	return -1;
}

static void* master_iface_init(int nodeid)
{
	struct legacy_master_iface cb;
	cb.set_node_state = master_set_node_state;
	cb.request_sdo = master_request_sdo;
	cb.send_sdo = master_send_sdo;
	cb.send_pdo = master_send_pdo;
	cb.nodeid = nodeid;

	return legacy_master_iface_new(&cb);
}

static int init_heartbeat_timer(struct canopen_node* node)
{
	struct mloop_timer* timer = mloop_timer_new();
	if (!timer)
		return -1;

	mloop_timer_set_context(timer, node, NULL);
	mloop_timer_set_time(timer, HEARTBEAT_TIMEOUT * 1000000LL);
	mloop_timer_set_callback(timer, on_heartbeat_timeout);
	node->heartbeat_timer = timer;

	return 0;
}

static int init_ping_timer(struct canopen_node* node)
{
	struct mloop_timer* timer = mloop_timer_new();
	if (!timer)
		return -1;

	mloop_timer_set_type(timer, MLOOP_TIMER_PERIODIC);
	mloop_timer_set_context(timer, node, NULL);
	mloop_timer_set_time(timer, HEARTBEAT_PERIOD * 1000000LL);
	mloop_timer_set_callback(timer, on_ping_timeout);
	node->ping_timer = timer;

	return 0;
}

static int init_node_structure(int nodeid)
{
	struct canopen_node* node = &node_[nodeid];

	memset(node, 0, sizeof(*node));

	if (init_heartbeat_timer(node) < 0)
		return -1;;

	if (init_ping_timer(node) < 0)
		goto ping_timer_failure;

	return 0;

ping_timer_failure:
	mloop_timer_free(node->heartbeat_timer);
	return -1;
}

static void destroy_node_structure(int nodeid)
{
	struct canopen_node* node = &node_[nodeid];

	mloop_timer_free(node->ping_timer);
	mloop_timer_free(node->heartbeat_timer);
}

static int init_all_node_structures()
{
	int i;
	for_each_node(i)
		if (init_node_structure(i) < 0)
			goto failure;

	return 0;

failure:
	for (; i > 0; --i)
		destroy_node_structure(i);

	return -1;
}

static void destroy_all_node_structures()
{
	int i;
	for_each_node(i)
		destroy_node_structure(i);
}

static void unload_all_drivers()
{
	int i;
	for_each_node(i)
		if(node_[i].driver)
			unload_driver(i);
}

void sdo_rest_not_found(struct rest_client* client, const char* message)
{
	struct rest_reply_data reply = {
		.status_code = "404 Not Found",
		.content_type = "text/plain",
		.content_length = strlen(message),
		.content = message
	};

	rest_reply(client->output, &reply);

	client->state = REST_CLIENT_DONE;
}

void sdo_rest_server_error(struct rest_client* client, const char* message)
{
	struct rest_reply_data reply = {
		.status_code = "500 Internal Server Error",
		.content_type = "text/plain",
		.content_length = strlen(message),
		.content = message
	};

	rest_reply(client->output, &reply);

	client->state = REST_CLIENT_DONE;
}

void on_sdo_rest_string(struct sdo_req* req, struct rest_client* client)
{
	struct rest_reply_data reply = {
		.status_code = "200 OK",
		.content_type = "text/plain",
		.content_length = req->data.index,
		.content = req->data.data
	};

	rest_reply(client->output, &reply);
}

void on_sdo_rest_u64(struct sdo_req* req, struct rest_client* client)
{
	char buffer[32];

	uint64_t number = 0;

	byteorder2(&number, req->data.data, sizeof(number), req->data.index);

	sprintf(buffer, "%llu\r\n", number);

	struct rest_reply_data reply = {
		.status_code = "200 OK",
		.content_type = "text/plain",
		.content_length = strlen(buffer),
		.content = buffer
	};

	rest_reply(client->output, &reply);
}

void on_sdo_rest_s64(struct sdo_req* req, struct rest_client* client)
{
	char buffer[32];

	int64_t number = 0;

	byteorder2(&number, req->data.data, sizeof(number), req->data.index);

	sprintf(buffer, "%lld\r\n", number);

	struct rest_reply_data reply = {
		.status_code = "200 OK",
		.content_type = "text/plain",
		.content_length = strlen(buffer),
		.content = buffer
	};

	rest_reply(client->output, &reply);
}

void on_sdo_rest_r64(struct sdo_req* req, struct rest_client* client)
{
	char buffer[32];

	double number = 0;

	byteorder2(&number, req->data.data, sizeof(number), req->data.index);

	sprintf(buffer, "%lf\r\n", number);

	struct rest_reply_data reply = {
		.status_code = "200 OK",
		.content_type = "text/plain",
		.content_length = strlen(buffer),
		.content = buffer
	};

	rest_reply(client->output, &reply);
}

void on_sdo_rest_upload_done(struct sdo_req* req)
{
	struct rest_context* context = req->context;
	assert(context);
	struct rest_client* client = context->client;
	const struct eds_obj* eds_obj = context->eds_obj;

	if (req->status != SDO_REQ_OK) {
		sdo_rest_server_error(client, sdo_strerror(req->abort_code));
		goto done;
	}

	if (eds_obj) {
		if (canopen_type_is_signed_integer(eds_obj->type))
			on_sdo_rest_s64(req, client);
		else if (canopen_type_is_unsigned_integer(eds_obj->type))
			on_sdo_rest_u64(req, client);
		else if (canopen_type_is_real(eds_obj->type))
			on_sdo_rest_r64(req, client);
		else if (canopen_type_is_string(eds_obj->type))
			on_sdo_rest_string(req, client);

	} else {
		if (req->data.index <= 8)
			on_sdo_rest_u64(req, client);
		else
			on_sdo_rest_string(req, client);
	}

	client->state = REST_CLIENT_DONE;

done:
	free(context);
	sdo_req_free(req);
}

void sdo_rest_service(struct rest_client* client, const void* content)
{
	(void)content;

	if (client->req.url_index < 4) {
		sdo_rest_not_found(client, "Wrong URL format. Must be /sdo/<nodeid>/<index>/<subindex>\r\n");
		return;
	}

	int nodeid = strtoul(client->req.url[1], NULL, 10);
	int index = strtoul(client->req.url[2], NULL, 16);
	int subindex = strtoul(client->req.url[3], NULL, 10);

	if (!(CANOPEN_NODEID_MIN <= nodeid && nodeid <= CANOPEN_NODEID_MAX)) {
		sdo_rest_not_found(client, "Invalid node id\r\n");
		return;
	}

	struct rest_context* context = malloc(sizeof(*context));
	context->client = client;

	struct canopen_node* node = &node_[nodeid];

	const struct canopen_eds* eds = eds_db_find(node->vendor_id,
						    node->product_code,
						    -1);
	if (eds)
		context->eds_obj = eds_obj_find(eds, index, subindex);

	struct sdo_req_info info = {
		.type = SDO_REQ_UPLOAD,
		.index = index,
		.subindex = subindex,
		.on_done = on_sdo_rest_upload_done,
		.context = context
	};

	struct sdo_req* req = sdo_req_new(&info);
	if (!req)
		goto req_failure;

	if (sdo_req_start(req, sdo_req_queue_get(nodeid)) < 0)
		goto req_start_failure;

	return;

req_start_failure:
	sdo_req_free(req);
req_failure:
	free(context);

	sdo_rest_server_error(client, "Failed to start sdo request\r\n");
}

int main(int argc, char* argv[])
{
	int rc = 0;
	const char* iface = argv[1];

	start_time_ = gettime_us();
	profile("Starting up canopen-master...\n");

	memset(nodes_seen_, 0, sizeof(nodes_seen_));
	memset(node_, 0, sizeof(node_));

	mloop_ = mloop_default();

	profile("Load EDS database...\n");
	eds_db_load();

	profile("Initialize and register SDO REST service...\n");
	if (rest_init() < 0)
		return 1;

	if (rest_register_service(HTTP_GET, "sdo", sdo_rest_service) < 0)
		goto rest_service_failure;

	profile("Open interface...\n");
	socket_ = socketcan_open(iface);
	if (socket_ < 0) {
		perror("Could not open interface");
		goto socketcan_open_failure;
	}

	profile("Initialize SDO queues...\n");
	if (sdo_req_queues_init(socket_, SDO_FIFO_MAX_LENGTH) < 0)
		goto sdo_req_queues_failure;

	profile("Initialize node structure...\n");
	if (init_all_node_structures() < 0)
		goto node_init_failure;

	net_dont_block(socket_);
	net_fix_sndbuf(socket_);

	profile("Create legacy driver manager...\n");
	driver_manager_ = legacy_driver_manager_new();
	if (!driver_manager_) {
		rc = 1;
		goto driver_manager_failure;
	}

	profile("Start worker threads...\n");
	if (mloop_start_workers(4, 1024, 4096) != 0) {
		rc = 1;
		goto worker_failure;
	}

	rc = run_appbase(&argc, argv);

	unload_all_drivers();

	if (mux_handler_) {
		mloop_socket_set_fd(mux_handler_, -1);
		mloop_socket_free(mux_handler_);
	}

	mloop_stop_workers();

worker_failure:
	legacy_driver_manager_delete(driver_manager_);

driver_manager_failure:
	destroy_all_node_structures();

node_init_failure:
	sdo_req_queues_cleanup();

sdo_req_queues_failure:
	if (socket_ >= 0)
		close(socket_);

socketcan_open_failure:
rest_service_failure:
	rest_cleanup();

	eds_db_unload();

	return rc;
}

