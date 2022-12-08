/**
 * This RDMA server code is heavily adapated from fastswap.
 * (https://github.com/clusterfarmem/fastswap)
 */

#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rdma_client.hpp"

int start_rdma_server();
int destroy_server();