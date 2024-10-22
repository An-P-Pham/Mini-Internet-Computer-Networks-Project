#include "ctcp_bbr.h"

#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
#define TOTAL_PACING_GAINS 8

/* Our Array of BBR ModeHandlers: Contains function pointer to correct function */
static const FuncPtr ModeHandlerArr[TOTAL_NUM_BBR_MODES] = {&bbr_startup_state, &bbr_drain_state, &bbr_probe_bw_state, &bbr_probe_rtt_state}; 

ctcp_bbr_t * bbr_init()
{
    ctcp_bbr_t *bbr = malloc(sizeof(ctcp_bbr_t));
    bbr->next_packet_send_time = 0;
    bbr->min_rtt_filter_window = BBR_MIN_RTT_THRESHOLD;
    
    bbr->startup_bw_arr[0] = 0;
    bbr->startup_bw_arr[1] = 0;
    bbr->startup_bw_arr[2] = 0;

    bbr->min_rtt = -1;
    bbr->max_bw = -1;

    bbr->rtt_cnt = 0; 
    bbr->curr_pacing_gain = bbr_pacing_gain[BBR_STARTUP];
    bbr->btlbw = 11520;
    bbr->rtt_prop = 200;
    bbr->inflight_data = 0; 
    bbr->app_limited_until = 0; 

    
    return bbr;
}

/* Based on the following bbr mode, call the specific mode handler */
void bbr_update_model(ctcp_bbr_t *bbr)
{
    /* Forwards bbr input to correct function handler */
    (*ModeHandlerArr[bbr->curr_bbr_mode])(bbr);
}

void bbr_startup_state(ctcp_bbr_t *bbr)
{
    
    if((bbr->rtt_cnt % BBR_FULL_BW_COUNT == 0) && (bbr->startup_bw_arr[0] != 0))
    {
        /* Check if our growth is less than 25 percent */
        float changePercentage = (float)((bbr->startup_bw_arr[2] - bbr->startup_bw_arr[0])/bbr->startup_bw_arr[0]);
        changePercentage = changePercentage * 100;
        if(changePercentage < 25.0)
        {
            bbr->curr_bbr_mode = BBR_DRAIN;
            bbr->drain_round = bbr->rtt_cnt;
            return;
        }
    }

    if((bbr->rtt_cnt % BBR_FULL_BW_COUNT == 0) && (bbr->startup_bw_arr[0] == 0))
    {
        bbr->curr_bbr_mode = BBR_DRAIN;
        bbr->drain_round = bbr->rtt_cnt;
        return;
    }
    else
    {
        bbr->curr_pacing_gain = bbr_pacing_gain[BBR_STARTUP];
    }

    bbr->curr_cwnd = (float)(bbr->rtt_prop * bbr->btlbw * bbr_pacing_gain[BBR_STARTUP]);
    return;
}

void bbr_drain_state(ctcp_bbr_t *bbr)
{
    static const uint8_t MAX_DRAIN_ROUNDS = 4;
    uint8_t rtt_rounds_elapsed = bbr->rtt_cnt - bbr->probe_rtt_round;
    if(rtt_rounds_elapsed >= MAX_DRAIN_ROUNDS)
    {
        bbr->curr_bbr_mode = BBR_PROBE_BW;
        bbr->probe_bw_data = 0;
        bbr->probe_bw_pacing_idx = 2;
        return;
    }
    bbr->curr_pacing_gain = bbr_pacing_gain[BBR_DRAIN];
    bbr->curr_cwnd = (float)(bbr->rtt_prop * bbr->btlbw * bbr_pacing_gain[BBR_DRAIN]);
    return;
}

void bbr_probe_bw_state(ctcp_bbr_t *bbr)
{
    uint32_t bdp = (bbr->btlbw * bbr->rtt_prop/1000);
    if(bbr->probe_bw_data >= bdp)
    {
        bbr->probe_bw_data = 0;
        bbr->probe_bw_pacing_idx = (bbr->probe_bw_pacing_idx == TOTAL_PACING_GAINS) ? 1 : (bbr->probe_bw_pacing_idx + 1);
    }
    bbr->curr_pacing_gain = bbr_pacing_gain[bbr->probe_bw_pacing_idx];
    bbr->curr_cwnd = (float)(bbr->rtt_prop * bbr->btlbw * bbr_pacing_gain[7]);
    return;
}

void bbr_probe_rtt_state(ctcp_bbr_t *bbr)
{
    static const uint8_t MAX_PROBE_RTT_ROUNDS = 4;
    uint8_t rtt_rounds_elapsed = bbr->rtt_cnt - bbr->probe_rtt_round;
    if(rtt_rounds_elapsed >= MAX_PROBE_RTT_ROUNDS)
    {
        bbr->curr_bbr_mode = BBR_PROBE_BW;
        bbr->probe_bw_data = 0;
        bbr->probe_bw_pacing_idx = 2;
    }
    bbr->curr_pacing_gain = bbr_pacing_gain[7];
    bbr->curr_cwnd = (float)(bbr->rtt_prop * bbr->btlbw * bbr_pacing_gain[1]);
    return;
}

float bbr_update_bw(ctcp_bbr_t *bbr, long round_trip_time, uint32_t seg_len)
{
    bbr->rtt_cnt += 1;
    float bwCalc = (float)(seg_len/round_trip_time);
    float bandwidth = (round_trip_time == 0) ? 0.0 : bwCalc;
    return bandwidth;
}

void bbr_update_rtt(ctcp_bbr_t *bbr, long round_trip_time)
{
    //Should we change the rtt_prop time?
    bbr->rtt_prop = (bbr->curr_bbr_mode == BBR_PROBE_RTT) ? round_trip_time : bbr->rtt_prop;
    long time_since_rtt_update = current_time() - bbr->rtt_updated_stamp;

    if(time_since_rtt_update >= bbr->rtt_prop && bbr->curr_bbr_mode == BBR_PROBE_BW)
    {
        bbr->probe_rtt_round = bbr->rtt_cnt;
        bbr->curr_bbr_mode = BBR_PROBE_RTT;
    }

    if(round_trip_time < bbr->rtt_prop)
    {
        bbr->rtt_prop = round_trip_time;
        switch(bbr->curr_bbr_mode)
        {
            case BBR_PROBE_BW:
                bbr->probe_rtt_round = bbr->rtt_cnt;
                bbr->curr_bbr_mode = BBR_PROBE_RTT;
                break;
            case BBR_PROBE_RTT:
                bbr->curr_bbr_mode = BBR_STARTUP;
                break;
            default:
                break;
        }
    }
}