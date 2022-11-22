/**
 * This RDMA server code is heavily adapated from fastswap.
 */

#ifndef RDMA_SERVER_H
#define RDMA_SERVER_H

/* Default port where the RDMA server is listening */
#define DEFAULT_RDMA_PORT (20886)

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

extern const unsigned int NUM_QUEUES = 10;					// Number of cores.
extern const size_t BUFFER_SIZE = 1024 * 1024 * 1024 * 32l; // 32GB, according to fastswap

struct device
{
	struct ibv_pd *pd;
	struct ibv_context *verbs;
};

struct queue
{
	struct ibv_qp *qp;
	struct ibv_cq *cq;
	struct rdma_cm_id *cm_id;
	struct ctrl *ctrl;
	enum
	{
		INIT,
		CONNECTED
	} state;
};

struct ctrl
{
	struct queue *queues;
	struct ibv_mr *mr_buffer;
	void *buffer;
	struct device *dev;

	struct ibv_comp_channel *comp_channel;
};

/* Server metadata. Need to be destoryed on disconnection. */
struct rdma_server
{
	struct rdma_event_channel *ec;
	struct rdma_cm_id *listener;
};

struct memregion
{
	uint64_t baseaddr;
	uint32_t key;
};

static void die(const char *reason);

static int alloc_control();
static int on_connect_request(struct rdma_cm_id *id, struct rdma_conn_param *param);
static int on_connection(struct queue *q);
static int on_disconnect(struct queue *q);
static int on_event(struct rdma_cm_event *event);
static device *get_device(struct queue *q);
static void destroy_device(struct ctrl *ctrl);
static void create_qp(struct queue *q);

static rdma_server *start_rdma_server();
static void destroy_server(struct rdma_server *server);

#endif /* RDMA_SERVER_H */