#ifndef MY_LIB_WIFI
#define MY_LIB_WIFI

typedef struct NTP_T_
{
    ip_addr_t ntp_server_address;
    struct udp_pcb *ntp_pcb;
    async_at_time_worker_t request_worker;
    async_at_time_worker_t resend_worker;
    bool ntp_synchronized;
    char timestr[64];
} NTP_T;

extern NTP_T* ntp_state;

int start_query_ntp();

#endif