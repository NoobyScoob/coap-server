#include "mbed.h"
#include <string>
#include "sn_coap_protocol.h"
#include "sn_coap_header.h"

#define MAX_PKT_SIZE 1024
#define BLINKING_RATE 500ms
#define PROXY_SERVER_ADDR "10.0.0.139"
#define PROXY_SERVER_PORT 5688
#define SERVER_PORT 5683
#define MAX_TEMP 70
#define MIN_TEMP 60

UDPSocket sndSocket;
UDPSocket rcvSocket;
Thread recv_frm_thread;

struct coap_s* coap_handler;
coap_version_e coap_version = COAP_VERSION_1;
int temp_min = 9999, temp_max = 0, temp_count = 0, temp_tot = 0;

typedef struct {
    int temp;
    int temp_max;
    int temp_min;
    int temp_count;
    int temp_tot;
} message_t;

// 16 objects can be buffered, due to memory constraints
MemoryPool<message_t, 16> mpool;
Queue<message_t, 64> queue;

// function arguments for coap handler
void* coap_malloc(uint16_t size) {
    return malloc(size);
}

void coap_free(void* addr) {
    free(addr);
}

uint8_t coap_tx_cb(uint8_t *a, uint16_t b, sn_nsdl_addr_s *c, void *d) {
    printf("coap tx cb\n");
    return 0;
}

int8_t coap_rx_cb(sn_coap_hdr_s *a, sn_nsdl_addr_s *b, void *c) {
    printf("coap rx cb\n");
    return 0;
}

// this function is run on recv_frm_thread
void recv_frm() {
    SocketAddress addr;
    DigitalOut led(LED3);
    uint8_t* recv_bfr = (uint8_t*)malloc(MAX_PKT_SIZE);

    printf("Coap server listening on: %d\n", SERVER_PORT);
    while (1) {
        printf("Receving message...\n");
        nsapi_size_or_error_t res = rcvSocket.recvfrom(&addr, recv_bfr, MAX_PKT_SIZE);
        led = !led;
        printf("Received a message of length: '%d'\n", res);
        sn_coap_hdr_s* parsed = sn_coap_parser(coap_handler, res, recv_bfr, &coap_version);

        std::string payload((const char*)parsed->payload_ptr, parsed->payload_len);

        printf("\tmsg_id:           %d\n", parsed->msg_id);
        printf("\tmsg_code:         %d\n", parsed->msg_code);
        printf("\tcontent_format:   %d\n", parsed->content_format);
        printf("\tpayload_len:      %d\n", parsed->payload_len);
        printf("\tpayload:          %s\n", payload.c_str());
        printf("\toptions_list_ptr: %p\n", parsed->options_list_ptr);

        // logic to store the temperature data here
        // check payload : think of some predefined format
        // if payload matches temperature data
        // filter it using some golbal max and min?
        // classify the data ? max and min to map to the 

        int temp = atoi(payload.c_str());
        temp_tot += temp;
        temp_count++;

        if (temp < temp_min) {
            temp_min = temp;
        } else if (temp > temp_max) {
            temp_max = temp;
        }

        // main thread deques and sends the anamolies
        // to the server!
        if (temp < MIN_TEMP || temp > MAX_TEMP) {
            message_t *message = mpool.try_alloc();
            message->temp = temp;
            message->temp_max = temp_max;
            message->temp_min = temp_min;
            message->temp_tot = temp_tot;
            message->temp_count = temp_count;

            queue.put(message);
        }

        memset(recv_bfr, 0, sizeof(*recv_bfr));
    }

    free(recv_bfr);
    // comes out of loop implies message receiving failed
    printf("Coap message receiving failed!");
}

// main() runs in its own thread in the OS
int main() {
    printf("CoAp Server\n");

    #ifdef MBED_MAJOR_VERSION
        printf("Mbed OS version: %d.%d.%d\n\n", MBED_MAJOR_VERSION, MBED_MINOR_VERSION, MBED_PATCH_VERSION);
    #endif

    // run the top function in a thread
    // get the network interface
    NetworkInterface *network = NetworkInterface::get_default_instance();

    if (!network || network->connect() != NSAPI_ERROR_OK) {
        printf("Cannot connect to the network! See the serial output.");
        // wait forever!
        DigitalOut led(LED1);

        while (1) {
            led = !led;
            ThisThread::sleep_for(BLINKING_RATE);
        }
    }

    // print ip address of the network interface
    // Show the network address
    SocketAddress a;
    network->get_ip_address(&a);
    printf("IP address: %s\n", a.get_ip_address() ? a.get_ip_address() : "None");
    network->get_netmask(&a);
    printf("Netmask: %s\n", a.get_ip_address() ? a.get_ip_address() : "None");
    network->get_gateway(&a);
    printf("Gateway: %s\n\n", a.get_ip_address() ? a.get_ip_address() : "None");

    printf("Network connection successful! Opening a socket...\n");
    sndSocket.open(network);

    // initialize receive socket
    rcvSocket.open(network);
    rcvSocket.bind(SERVER_PORT);

    // initialize coap protocol handler
    coap_handler = sn_coap_protocol_init(&coap_malloc, &coap_free, &coap_tx_cb, &coap_rx_cb);
    recv_frm_thread.start(&recv_frm);

    message_t *message;
    DigitalOut led(LED2);
    // path to the resource
    const char* coap_uri_path = "/data";
    const char* json = "{\"id\":\"123\",\"type\":\"warning\",\"temp\":\"%d\",\"temp_min\":\"%d\",\"temp_max\":\"%d\",\"temp_avg\":\"%d\"}";

    while(1) {
        osEvent event = queue.get();
        led = !led;
        // check for event type
        if (event.status != osEventMessage) { continue; }

        printf("Processing message from the queue..\n");
        // send this message to proxy
        message = (message_t *)event.value.p;
        // construct payload based on the message
        char payload[128];
        sprintf(payload, json, message->temp, message->temp_min, message->temp_max, (message->temp_tot)/message->temp_count);
        sn_coap_hdr_s *coap_res_ptr = (sn_coap_hdr_s*)calloc(sizeof(sn_coap_hdr_s), 1);

        coap_res_ptr->uri_path_ptr      = (uint8_t*)coap_uri_path;
        coap_res_ptr->uri_path_len      = strlen(coap_uri_path);
        coap_res_ptr->msg_code          = COAP_MSG_CODE_REQUEST_POST;
        coap_res_ptr->content_format    = COAP_CT_JSON;
        coap_res_ptr->payload_len       = strlen(payload);
        coap_res_ptr->payload_ptr       = (uint8_t*)payload;
        coap_res_ptr->options_list_ptr  = 0;
        coap_res_ptr->msg_id            = 7;

        // Calculate the CoAP message size, allocate the memory and build the message
        uint16_t message_len = sn_coap_builder_calc_needed_packet_data_size(coap_res_ptr);
        printf("Calculated message length: %d bytes\n", message_len);

        uint8_t* message_ptr = (uint8_t*)malloc(message_len);
        sn_coap_builder(message_ptr, coap_res_ptr);

        SocketAddress server_addr(PROXY_SERVER_ADDR, PROXY_SERVER_PORT); 
        printf("Sending message to proxy...\n");
        int sent_bytes = sndSocket.sendto(server_addr, message_ptr, message_len);
        if (sent_bytes < 0) {
            printf("Error: socket sendto failed.\n");
            return 1;
        }

        printf("Sent %d bytes to coap://10.0.0.139:5688/data\n", sent_bytes);

        free(coap_res_ptr);
        free(message_ptr);
        mpool.free(message);
    }
}