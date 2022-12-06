#ifndef RDMA_CLIENT_H
#define RDMA_CLIENT_H

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
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>

#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>

#define SERVER_PORT (20886); // DEFAULT_RDMA_PORT
#define NUM_QUEUES (20)		 // Should be the same as server side number of queues

/* Private data passed over rdma_cm protocol */
typedef struct
{
	uint64_t baseaddr; /* Remote buffer address for RDMA */
	uint32_t key;	   /* Remote key for RDMA */
} memregion_t;

struct device
{
	struct ibv_pd *pd;
	struct ibv_context *verbs;
};

struct rdma_queue
{
	struct ibv_qp *qp;
	struct ibv_cq *cq;

	struct rdma_client *client;
	memregion_t *servermr;

	struct rdma_cm_id *cm_id;
};

typedef struct rdma_queue rdma_queue_t;
struct rdma_client
{
	int num_queues;
	struct rdma_event_channel *ec;

	struct device *rdev; // TODO: move this to queue
	struct rdma_queue *queues;

	struct ibv_comp_channel *comp_channel;

	union
	{
		struct sockaddr addr;
		struct sockaddr_in addr_in;
	};
};

extern static struct rdma_client *start_rdma_client(uint32_t sip, int num_connections);
extern static int destroy_client(struct rdma_client *client);
extern rdma_queue_t *rdma_get_queues(struct rdma_client *client);
extern int rdma_read(rdma_queue_t *queue, uint64_t offset, uint16_t data_len, uint8_t *data_buf);
extern int rdma_write(rdma_queue_t *queue, uint64_t offset, uint16_t data_len, uint8_t *data_buf);

static int start_client(struct rdma_client **c, uint32_t sip);
static int init_queues(struct rdma_client *client);
static void stop_queue(struct rdma_queue *q);
static void free_queue(struct rdma_queue *q);
static int init_queue(struct rdma_client *client, int idx);
static int connect_to_server(struct rdma_queue *q);
static void destroy_queue_ib(struct rdma_queue *q);
static int create_queue_ib(struct rdma_queue *q);
static struct device *get_device(struct rdma_queue *q);
static int create_qp(struct rdma_queue *queue);
static int process_rdma_cm_event(struct rdma_event_channel *echannel,
								 enum rdma_cm_event_type expected_event,
								 struct rdma_cm_event **cm_event);
static void die(const char *reason);

#endif /* RDMA_CLIENT_H */