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

#define bench_start()                            \
	cycles_low_start = 0, cycles_high_start = 0; \
	cycles_low_end = 0, cycles_high_end = 0;     \
	helpers::timer_start(&cycles_high_start, &cycles_low_start);

#define bench_end(title)                                   \
	helpers::timer_end(&cycles_high_end, &cycles_low_end); \
	printf("%s: \t\t %ld\n", #title, helpers::get_elapsed_cycles(cycles_high_start, cycles_low_start, cycles_high_end, cycles_low_end));

#include <arpa/inet.h>
#include <sys/socket.h>
#include <rdma/rdma_cma.h>

#include "helpers.hpp"
#include "rdma_client.hpp"

const char *RDMA_SERVER_IP = "128.110.218.157"; // IP that the RDMA server is listening on
const size_t BUFFER_SIZE = 1 << 16;				// MAX DATA_LEN

#define CONNECTION_TIMEOUT_MS 2000
#define QP_MAX_RECV_WR 4
#define QP_MAX_SEND_WR (4096)
#define CQ_NUM_CQES (QP_MAX_SEND_WR)

/* Private data passed over rdma_cm protocol */
typedef struct
{
	uint64_t baseaddr; /* Remote buffer address for RDMA */
	uint32_t key;	   /* Remote key for RDMA */
} memregion_t;

struct rdma_device
{
	struct ibv_pd *pd;
	struct ibv_context *verbs;
};

struct rdma_queue
{
	struct ibv_qp *qp;
	struct ibv_cq *cq;

	struct rdma_client *client;

	void *buffer;
	struct ibv_mr *localmr;
	memregion_t *servermr;

	struct rdma_cm_id *cm_id;
};

struct rdma_client
{
	int num_queues;
	struct rdma_event_channel *ec;

	struct rdma_device *rdev; // TODO: move this to queue
	struct rdma_queue *queues;

	struct ibv_comp_channel *comp_channel;

	union
	{
		struct sockaddr addr;
		struct sockaddr_in addr_in;
	};
};

static int start_client(struct rdma_client **c, const char *sip, int num_connections);
static int init_queues(struct rdma_client *client);
static void stop_queue(struct rdma_queue *q);
static void free_queue(struct rdma_queue *q);
static int init_queue(struct rdma_client *client, int idx);
static int connect_to_server(struct rdma_queue *q);
static void destroy_queue_ib(struct rdma_queue *q);
static int create_queue_ib(struct rdma_queue *q);
static struct rdma_device *get_device(struct rdma_queue *q);
static int create_qp(struct rdma_queue *queue);
static int process_rdma_cm_event(struct rdma_event_channel *echannel,
								 enum rdma_cm_event_type expected_event,
								 struct rdma_cm_event **cm_event);
static void die(const char *reason);
static int parse_ipaddr(struct sockaddr_in *saddr, const char *ip);

/* ====================== APIs ====================== */
struct rdma_client *start_rdma_client()
{
	struct rdma_client *gclient = NULL;
	printf("\n* AIFM RDMA BACKEND *\n");

	TEST_NZ(start_client(&gclient, RDMA_SERVER_IP, NUM_QUEUES));
	printf("%d queues initialized successfully\n\n", NUM_QUEUES);
	return gclient;
}

int rdma_read(rdma_queue_t *queue, uint64_t offset, uint16_t data_len, uint8_t *data_buf)
{
	// unsigned total_cycles_low_start = 0, total_cycles_high_start = 0;
	// unsigned total_cycles_low_end = 0, total_cycles_high_end = 0;
	// helpers::timer_start(&total_cycles_high_start, &total_cycles_low_start);

	// unsigned cycles_low_start, cycles_high_start;
	// unsigned cycles_low_end, cycles_high_end;
	// bench_start();

	struct ibv_wc wc;
	struct ibv_sge client_send_sge;
	struct ibv_send_wr client_send_wr, *bad_client_send_wr = NULL;
	int ret = -1;

	client_send_sge.addr = (uint64_t)queue->localmr->addr;
	client_send_sge.length = (uint32_t)data_len;
	client_send_sge.lkey = queue->localmr->lkey;

	bzero(&client_send_wr, sizeof(client_send_wr));
	client_send_wr.sg_list = &client_send_sge;
	client_send_wr.num_sge = 1;
	client_send_wr.opcode = IBV_WR_RDMA_READ;
	client_send_wr.send_flags = IBV_SEND_SIGNALED;
	client_send_wr.wr.rdma.rkey = queue->servermr->key;
	client_send_wr.wr.rdma.remote_addr = queue->servermr->baseaddr + offset;

	ret = ibv_post_send(queue->qp,
						&client_send_wr,
						&bad_client_send_wr);
	if (ret)
	{
		printf("failed to post rdma read req, error: %d\n", -errno);
		return ret;
	}

	// bench_end(build wr);
	// bench_start();

	do
	{
		ret = ibv_poll_cq(queue->cq, 1, &wc);
	} while (ret == 0);

	if (wc.status != IBV_WC_SUCCESS)
	{
		fprintf(stderr, "failed status %s (%d) for wr_id %d\n",
				ibv_wc_status_str(wc.status),
				wc.status, (int)wc.wr_id);
		return -1;
	}

	// bench_end(polling cq);
	// bench_start();

	memcpy(data_buf, queue->localmr->addr, data_len);

	// bench_end(memcopy);
	// bench_start();

	ibv_ack_cq_events(queue->cq, 1);

	// bench_end(ack_cq);
	// printf("\n");

	// helpers::timer_end(&total_cycles_high_end, &total_cycles_low_end);
	// printf("%s total: \t %ld\n\n", __FUNCTION__,
	// 	   helpers::get_elapsed_cycles(
	// 		   total_cycles_high_start, total_cycles_low_start, total_cycles_high_end, total_cycles_low_end));

	return 0;
}

int rdma_write(rdma_queue_t *queue, uint64_t offset, uint16_t data_len, const uint8_t *data_buf)
{
	// unsigned total_cycles_low_start = 0, total_cycles_high_start = 0;
	// unsigned total_cycles_low_end = 0, total_cycles_high_end = 0;
	// helpers::timer_start(&total_cycles_high_start, &total_cycles_low_start);

	// unsigned cycles_low_start, cycles_high_start;
	// unsigned cycles_low_end, cycles_high_end;
	// bench_start();

	struct ibv_wc wc;
	struct ibv_sge client_send_sge;
	struct ibv_send_wr client_send_wr, *bad_client_send_wr = NULL;
	int ret = -1;

	memcpy(queue->localmr->addr, data_buf, data_len);

	// bench_end(memcopy);
	// bench_start();

	client_send_sge.addr = (uint64_t)queue->localmr->addr;
	client_send_sge.length = (uint32_t)data_len;
	client_send_sge.lkey = queue->localmr->lkey;
	bzero(&client_send_wr, sizeof(client_send_wr));

	client_send_wr.sg_list = &client_send_sge;
	client_send_wr.num_sge = 1;
	client_send_wr.opcode = IBV_WR_RDMA_WRITE;
	client_send_wr.send_flags = IBV_SEND_SIGNALED;
	client_send_wr.wr.rdma.rkey = queue->servermr->key;
	client_send_wr.wr.rdma.remote_addr = queue->servermr->baseaddr + offset;

	ret = ibv_post_send(queue->qp,
						&client_send_wr,
						&bad_client_send_wr);
	if (ret)
	{
		printf("failed to post rdma write req, error: %d\n", -errno);
		return ret;
	}

	// bench_end(build wr);
	// bench_start();

	do
	{
		ret = ibv_poll_cq(queue->cq, 1, &wc);
	} while (ret == 0);

	if (wc.status != IBV_WC_SUCCESS)
	{
		fprintf(stderr, "failed status %s (%d) for wr_id %d\n",
				ibv_wc_status_str(wc.status),
				wc.status, (int)wc.wr_id);
		return -1;
	}

	// bench_end(polling cq);
	// bench_start();

	ibv_ack_cq_events(queue->cq, 1);

	// bench_end(ack_cq);
	// printf("\n");

	// helpers::timer_end(&total_cycles_high_end, &total_cycles_low_end);
	// printf("%s total: \t %ld\n\n", __FUNCTION__,
	// 	   helpers::get_elapsed_cycles(
	// 		   total_cycles_high_start, total_cycles_low_start, total_cycles_high_end, total_cycles_low_end));

	return 0;
}

int destroy_client(struct rdma_client *client)
{
	printf("start: %s\n", __FUNCTION__);
	struct rdma_queue *queue;

	for (int i = 0; i < client->num_queues; i++)
	{
		queue = &client->queues[i];
		ibv_dereg_mr(queue->localmr);
		rdma_disconnect(queue->cm_id);
		destroy_queue_ib(queue);
		rdma_destroy_id(queue->cm_id);
		free(queue->servermr);
		free(queue->buffer);
	}

	if (client->rdev)
	{
		ibv_dealloc_pd(client->rdev->pd);
		free(client->rdev);
		client->rdev = NULL;
	}

	ibv_destroy_comp_channel(client->comp_channel);
	rdma_destroy_event_channel(client->ec);

	free(client->queues);
	free(client);
	printf("RDMA server cleaned up\n\n");
	return 0;
}

rdma_queue_t *rdma_get_queue(struct rdma_client *client, int idx)
{
	return &client->queues[idx];
}

/* ===================== RDMA Helpers ===================== */

static int start_client(struct rdma_client **c, const char *sip, int num_connections)
{
	struct rdma_client *client;

	*c = (struct rdma_client *)malloc(sizeof(struct rdma_client));
	TEST_Z(*c);
	client = *c;
	memset(client, 0, sizeof(struct rdma_client));

	client->num_queues = num_connections;

	client->ec = rdma_create_event_channel();
	TEST_Z(client->ec);

	client->queues = (struct rdma_queue *)malloc(sizeof(struct rdma_queue) * client->num_queues);
	TEST_Z(client->queues);
	memset(client->queues, 0, sizeof(struct rdma_queue) * client->num_queues);

	printf("will try to connect to %s:%d\n", sip, DEFAULT_RDMA_PORT);
	printf("target num of queues: %d\n", client->num_queues);

	TEST_NZ(parse_ipaddr(&(client->addr_in), sip));
	client->addr_in.sin_port = htons(DEFAULT_RDMA_PORT);

	return init_queues(client);
}

static int init_queues(struct rdma_client *client)
{
	int ret, i;
	for (i = 0; i < client->num_queues; ++i)
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
	ibv_destroy_cq(q->cq);
	rdma_destroy_id(q->cm_id);
	if (q->servermr)
	{
		free(q->servermr);
	}
}

static int init_queue(struct rdma_client *client, int idx)
{
	struct rdma_queue *queue;
	struct rdma_cm_event *event = NULL;
	struct rdma_device *rdev;
	int ret;

	// printf("start: %s[%d]\n", __FUNCTION__, idx);

	queue = &client->queues[idx];
	queue->client = client;

	TEST_NZ(rdma_create_id(client->ec, &queue->cm_id, NULL, RDMA_PS_TCP));

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

	// printf("RDMA address is resolved.\n");

	ret = rdma_resolve_route(queue->cm_id, CONNECTION_TIMEOUT_MS);
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

	// printf("RDMA route is resolved.\n");

	rdev = get_device(queue);
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

	/* Fetch private data from established event. */
	ret = process_rdma_cm_event(client->ec,
								RDMA_CM_EVENT_ESTABLISHED,
								&event);
	if (ret)
	{
		printf("waiting for RDMA_CM_EVENT_ESTABLISHED failed\n");
		goto out_destroy_cm_id;
	}

	if (!queue->servermr)
	{
		const memregion_t *rmr = (memregion_t *)event->param.conn.private_data;
		TEST_Z(queue->servermr = (memregion_t *)malloc(sizeof(memregion_t)));
		queue->servermr->baseaddr = rmr->baseaddr;
		queue->servermr->key = rmr->key;
	}
	// printf("servermr baseaddr= %ld, key= %d\n", queue->servermr->baseaddr, queue->servermr->key);

	/* Prepare local buffer and register mr. */
	queue->buffer = malloc(BUFFER_SIZE);
	TEST_Z(queue->buffer);

	queue->localmr = ibv_reg_mr(rdev->pd,
								(void *)queue->buffer,
								(uint32_t)BUFFER_SIZE,
								(IBV_ACCESS_LOCAL_WRITE |
								 IBV_ACCESS_REMOTE_WRITE |
								 IBV_ACCESS_REMOTE_READ));
	TEST_Z(queue->localmr);

	TEST_NZ(rdma_ack_cm_event(event));

	// printf("queue[%d] initialized successfully\n", idx);
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
	rdma_destroy_qp(q->cm_id);
	/* NOTE(Qing): There is a bug here that sporadically ibv_destroy_cq never returns. */
	// ibv_destroy_cq(q->cq); // May cause mem leak but should be fine, we are terminating the client anyway.
}

static int create_queue_ib(struct rdma_queue *q)
{
	int ret;

	if (!q->client->comp_channel)
	{
		q->client->comp_channel = ibv_create_comp_channel(q->cm_id->verbs);
		TEST_Z(q->client->comp_channel);
	}

	q->cq = ibv_create_cq(q->cm_id->verbs /* which device*/,
						  CQ_NUM_CQES /* maximum capacity*/,
						  NULL /* user context, not used here */,
						  q->client->comp_channel /* which IO completion channel */,
						  0 /* signaling vector, not used here*/);

	TEST_Z(q->cq);
	// TEST_NZ(ibv_req_notify_cq(q->cq, 0));
	// printf("completion queue created at %p with %d elements \n", q->cq, q->cq->cqe);

	ret = create_qp(q);
	if (ret)
		goto out_destroy_ib_cq;

	return 0;

out_destroy_ib_cq:
	ibv_destroy_cq(q->cq);
	return ret;
}

static struct rdma_device *get_device(struct rdma_queue *q)
{
	struct rdma_device *rdev = NULL;

	if (!q->client->rdev)
	{
		TEST_Z(rdev = (struct rdma_device *)malloc(sizeof(struct rdma_device)));

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

out_free_dev:
	free(rdev);
	return NULL;
}

static int create_qp(struct rdma_queue *queue)
{
	struct rdma_device *rdev = queue->client->rdev;
	struct ibv_qp_init_attr init_attr;
	int ret;

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

static int process_rdma_cm_event(struct rdma_event_channel *echannel,
								 enum rdma_cm_event_type expected_event,
								 struct rdma_cm_event **cm_event)
{
	int ret = 1;
	ret = rdma_get_cm_event(echannel, cm_event);
	if (ret)
	{
		printf("failed to retrieve a cm event, errno: %d \n",
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
		printf("unexpected event received: %s [ expecting: %s ]",
			   rdma_event_str((*cm_event)->event),
			   rdma_event_str(expected_event));
		/* important, we acknowledge the event */
		rdma_ack_cm_event(*cm_event);
		return -1; // unexpected event :(
	}
	/* The caller must acknowledge the event */
	return ret;
}

/* ===================== Utils ==================== */

static void die(const char *reason)
{
	fprintf(stderr, "%s - errno: %d\n", reason, errno);
	exit(EXIT_FAILURE);
}

static int parse_ipaddr(struct sockaddr_in *saddr, const char *ip)
{
	uint8_t *addr = (uint8_t *)&saddr->sin_addr.s_addr;
	size_t buflen = strlen(ip);

	if (buflen > INET_ADDRSTRLEN)
		return -EINVAL;
	if (inet_pton(AF_INET, ip, addr) == 0)
		return -EINVAL;
	saddr->sin_family = AF_INET;
	return 0;
}