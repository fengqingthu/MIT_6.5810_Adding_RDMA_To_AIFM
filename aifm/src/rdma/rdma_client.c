#include "rdma_client.h"

static char serverip[INET_ADDRSTRLEN];
static char clientip[INET_ADDRSTRLEN] = "0.0.0.0";

// TODO: destroy ctrl

#define CONNECTION_TIMEOUT_MS 2000
#define QP_QUEUE_DEPTH 256
/* we don't really use recv wrs, so any small number should do */
#define QP_MAX_RECV_WR 4
/* we mainly do send wrs */
#define QP_MAX_SEND_WR (4096)
#define CQ_NUM_CQES (QP_MAX_SEND_WR)
#define POLL_BATCH_HIGH (QP_MAX_SEND_WR / 4)

/* APIs. */
static struct rdma_client *start_rdma_client(char *sip)
{
	struct rdma_client *gclient = NULL;

	memcpy(serverip, sip, INET_ADDRSTRLEN);
	int ret;

	printf("%s\n", __FUNCTION__);
	printf("* AIFM RDMA BACKEND *");

	TEST_NZ(start_client(&gclient));

	return gclient;
}

/* RDMA Helpers. */

/* Setup RDMA resources. */
static int start_client(struct rdma_client **c)
{
	struct rdma_client *client;
	printf("will try to connect to %s:%d\n", serverip, SERVER_PORT);

	*c = malloc(sizeof(struct rdma_client));
	TEST_Z(*c);
	client = *c;

	client->ec = rdma_create_event_channel();
	TEST_Z(client->ec);

	client->queues = malloc(sizeof(struct rdma_queue) * NUM_QUEUES);
	TEST_Z(client->queues);

	TEST_NZ(parse_ipaddr(&(client->addr_in), serverip));

	client->addr_in.sin_port = htons(SERVER_PORT);

	TEST_NZ(parse_ipaddr(&(client->srcaddr_in), clientip));
	/* no need to set the port on the srcaddr */

	return init_queues(client);
}

static int init_queues(struct rdma_client *client)
{
	int ret, i;
	for (i = 0; i < NUM_QUEUES; ++i)
	{
		ret = init_queue(client, i);
		if (ret)
		{
			printf("failed to initialized queue: %d\n", i);
			goto out_free_queues;
		}
	}

	return 0;

out_free_queues:
	for (i--; i >= 0; i--)
	{
		stop_queue(&client->queues[i]);
		free_queue(&client->queues[i]);
	}

	return ret;
}

static void stop_queue(struct rdma_queue *q)
{
	rdma_disconnect(q->cm_id);
}

static void free_queue(struct rdma_queue *q)
{
	rdma_destroy_qp(q->cm_id);
	ibv_destory_cq(q->cq);
	rdma_destroy_id(q->cm_id);
}

static int init_queue(struct rdma_client *client,
					  int idx)
{
	struct rdma_queue *queue;
	struct rdma_cm_event *event = NULL;
	int ret;

	printf("start: %s[%d]\n", __FUNCTION__, idx);

	queue = &client->queues[idx];
	queue->client = client;

	TEST_NZ(rdma_create_id(client->ec, queue->cm_id, NULL, RDMA_PS_TCP));

	ret = rdma_resolve_addr(queue->cm_id, NULL, &client->addr,
							CONNECTION_TIMEOUT_MS);
	if (ret)
	{
		printf("rdma_resolve_addr failed: %d\n", ret);
		goto out_destroy_cm_id;
	}

	ret = process_rdma_cm_event(client->ec,
								RDMA_CM_EVENT_ADDR_RESOLVED,
								&event);
	if (ret)
	{
		printf("waiting for RDMA_CM_EVENT_ADDR_RESOLVED failed\n");
		goto out_destroy_cm_id;
	}

	TEST_NZ(rdma_ack_cm_event(event));

	printf("RDMA address is resolved.");

	ret = rdma_resolve_route(q->cm_id, CONNECTION_TIMEOUT_MS);
	if (ret)
	{
		printf("rdma_resolve_route failed\n");
		goto out_destroy_cm_id;
	}

	ret = process_rdma_cm_event(client->ec,
								RDMA_CM_EVENT_ROUTE_RESOLVED,
								&event);
	if (ret)
	{
		printf("waiting for RDMA_CM_EVENT_ROUTE_RESOLVED failed\n");
		goto out_destroy_cm_id;
	}

	TEST_NZ(rdma_ack_cm_event(event));

	printf("RDMA route is resolved.");

	struct device *rdev = get_device(queue);
	if (!rdev)
	{
		printf("no device found\n");
		goto out_destroy_cm_id;
	}

	ret = create_queue_ib(queue);
	if (ret)
	{
		printf("failed creating QP and CQ for queue %d.\n", idx);
		goto out_destroy_cm_id;
	}

	ret = connect_to_server(queue);
	if (ret)
	{
		printf("failed connecting to server.\n");
		goto out_destroy_cm_id;
	}

	/* TODO: fetch private data from established event. */

	return 0;

out_destroy_cm_id:
	rdma_destroy_id(queue->cm_id);
	return ret;
}

/* CM Helpers. */
static int connect_to_server(struct rdma_queue *q)
{
	struct rdma_conn_param param = {};
	int ret;

	param.qp_num = q->qp->qp_num;
	param.flow_control = 1;
	param.responder_resources = 16;
	param.initiator_depth = 16;
	param.retry_count = 7;
	param.rnr_retry_count = 7;

	/* No client metadata for server. */
	param.private_data = NULL;
	param.private_data_len = 0;

	// printf("max_qp_rd_atom=%d max_qp_init_rd_atom=%d\n",
	// 	   q->client->rdev->dev->attrs.max_qp_rd_atom,
	// 	   q->client->rdev->dev->attrs.max_qp_init_rd_atom);

	ret = rdma_connect(q->cm_id, &param);
	if (ret)
	{
		printf("rdma_connect failed (%d)\n", ret);
		destroy_queue_ib(q);
	}

	return 0;
}

static void destroy_queue_ib(struct rdma_queue *q)
{
	printf("start: %s\n", __FUNCTION__);

	rdma_destroy_qp(q->cm_id);
	ibv_destroy_cq(q->cq);
}

static int create_queue_ib(struct rdma_queue *q)
{
	int ret;

	printf("start: %s\n", __FUNCTION__);

	if (!q->client->comp_channel)
	{
		q->client->comp_channel = ibv_create_comp_channel(q->cm_id->verbs);
		TEST_Z(q->client->comp_channel);
	}
	printf("completion channel created successfully.\n");

	q->cq = ibv_create_cq(q->client->rdev->verbs /* which device*/,
						  CQ_NUM_CQES /* maximum capacity*/,
						  NULL /* user context, not used here */,
						  q->client->comp_channel /* which IO completion channel */,
						  0 /* signaling vector, not used here*/);
	TEST_Z(q->cq);
	TEST_NZ(ibv_req_notify_cq(q->cq, 0));

	ret = create_qp(q);
	if (ret)
		goto out_destroy_ib_cq;

	printf("CQ created at %p with %d elements \n", q->cq, q->cq->cqe);
	return 0;

out_destroy_ib_cq:
	ib_free_cq(q->cq);
out_err:
	return ret;
}

static struct device *get_device(struct rdma_queue *q)
{
	struct device *rdev = NULL;

	if (!q->client->rdev)
	{
		rdev = malloc(sizeof(*rdev));
		if (!rdev)
		{
			printf("no memory for rdev\n");
			goto out_err;
		}

		rdev->verbs = q->cm_id->verbs;

		rdev->pd = ibv_alloc_pd(rdev->verbs);
		if (!rdev->pd)
		{
			printf("ibv_alloc_pd failed\n");
			goto out_free_dev;
		}

		q->client->rdev = rdev;
	}

	return q->client->rdev;

out_free_pd:
	ibv_dealloc_pd(rdev->pd);
out_free_dev:
	free(rdev);
out_err:
	return NULL;
}

static int create_qp(struct rdma_queue *queue)
{
	struct device *rdev = queue->client->rdev;
	struct ibv_qp_init_attr init_attr;
	int ret;

	printf("start: %s\n", __FUNCTION__);

	memset(&init_attr, 0, sizeof(init_attr));
	init_attr.cap.max_send_wr = QP_MAX_SEND_WR;
	init_attr.cap.max_recv_wr = QP_MAX_RECV_WR;
	init_attr.cap.max_recv_sge = 1;
	init_attr.cap.max_send_sge = 1;

	init_attr.qp_type = IBV_QPT_RC;
	init_attr.send_cq = queue->cq;
	init_attr.recv_cq = queue->cq;

	ret = rdma_create_qp(queue->cm_id, rdev->pd, &init_attr);
	if (ret)
	{
		printf("rdma_create_qp failed: %d\n", ret);
		return ret;
	}

	queue->qp = queue->cm_id->qp;
	return ret;
}

int process_rdma_cm_event(struct rdma_event_channel *echannel,
						  enum rdma_cm_event_type expected_event,
						  struct rdma_cm_event **cm_event)
{
	int ret = 1;
	ret = rdma_get_cm_event(echannel, cm_event);
	if (ret)
	{
		printf("Failed to retrieve a cm event, errno: %d \n",
			   -errno);
		return -errno;
	}
	/* lets see, if it was a good event */
	if (0 != (*cm_event)->status)
	{
		printf("CM event has non zero status: %d\n", (*cm_event)->status);
		ret = -((*cm_event)->status);
		/* important, we acknowledge the event */
		rdma_ack_cm_event(*cm_event);
		return ret;
	}
	/* if it was a good event, was it of the expected type */
	if ((*cm_event)->event != expected_event)
	{
		printf("Unexpected event received: %s [ expecting: %s ]",
			   rdma_event_str((*cm_event)->event),
			   rdma_event_str(expected_event));
		/* important, we acknowledge the event */
		rdma_ack_cm_event(*cm_event);
		return -1; // unexpected event :(
	}
	/* The caller must acknowledge the event */
	return ret;
}

/* Utils */

void die(const char *reason)
{
	fprintf(stderr, "%s - errno: %d\n", reason, errno);
	exit(EXIT_FAILURE);
}

static int parse_ipaddr(struct sockaddr_in *saddr, char *ip)
{
	uint8_t *addr = (uint8_t *)&saddr->sin_addr.s_addr;
	size_t buflen = strlen(ip);

	printf("start: %s\n", __FUNCTION__);

	if (buflen > INET_ADDRSTRLEN)
		return -EINVAL;
	if (in4_pton(ip, buflen, addr, '\0', NULL) == 0)
		return -EINVAL;
	saddr->sin_family = AF_INET;
	return 0;
}

struct ibv_mr *buffer_register(struct ibv_pd *pd,
							   void *addr, uint32_t length,
							   enum ibv_access_flags permission)
{
	struct ibv_mr *mr = NULL;
	if (!pd)
	{
		printf("Protection domain is NULL, ignoring \n");
		return NULL;
	}
	mr = ibv_reg_mr(pd, addr, length, permission);
	if (!mr)
	{
		printf("Failed to create mr on buffer, errno: %d \n", -errno);
		return NULL;
	}
	printf("Registered: %p , len: %u , stag: 0x%x \n",
		   mr->addr,
		   (unsigned int)mr->length,
		   mr->lkey);
	return mr;
}