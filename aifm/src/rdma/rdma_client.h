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

extern static const int SERVER_PORT = 20886; // DEFAULT_RDMA_PORT
extern static const int NUM_QUEUES = 10;	 // Number of cores.

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

	struct rdma_cm_id *cm_id;
	enum
	{
		INIT,
		CONNECTED
	} state;
};

struct rdma_client
{
	struct rdma_event_channel *ec;

	struct device *rdev; // TODO: move this to queue
	struct rdma_queue *queues;
	memregion_t servermr;
	struct ibv_comp_channel *comp_channel;

	union
	{
		struct sockaddr addr;
		struct sockaddr_in addr_in;
	};

	union
	{
		struct sockaddr srcaddr;
		struct sockaddr_in srcaddr_in;
	};
};

static struct rdma_client *start_rdma_client(char *sip);
static void destroy_client(struct rdma_client *client);

#endif /* RDMA_CLIENT_H */