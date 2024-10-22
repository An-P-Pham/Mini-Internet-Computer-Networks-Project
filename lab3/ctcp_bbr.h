#include <math.h>
#include "ctcp_utils.h"

#define TOTAL_NUM_BBR_MODES 4
#define MAX_UINT_VAL 0xFFFFFFFF /* Used to initialize min_rtt */
#define MAX_ULONG_VAL 0xFFFFFFFFFFFFFFFF

/* BBR constants */
#define BBR_MIN_RTT_THRESHOLD 10000 /* Time in seconds when MIN_RTT not touched */
#define BBR_FULL_BW_COUNT 3 /* N rounds without bw growth -> pipe full */

typedef enum {
    BBR_STARTUP = 0, /* ramp up sending rate rapidly to fill pipe */
    BBR_DRAIN = 1, /* drain any queue created during startup */
    BBR_PROBE_BW = 2, /* discover, share bw: pace around estimated bw */
    BBR_PROBE_RTT = 3 /* cut cwnd to min to probe min_rtt */
} bbr_mode;

typedef struct {
    long next_packet_send_time; /* Next packet send time */
    long rtt_updated_stamp; /* Indicates when rrt is updated */
    uint32_t rtt_cnt; /* count of packet-timed rounds elapsed */
    int min_rtt_filter_window; /* Indicates time threshold for when rtt must be updated */
    
    float curr_pacing_gain; /* Which BBR pacing gain are we currently using */

    uint32_t probe_bw_data; /* Total amount of data sent during probe bw phase */
    uint8_t probe_bw_pacing_idx; /* current pacing gain index for probe bw phase */

    int32_t rtt_prop; //TODO: should we change to long?
    int32_t btlbw;
    long min_rtt; /* Minimum round-trip time of the connection */
    int32_t max_bw; /* Maximum bandwidth of the connection */

    uint32_t startup_bw_arr[BBR_FULL_BW_COUNT]; /* tracks 3 non limited bw after startup */
    
    uint32_t drain_round; /* Which round draining takes place */
    uint32_t probe_rtt_round; /* Which round probe rtt takes place */

    uint16_t curr_cwnd; /* Current congestion window of bbr */

    uint32_t inflight_data; /* Total amount data (in bytes) in flight */
    uint32_t app_limited_until; /* If application is rate limited */
    bbr_mode curr_bbr_mode;
} ctcp_bbr_t;

/* The pacing_gain values for the PROBE_BW gain cycle */
//static const float bbr_high_gain = 2/log(2);
//static const float bbr_drain_gain = 1/bbr_high_gain;
static const float bbr_pacing_gain[] = {2.885, 1/2.885, 1.25, 0.75, 1.0, 1.0, 1.0, 1.0, 1.0};
//static uint32_t bbr_cwnd_min_target = 4; //Try to keep at least this many packets in flight

/* Function pointer to function that takes ctcp_bbr_t* input */
typedef void (*FuncPtr)(ctcp_bbr_t *);

ctcp_bbr_t * bbr_init();

void bbr_update_model(ctcp_bbr_t *bbr);

void bbr_startup_state(ctcp_bbr_t *bbr);

void bbr_drain_state(ctcp_bbr_t *bbr);

void bbr_probe_bw_state(ctcp_bbr_t *bbr);

void bbr_probe_rtt_state(ctcp_bbr_t *bbr);

float bbr_update_bw(ctcp_bbr_t *bbr, long round_trip_time, uint32_t seg_len);

void bbr_update_rtt(ctcp_bbr_t *bbr, long round_trip_time);