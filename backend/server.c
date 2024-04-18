#include "common.h"

static struct rdma_cm_id *cm_server_id = NULL;
static struct rdma_event_channel *cm_event_channel = NULL;
static struct ibv_qp_init_attr qp_init_attr; // client queue pair attributes

static struct ibv_recv_wr client_recv_wr, *bad_client_recv_wr = NULL;
static struct ibv_send_wr server_send_wr, *bad_server_send_wr = NULL;
static struct ibv_sge client_recv_sge, server_send_sge;

static struct exchange_buffer server_buff, client_buff;
static struct per_client_resources *client_res = NULL;

// client resources struct
struct per_client_resources {
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_comp_channel *completion_channel;
    struct ibv_qp *qp;
    struct rdma_cm_id *client_id;
};

// per memory struct
struct memory_region_struct {
    char *memory_region;
    struct ibv_mr *memory_region_mr;
    unsigned long *mapping_table_start;
};

static void setup_client_resources(struct rdma_cm_id *cm_client_id) {
    /* Init the client resources */
    client_res = (struct per_client_resources *) malloc(sizeof(struct per_client_resources));
    if (!cm_client_id) {
        error("Client id is still NULL \n");
        return;
    }
    client_res->client_id = cm_client_id;

    /* Init the Protection Domain for the client */
    HANDLE(client_res->pd = ibv_alloc_pd(cm_client_id->verbs));
    debug("Protection domain (PD) allocated: %p \n", client_res->pd)

    /* Init the Completion Channel for the client */
    HANDLE(client_res->completion_channel = ibv_create_comp_channel(cm_client_id->verbs));
    debug("I/O completion event channel created: %p \n",
          client_res->completion_channel)

    /* Init the Completion Queue for the client */
    HANDLE(client_res->cq = ibv_create_cq(cm_client_id->verbs,
                                          CQ_CAPACITY,
                                          NULL,
                                          client_res->completion_channel,
                                          0));
    debug("Completion queue (CQ) created: %p with %d elements \n",
          client_res->cq, client_res->cq->cqe)

    /* Ask for the event for all activities in the completion queue */
    HANDLE_NZ(ibv_req_notify_cq(client_res->cq,
                                0));
    bzero(&qp_init_attr, sizeof qp_init_attr);
    qp_init_attr.cap.max_recv_sge = MAX_SGE; /* Maximum SGE per receive posting */
    qp_init_attr.cap.max_recv_wr = MAX_WR; /* Maximum receive posting capacity */
    qp_init_attr.cap.max_send_sge = MAX_SGE; /* Maximum SGE per send posting */
    qp_init_attr.cap.max_send_wr = MAX_WR; /* Maximum send posting capacity */
    qp_init_attr.qp_type = IBV_QPT_RC; /* QP type, RC = Reliable connection */
    qp_init_attr.recv_cq = client_res->cq;
    qp_init_attr.send_cq = client_res->cq;
    HANDLE_NZ(rdma_create_qp(client_res->client_id,
                             client_res->pd,
                             &qp_init_attr));
    client_res->qp = cm_client_id->qp;
    debug("Client QP created: %p \n", client_res->qp)
}

static void poll_for_completion_events(int num_wc) {
    struct ibv_wc wc;
    int total_wc = process_work_completion_events(client_res->completion_channel, &wc, num_wc);

    for (int i = 0 ; i < total_wc; i++) {
        if( (&(wc) + i)->opcode & IBV_WC_RECV ) {
            if ( client_buff.message->type == HELLO ) {
                show_exchange_buffer(client_buff.message);
            }
        }
    }
}

/* Establish connection with the client */
static void accept_conn(struct rdma_cm_id *cm_client_id) {
    struct rdma_conn_param conn_param;
    memset(&conn_param, 0, sizeof(conn_param));
    conn_param.initiator_depth = 3;
    conn_param.responder_resources = 3;

    HANDLE_NZ(rdma_accept(cm_client_id, &conn_param));
    debug("Wait for : RDMA_CM_EVENT_ESTABLISHED event \n")
}

/* Create a buffer for receiving client's message. Register ibv_reg_mr using client's pd,
 * message addr, length of message, and permissions for the buffer.
 * Post the client buffer as a Receive Request (RR) to the Work Queue (WQ)
 * */
static void post_recv_hello() {
    client_buff.message = malloc(sizeof(struct msg));
    HANDLE(client_buff.buffer = rdma_buffer_register(client_res->pd,
                                                     client_buff.message,
                                                     sizeof(struct msg),
                                                     (IBV_ACCESS_LOCAL_WRITE)));

    client_recv_sge.addr = (uint64_t) client_buff.buffer->addr;
    client_recv_sge.length = client_buff.buffer->length;
    client_recv_sge.lkey = client_buff.buffer->lkey;

    bzero(&client_recv_wr, sizeof(client_recv_wr));
    client_recv_wr.sg_list = &client_recv_sge;
    client_recv_wr.num_sge = 1;

    HANDLE_NZ(ibv_post_recv(client_res->qp,
                            &client_recv_wr,
                            &bad_client_recv_wr));

    info("Receive buffer for client OFFSET pre-posting is successful \n");
}

static void post_send_hello() {
    server_buff.message = malloc(sizeof(struct msg));
    server_buff.message->type = HELLO;
    server_buff.message->data.offset = client_buff.message->data.offset + 1;

    server_buff.buffer = rdma_buffer_register(client_res->pd,
                                              server_buff.message,
                                              sizeof(struct msg),
                                              (IBV_ACCESS_LOCAL_WRITE |
                                               IBV_ACCESS_REMOTE_READ |
                                               IBV_ACCESS_REMOTE_WRITE));

    show_exchange_buffer(server_buff.message);

    server_send_sge.addr = (uint64_t) server_buff.message;
    server_send_sge.length = (uint32_t) sizeof(struct msg);
    server_send_sge.lkey = server_buff.buffer->lkey;

    bzero(&server_send_wr, sizeof(server_send_wr));
    server_send_wr.sg_list = &server_send_sge;
    server_send_wr.num_sge = 1;
    server_send_wr.opcode = IBV_WR_SEND;
    server_send_wr.send_flags = IBV_SEND_SIGNALED;

    HANDLE_NZ(ibv_post_send(client_res->qp, &server_send_wr, &bad_server_send_wr));
    info("Send request with memory map ADDRESS is successful \n");
}

/*
 * Create Memory Buffer for the Server Client Communication.
 * TODO: What sort of memory are we transferring?
 */
static void build_memory_map(struct memory_region_struct *conn) {
    conn->memory_region = malloc(DATA_SIZE + (8 * (DATA_SIZE / BLOCK_SIZE)));
    for ( int i = 0; i < (DATA_SIZE / BLOCK_SIZE); i++ ) {
        strcpy(conn->memory_region + (i * BLOCK_SIZE) + (8 * (DATA_SIZE / BLOCK_SIZE)), "A");
    }
    print_memory_map(conn->memory_region);
    conn->memory_region_mr = rdma_buffer_register(client_res->pd,
                                                  conn->memory_region,
                                                  DATA_SIZE + (8 * (DATA_SIZE / BLOCK_SIZE)),
                                                  (IBV_ACCESS_LOCAL_WRITE |
                                                   IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE));
    conn->mapping_table_start = (unsigned long *) conn->memory_region;
    debug("Memory Map address - %p\n", conn->mapping_table_start)
}

/*
 * If the client sent the OFFSET, server must send the memory region address.
 * Initialize the server buffer with the memory_region and add it the Send Request Queue (SR)
 * as a Work Request (WR)
 */
static void post_send_memory_map(struct memory_region_struct *_region) {
    struct ibv_wc wc;


    server_buff.message = malloc(sizeof(struct msg));
    server_buff.message->type = FRAME;

    memcpy(&server_buff.message->data.mr, _region->memory_region_mr, sizeof(struct ibv_mr));
    server_buff.message->data.mr.addr = (void *) (_region->memory_region);

    server_buff.buffer = rdma_buffer_register(client_res->pd,
                                              server_buff.message,
                                              sizeof(struct msg),
                                              (IBV_ACCESS_LOCAL_WRITE |
                                               IBV_ACCESS_REMOTE_READ |
                                               IBV_ACCESS_REMOTE_WRITE));

    info("Sending ADDRESS... \n");
    show_exchange_buffer(server_buff.message);

    server_send_sge.addr = (uint64_t) server_buff.message;
    server_send_sge.length = (uint32_t) sizeof(struct msg);
    server_send_sge.lkey = server_buff.buffer->lkey;

    bzero(&server_send_wr, sizeof(server_send_wr));
    server_send_wr.sg_list = &server_send_sge;
    server_send_wr.num_sge = 1;
    server_send_wr.opcode = IBV_WR_SEND;
    server_send_wr.send_flags = IBV_SEND_SIGNALED;

    HANDLE_NZ(ibv_post_send(client_res->qp, &server_send_wr, &bad_server_send_wr));
    info("Send request with memory map ADDRESS is successful \n");
}

static int established_connection() {
    struct sockaddr_in remote_sockaddr;

    /* Extract client socket information */
    memcpy(&remote_sockaddr /* where to save */,
           rdma_get_peer_addr(client_res->client_id) /* gives you remote sockaddr */,
           sizeof(struct sockaddr_in) /* max size */);

    info("A new connection is accepted from %s \n",
         inet_ntoa(remote_sockaddr.sin_addr));
}

static int disconnect_and_cleanup(struct memory_region_struct *conn) {
    int ret = -1;
    /* Destroy QP */
    rdma_destroy_qp(client_res->client_id);

    /* Destroy CQ */
    ret = ibv_destroy_cq(client_res->cq);
    if (ret) {
        error("Failed to destroy completion queue cleanly, %d \n", -errno);
    }
    /* Destroy completion channel */
    ret = ibv_destroy_comp_channel(client_res->completion_channel);
    if (ret) {
        error("Failed to destroy completion channel cleanly, %d \n", -errno);
    }
    /* Destroy rdma server id */
    ret = rdma_destroy_id(cm_server_id);
    if (ret) {
        error("Failed to destroy server id cleanly, %d \n", -errno);
    }
    rdma_destroy_event_channel(cm_event_channel);

    /* Destroy client cm id */
    ret = rdma_destroy_id(client_res->client_id);
    if (ret) {
        error("Failed to destroy client id cleanly, %d \n", -errno);
    }
    free(conn);
    free(client_res);
    printf("Server shut-down is complete \n");
    return 0;
}

int wait_for_update(struct memory_region_struct* _region, int offset) {
    char * previousValue = "A";

    while(1) {
        char* str = _region->memory_region + ( 8 * (DATA_SIZE / BLOCK_SIZE)) + (2 * BLOCK_SIZE);
        if (strcmp(previousValue, str) != 0) {
            info("Previous String 2: %s\n", previousValue);
            info("Updating %s to new string %s\n", previousValue, str);

            previousValue = strdup(str);
            print_memory_map(_region->memory_region);
        }

    }
}

static int wait_for_event() {
    int ret;

    struct rdma_cm_event *received_event = NULL;
    struct memory_region_struct *_region = NULL;
    pthread_t thread1, thread2;

    while (rdma_get_cm_event(cm_event_channel, &received_event) == 0) {
        /* Initialize the received event */
        struct rdma_cm_event cm_event;
        memcpy(&cm_event, received_event, sizeof(*received_event));
        info("%s event received \n", rdma_event_str(cm_event.event));

        /* SWITCH case to check what type of event was received */
        switch (cm_event.event) {

            /* Initially Server receives and Client Connect Request */
            case RDMA_CM_EVENT_CONNECT_REQUEST:
                _region = (struct memory_region_struct *) malloc(sizeof(struct memory_region_struct *));
                rdma_ack_cm_event(received_event); //Ack the event
                setup_client_resources(cm_event.id); // send a recv req for client_metadata
                //build_memory_map(_region);
                post_recv_hello();
                accept_conn(cm_event.id);
                break;

            /*  After the client establishes the connection */
            case RDMA_CM_EVENT_ESTABLISHED:
                rdma_ack_cm_event(received_event);
                established_connection();
                poll_for_completion_events(1);
                post_send_hello();
                poll_for_completion_events(1);
                post_recv_hello();
                poll_for_completion_events(1);
                post_send_memory_map(_region);
                //poll_for_completion_events(1);
                //wait_for_update(_region, 2);
                break;

            /* Disconnect and Cleanup */
            case RDMA_CM_EVENT_DISCONNECTED:
                rdma_ack_cm_event(received_event);
                disconnect_and_cleanup(_region);
                break;
            default:
                error("Event not found %s", (char *) cm_event.event);
                return -1;
        }
    }
    return ret;
}

static void start_rdma_server(struct sockaddr_in *server_socket_addr) {
    // Create RDMA Event Channel
    HANDLE(cm_event_channel = rdma_create_event_channel());

    // Using the RDMA EC, create ID to track communication information
    HANDLE_NZ(rdma_create_id(cm_event_channel, &cm_server_id, NULL, RDMA_PS_TCP));

    // Using the ID, bind the socket information
    HANDLE_NZ(rdma_bind_addr(cm_server_id, (struct sockaddr *) server_socket_addr));

    // Server Listening...
    HANDLE_NZ(rdma_listen(cm_server_id, 8));
    info("Server is listening successfully at: %s , port: %d \n",
         inet_ntoa(server_socket_addr->sin_addr),
         ntohs(server_socket_addr->sin_port));
}

int main(int argc, char **argv) {
    /* struct for Internet Socket Address */
    struct sockaddr_in server_socket_addr;

    /* set the struct variable to 0 */
    bzero(&server_socket_addr, sizeof(server_socket_addr));

    /* IP protocol family */
    server_socket_addr.sin_family = AF_INET;

    /* ANY address */
    server_socket_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    int ret, option;
    while ((option = getopt(argc, argv, "a:p:")) != -1) {
        switch (option) {
            case 'a':
                ret = get_addr(optarg, (struct sockaddr *) &server_socket_addr);
                if (ret) {
                    error("Invalid IP");
                    return ret;
                }
                break;
            case 'p':
                server_socket_addr.sin_port = htons(strtol(optarg, NULL, 0));
                break;
            default:
                break;
        }
    }

    if (!server_socket_addr.sin_port) {
        server_socket_addr.sin_port = htons(DEFAULT_RDMA_PORT);
    }

    /* Start the rdma server with the socket information */
    start_rdma_server(&server_socket_addr);

    wait_for_event();
    return 0;
}
