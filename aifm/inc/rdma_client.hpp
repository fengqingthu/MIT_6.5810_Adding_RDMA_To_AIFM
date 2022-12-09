#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <getopt.h>

#include <infiniband/verbs.h>

#define DEFAULT_RDMA_PORT (20886)				// Default port where the RDMA server is listening
#define NUM_QUEUES (20)							// Number of most possible concurrent calls

struct rdma_queue;
typedef struct rdma_queue rdma_queue_t;

struct rdma_client;

struct rdma_client *start_rdma_client();
int destroy_client(struct rdma_client *client);
rdma_queue_t *rdma_get_queue(struct rdma_client *client, int idx);
int rdma_read(rdma_queue_t *queue, uint64_t offset, uint16_t data_len, uint8_t *data_buf);
int rdma_write(rdma_queue_t *queue, uint64_t offset, uint16_t data_len, const uint8_t *data_buf);