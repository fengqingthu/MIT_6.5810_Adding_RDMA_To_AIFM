/**
 * This RDMA server code is heavily adapated from fastswap.
 * (https://github.com/clusterfarmem/fastswap)
 */

#ifndef RDMA_SERVER_H
#define RDMA_SERVER_H

/* Default port where the RDMA server is listening */
#define DEFAULT_RDMA_PORT (20886)

#define NUM_QUEUES (20) // Number of most possible threads.

#define TEST_NZ(x)                                            \
	do                                                        \
	{                                                         \
		if ((x))                                              \
			die("error: " #x " failed (returned non-zero)."); \
	} while (0)
#define TEST_Z(x)                                              \
	do                                                         \
	{                                                          \
		if (!(x))                                              \
			die("error: " #x " failed (returned zero/null)."); \
	} while (0)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <rdma/rdma_cma.h>

const size_t BUFFER_SIZE = 1024 * 1024 * 1024 * 32l; // 32GB by default, according to fastswap

struct device
{
	struct ibv_pd *pd;
	struct ibv_context *verbs;
};

struct queue
{
	struct ibv_qp *qp;
	struct ibv_cq *cq; // Server side cq is never allocated.
	struct rdma_cm_id *cm_id;
	struct rdma_server *server;
	enum
	{
		INIT,
		CONNECTED
	} state;
};

/* Server metadata. Need to be destoryed on disconnection. At a time
 * there should only be one global server instance. */
struct rdma_server
{
	struct rdma_event_channel *ec;
	struct rdma_cm_id *listener;

	struct queue *queues;
	struct ibv_mr *mr_buffer;

	void *buffer;
	struct device *dev;

	unsigned int queue_ctr;

	struct ibv_comp_channel *comp_channel; // Never used on the server side.
};

/* Private data passed over rdma_cm protocol */
typedef struct
{
	uint64_t baseaddr; /* Remote buffer address for RDMA */
	uint32_t key;	   /* Remote key for RDMA */
} memregion_t;

extern static int start_rdma_server();
extern static int destroy_server();

static void die(const char *reason);
static int alloc_server();
static struct device *get_device(struct queue *q);
static void create_qp(struct queue *q);
static int on_connect_request(struct rdma_cm_id *id, struct rdma_conn_param *param);
static int on_connection(struct queue *q);
static int on_disconnect(struct queue *q);
static int on_event(struct rdma_cm_event *event);

#endif /* RDMA_SERVER_H */