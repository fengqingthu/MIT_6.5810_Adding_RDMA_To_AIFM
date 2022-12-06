/**
 * This RDMA server code is heavily adapated from fastswap.
 * (https://github.com/clusterfarmem/fastswap)
 */

#pragma once

#include "rdma_client.hpp"

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
	struct rdma_device *dev;

	unsigned int queue_ctr;

	struct ibv_comp_channel *comp_channel; // Never used on the server side.
};

int start_rdma_server();
int destroy_server();

static int alloc_server();
static struct rdma_device *server_get_device(struct queue *q);
static void server_create_qp(struct queue *q);
static int on_connect_request(struct rdma_cm_id *id, struct rdma_conn_param *param);
static int on_connection(struct queue *q);
static int on_disconnect(struct queue *q);
static int on_event(struct rdma_cm_event *event);