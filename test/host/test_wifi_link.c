#include <assert.h>
#include "../../main/link/wifi_link.c"
int64_t host_now_us;
static unsigned failures,acks;
static void failed(wearable_action_t a,uint32_t seq,void *ctx) {(void)a;(void)seq;(void)ctx;failures++;}
static void confirmed(wearable_action_t a,const wearable_ack_packet_t *ack,void *ctx) {(void)a;(void)ack;(void)ctx;acks++;}
static const char *id="0123456789abcdef0123456789abcdef";
static httpd_req_t poll_at(int64_t time)
{
    host_now_us=time;expire_head();
    httpd_req_t req={.id=id};action_get(&req);return req;
}
static httpd_req_t ack(uint32_t sequence)
{
    uint8_t bytes[20]={6,2,0,0};
    bytes[4]=123;bytes[8]=(uint8_t)sequence;
    httpd_req_t req={.id=id,.body=bytes,.content_len=20};
    ack_post(&req);return req;
}
int main(void)
{
    assert(wifi_link_start(confirmed,failed,NULL)==ESP_OK);
    httpd_req_t req={.id=id};session_post(&req);
    assert(wifi_link_send_action(WEARABLE_ACTION_POINT_A));
    for(unsigned attempt=0;attempt<3;attempt++){
        req=poll_at(attempt*ACTION_ACK_WAIT_US);
        assert(req.response_len==12 && req.response[6]==1);
        req=poll_at(attempt*ACTION_ACK_WAIT_US+150000);assert(req.response_len==0);
    }
    req=poll_at(3000000);assert(req.response_len==0 && failures==1 && s_count==0);
    req=ack(1);assert(!strncmp(req.status,"410",3));
    assert(wifi_link_send_action(WEARABLE_ACTION_POINT_A));
    req=poll_at(3100000);assert(req.response_len==12 && req.response[6]==2);
    req=ack(2);assert(!strncmp(req.status,"204",3) && acks==1 && s_count==0);
    req=ack(2);assert(!strncmp(req.status,"204",3) && acks==1);
    assert(wifi_link_send_action(WEARABLE_ACTION_POINT_A));
    assert(wifi_link_send_action(WEARABLE_ACTION_POINT_B));
    req=poll_at(6200000);assert(failures==2 && req.response_len==12 && req.response[6]==4);
    req=ack(4);assert(acks==2 && s_count==0);
    puts("Wi-Fi HTTP queue: exactly 3 deliveries, expiry, late/duplicate ACK and next command OK");

    /* Push transport: heartbeat finds the controller, commands repeat until their ACK. */
    uint8_t out[WIFI_LINK_PUSH_MAX];
    const uint8_t device[6]={1,2,3,4,5,6},beat_ack[4]={'P','M',1,'h'};
    bool everyone=false;
    host_now_us=7000000;
    assert(wifi_link_push_output(out,sizeof(out),device,3900,&everyone)==20 && out[3]=='H' && everyone);
    assert(out[4]==1 && out[9]==6 && out[10]==123 && out[14]==4 && out[18]==(3900&0xff) && out[19]==(3900>>8));
    assert(!wifi_link_push_input(beat_ack,4,false));              /* the HTTP controller still holds the lease */
    host_now_us=17000000;
    assert(wifi_link_push_input(beat_ack,4,false) && wifi_link_is_connected());
    req=(httpd_req_t){.id=id};session_post(&req);assert(!strncmp(req.status,"409",3));
    assert(wifi_link_send_action(WEARABLE_ACTION_POINT_B));
    assert(wifi_link_push_output(out,sizeof(out),device,3900,&everyone)==32 && out[3]=='A' && !everyone && out[26]==5);
    assert(wifi_link_push_output(out,sizeof(out),device,3900,&everyone)==0);   /* not before 100 ms */
    host_now_us+=100000;
    assert(wifi_link_push_output(out,sizeof(out),device,3900,&everyone)==32 && out[26]==5);
    uint8_t push_ack[24]={'P','M',1,'a',6,2,0,0};
    push_ack[8]=123;push_ack[12]=5;
    assert(wifi_link_push_input(push_ack,24,true) && acks==3 && s_count==0);
    assert(wifi_link_push_input(push_ack,24,true) && acks==3);    /* a repeated ACK is harmless */
    host_now_us+=1000000;
    assert(wifi_link_push_output(out,sizeof(out),device,3900,&everyone)==20 && out[3]=='H' && !everyone && out[14]==5);
    assert(!wifi_link_push_input(beat_ack,4,false));              /* a second controller waits its turn */
    push_ack[2]=9;assert(!wifi_link_push_input(push_ack,24,true));/* unknown version */
    host_now_us+=3000001;
    assert(!wifi_link_is_connected() && !wifi_link_send_action(WEARABLE_ACTION_POINT_A));
    assert(wifi_link_push_output(out,sizeof(out),device,3900,&everyone)==20 && everyone);
    puts("Wi-Fi push link: discovery heartbeat, 100 ms repeats until ACK, one controller at a time, 3 s lease OK");
}
