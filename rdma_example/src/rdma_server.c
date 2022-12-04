#include "rdma_server.h"

/* Global server instance. */
static struct rdma_server *gserver = NULL;

static int start_rdma_server()
{
	struct sockaddr_in addr = {};
	struct rdma_cm_event *event = NULL;
	addr.sin_family = AF_INET;				  /* standard IP NET address */
	addr.sin_port = htons(DEFAULT_RDMA_PORT); /* use default port */

	/* Setup RDMA resources and start listening. */
	TEST_NZ(alloc_server());
	TEST_Z(gserver->ec = rdma_create_event_channel());
	TEST_NZ(rdma_create_id(gserver->ec, &gserver->listener, NULL, RDMA_PS_TCP));
	TEST_NZ(rdma_bind_addr(gserver->listener, (struct sockaddr *)&addr));
	TEST_NZ(rdma_listen(gserver->listener, NUM_QUEUES + 1));
	printf("listening on port %d.\n", ntohs(rdma_get_src_port(gserver->listener)));

	/* Wait for client connections. */
	for (unsigned int i = 0; i < NUM_QUEUES; ++i)
	{
		printf("\nwaiting for queue connection: %d\n", i);
		struct queue *q = &gserver->queues[i];

		// handle connection requests
		while (rdma_get_cm_event(gserver->ec, &event) == 0)
		{
			struct rdma_cm_event event_copy;

			memcpy(&event_copy, event, sizeof(*event));
			rdma_ack_cm_event(event);

			if (on_event(&event_copy) || q->state == CONNECTED)
				break;
		}
	}

	printf("done connecting all queues\n");

	return 0;
}

static int destroy_server()
{
	rdma_destroy_event_channel(gserver->ec);
	rdma_destroy_id(gserver->listener);
	TEST_Z(gserver->dev);
	ibv_dereg_mr(gserver->mr_buffer);
	free(gserver->buffer);
	ibv_dealloc_pd(gserver->dev->pd);
	free(gserver->dev);
	gserver->dev = NULL;

	// TODO(Qing): free queues.
	return 0;
}

static void die(const char *reason)
{
	fprintf(stderr, "%s - errno: %d\n", reason, errno);
	exit(EXIT_FAILURE);
}

static int alloc_server()
{
	/* Initialize server struct. */
	gserver = (struct rdma_server *)malloc(sizeof(struct rdma_server));
	TEST_Z(gserver);
	memset(gserver, 0, sizeof(struct rdma_server));

	gserver->queue_ctr = 0;

	gserver->ec = (struct rdma_event_channel *)malloc(sizeof(struct rdma_event_channel));
	TEST_Z(gserver->ec);
	memset(gserver->ec, 0, sizeof(struct rdma_event_channel));
	gserver->listener = (struct rdma_cm_id *)malloc(sizeof(struct rdma_cm_id));
	TEST_Z(gserver->listener);
	memset(gserver->listener, 0, sizeof(struct rdma_cm_id));

	gserver->queues = (struct queue *)malloc(sizeof(struct queue) * NUM_QUEUES);
	TEST_Z(gserver->queues);
	memset(gserver->queues, 0, sizeof(struct queue) * NUM_QUEUES);
	for (unsigned int i = 0; i < NUM_QUEUES; ++i)
	{
		gserver->queues[i].server = gserver;
		gserver->queues[i].state = INIT;
	}

	return 0;
}

static struct device *get_device(struct queue *q)
{
	struct device *dev = NULL;

	if (!q->server->dev)
	{
		dev = (struct device *)malloc(sizeof(*dev));
		TEST_Z(dev);
		dev->verbs = q->cm_id->verbs;
		TEST_Z(dev->verbs);
		dev->pd = ibv_alloc_pd(dev->verbs);
		TEST_Z(dev->pd);

		struct rdma_server *server = q->server;
		server->buffer = malloc(BUFFER_SIZE);
		TEST_Z(server->buffer);

		TEST_Z(server->mr_buffer = ibv_reg_mr(
				   dev->pd,
				   server->buffer,
				   BUFFER_SIZE,
				   IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ));

		printf("registered memory region of %zu bytes\n", BUFFER_SIZE);
		printf("baseaddr= %ld, key= %d\n", (uint64_t)server->mr_buffer->addr, server->mr_buffer->rkey);
		q->server->dev = dev;
	}

	return q->server->dev;
}

static void create_qp(struct queue *q)
{
	struct ibv_qp_init_attr qp_attr = {};

	qp_attr.send_cq = q->cq;
	qp_attr.recv_cq = q->cq;
	qp_attr.qp_type = IBV_QPT_RC;
	qp_attr.cap.max_send_wr = 10;
	qp_attr.cap.max_recv_wr = 10;
	qp_attr.cap.max_send_sge = 1;
	qp_attr.cap.max_recv_sge = 1;

	TEST_NZ(rdma_create_qp(q->cm_id, q->server->dev->pd, &qp_attr));
	q->qp = q->cm_id->qp;
}

static int on_connect_request(struct rdma_cm_id *id, struct rdma_conn_param *param)
{

	struct rdma_conn_param cm_params = {};
	struct ibv_device_attr attrs = {};
	struct queue *q = &gserver->queues[gserver->queue_ctr++];
	memregion_t servermr = {};

	TEST_Z(q->state == INIT);
	printf("%s\n", __FUNCTION__);

	id->context = q;
	q->cm_id = id;

	struct device *dev = get_device(q);
	create_qp(q);

	TEST_NZ(ibv_query_device(dev->verbs, &attrs));

	printf("attrs: max_qp=%d, max_qp_wr=%d, max_cq=%d max_cqe=%d \
          max_qp_rd_atom=%d, max_qp_init_rd_atom=%d\n",
		   attrs.max_qp,
		   attrs.max_qp_wr, attrs.max_cq, attrs.max_cqe,
		   attrs.max_qp_rd_atom, attrs.max_qp_init_rd_atom);

	printf("param attrs: initiator_depth=%d responder_resources=%d\n",
		   param->initiator_depth, param->responder_resources);

	/* Send the server buffer metadata using the connection private data. */
	servermr.baseaddr = (uint64_t)gserver->mr_buffer->addr;
	servermr.key = gserver->mr_buffer->rkey;

	cm_params.private_data = &servermr;
	cm_params.private_data_len = sizeof(servermr);
	// the following should hold for initiator_depth:
	// initiator_depth <= max_qp_init_rd_atom, and
	// initiator_depth <= param->initiator_depth
	cm_params.initiator_depth = param->initiator_depth;
	// the following should hold for responder_resources:
	// responder_resources <= max_qp_rd_atom, and
	// responder_resources >= param->responder_resources
	cm_params.responder_resources = param->responder_resources;
	cm_params.rnr_retry_count = param->rnr_retry_count;
	cm_params.flow_control = param->flow_control;

	TEST_NZ(rdma_accept(q->cm_id, &cm_params));

	return 0;
}

static int on_connection(struct queue *q)
{
	printf("%s\n", __FUNCTION__);
	struct rdma_server *server = q->server;

	TEST_Z(q->state == INIT);

	/* Send server side mr metadata on the first established connection. */
	/* if (q == &server->queues[0])
	{
		struct ibv_send_wr wr = {};
		struct ibv_send_wr *bad_wr = NULL;
		struct ibv_sge sge = {};
		struct memregion servermr = {};

		printf("connected. sending memory region info.\n");
		printf("MR key=%u base vaddr=%p\n", server->mr_buffer->rkey, server->mr_buffer->addr);

		servermr.baseaddr = (uint64_t)server->mr_buffer->addr;
		servermr.key = server->mr_buffer->rkey;

		wr.opcode = IBV_WR_SEND;
		wr.sg_list = &sge;
		wr.num_sge = 1;
		wr.send_flags = IBV_SEND_SIGNALED | IBV_SEND_INLINE;

		sge.addr = (uint64_t)&servermr;
		sge.length = sizeof(servermr);

		TEST_NZ(ibv_post_send(q->qp, &wr, &bad_wr));

		// TODO: poll here
	} */

	q->state = CONNECTED;
	return 0;
}

static int on_disconnect(struct queue *q)
{
	printf("%s\n", __FUNCTION__);

	if (q->state == CONNECTED)
	{
		q->state = INIT;
		rdma_destroy_qp(q->cm_id);
		rdma_destroy_id(q->cm_id);
	}

	return 0;
}

static int on_event(struct rdma_cm_event *event)
{
	printf("%s\n", __FUNCTION__);
	struct queue *q = (struct queue *)event->id->context;

	switch (event->event)
	{
	case RDMA_CM_EVENT_CONNECT_REQUEST:
		return on_connect_request(event->id, &event->param.conn);
	case RDMA_CM_EVENT_ESTABLISHED:
		return on_connection(q);
	case RDMA_CM_EVENT_DISCONNECTED:
		on_disconnect(q);
		return 1;
	default:
		printf("unknown event: %s\n", rdma_event_str(event->event));
		return 1;
	}
}

int main(int argc, char **argv)
{
	return start_rdma_server();
}