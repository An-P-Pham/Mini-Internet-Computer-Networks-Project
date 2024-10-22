/******************************************************************************
 * ctcp.c
 * ------
 * Implementation of cTCP done here. This is the only file you need to change.
 * Look at the following files for references and useful functions:
 *   - ctcp.h: Headers for this file.
 *   - ctcp_iinked_list.h: Linked list functions for managing a linked list.
 *   - ctcp_sys.h: Connection-related structs and functions, cTCP segment
 *                 definition.
 *   - ctcp_utils.h: Checksum computation, getting the current time.
 *
 *****************************************************************************/

#include "ctcp.h"
#include "ctcp_linked_list.h"
#include "ctcp_sys.h"
#include "ctcp_utils.h"
#include "ctcp_bbr.h"

/* MACROS & constants */

/* Initial SEQ & ACK should be 1 */
#define INIT_SEQ_NUM 1
#define INIT_ACK_NUM 1
#define BITS_PER_BYTE 8
#define MAX_NUM_RETRANSMIT 5

static const uint16_t ctcp_hdr_size = sizeof(ctcp_segment_t);

typedef enum
{
  ESTABLISHED = 0, /* Operating Normally */
  CLOSING /* Received a FIN  */
} ctcp_conn_state;

typedef struct {
  long prev_sent_time; /* Time stamp of last time this segment was sent */
  uint8_t curr_num_retransmit; /* Current number of re-transmit for current segment */
  bool is_in_flight; /* Indicates if this packet is currently in flight */
  bool is_app_limited; /* Indicates if packet is app-limited or not */
  ctcp_segment_t *curr_ctcp_segment; /* Pointer to a segment */
} ctcp_segment_with_info_t; /* Contains meta-data for each segment */

/**
 * Connection state.
 *
 * Stores per-connection information such as the current sequence number,
 * unacknowledged packets, etc.
 *
 * You should add to this to store other fields you might need.
 */
struct ctcp_state {
  struct ctcp_state *next;  /* Next in linked list */
  struct ctcp_state **prev; /* Prev in linked list */

  conn_t *conn;             /* Connection object -- needed in order to figure
                               out destination when sending */
  //linked_list_t *segments;  
                              /* Linked list of segments sent to this connection.
                               It may be useful to have multiple linked lists
                               for unacknowledged segments, segments that
                               haven't been sent, etc. Lab 1 uses the
                               stop-and-wait protocol and therefore does not
                               necessarily need a linked list. You may remove
                               this if this is the case for you */

  /* FIXME: Add other needed fields. */
  linked_list_t *unacked_segs_with_info; /* Linked list of segments that haven't recieved ack*/
  linked_list_t *segs_to_send; /* Linked list of segments that haven't been sent */
  ctcp_config_t *connect_config; /* Configuration for current connection */
  long final_packet_time; /* Time stamp of when the final packet was sent */
  uint32_t prev_ackno;        /* Prevous acknowledgement number (in bytes) */
  uint32_t curr_seqno;        /* Current Sequence number (in bytes) */
  uint32_t curr_ackno;        /* Current Acknowledgment number (in bytes) */
  uint16_t curr_window_size; /* Current window size */
  bool EOF_FLAG; /* If this connection received an EOF */
  bool FIN_FLAG; /* If we have received a FIN segment */
  ctcp_conn_state curr_conn_state; /* Keeps track of current state of ctcp connection */
  char *output_buffer;
  int output_len;

  /* Part 2 Variables */
  ctcp_bbr_t *bbr; /* Our BBR related structure */
  FILE *bdp_output_file; /* BDP measurement file */
  long prev_packet_sent_time; /* Time stamp of when the previous packet was sent */
  long timer_time_stamp; /* Tracks whenever ctcp_timer goes off */
  
};

/**
 * Linked list of connection states. Go through this in ctcp_timer() to
 * resubmit segments and tear down connections.
 */
static ctcp_state_t *state_list;

/* FIXME: Feel free to add as many helper functions as needed. Don't repeat
          code! Helper functions make the code clearer and cleaner. */

/**
 * Prepares segments that will be transferring data. Adds it to the unack seg linked list before sending
 */
void ctcp_prep_data_segment(ctcp_state_t *state, uint8_t* sending_data, uint16_t data_len);

/**
 * Checks segments that need to be sent & slides the window if there is enough room. 
 * After increasing the sliding window, the function sends the segmends that are part 
 * of the window.
 */
void ctcp_send_sliding_window(ctcp_state_t *state);

void ctcp_send_non_data_segment(ctcp_state_t *state, int flags);

void print_bdp_results(ctcp_state_t *state, long round_trip_time);

ctcp_state_t *ctcp_init(conn_t *conn, ctcp_config_t *cfg) {
  /* Connection could not be established. */
  if (conn == NULL) {
    return NULL;
  }

  /* Established a connection. Create a new state and update the linked list
     of connection states. */
  ctcp_state_t *state = calloc(sizeof(ctcp_state_t), 1);
  state->next = state_list;
  state->prev = &state_list;
  if (state_list)
    state_list->prev = &state->next;
  state_list = state;

  /* Set fields. */
  state->conn = conn;
  /* FIXME: Do any other initialization here. */
  state->unacked_segs_with_info = ll_create();
  state->segs_to_send = ll_create();
  state->connect_config = cfg;
  state->curr_seqno = state->curr_ackno = state->prev_ackno = INIT_ACK_NUM;
  state->curr_window_size = 0;
  state->final_packet_time = 0;
  state->curr_conn_state = ESTABLISHED;
  state->EOF_FLAG = state->FIN_FLAG = false;
  state->output_buffer = NULL;
  state->output_len = 0;

  /* Part 2 initialization */
  state->prev_packet_sent_time = 0;
  state->timer_time_stamp = 0;
  if((state->bbr = bbr_init()) == NULL) //Error in initializing bbr
  {
    return NULL;
  }
  state->bbr->curr_cwnd = cfg->send_window;
  state->bdp_output_file = fopen("bdp.txt", "w");
  
  return state;
}

void ctcp_destroy(ctcp_state_t *state) {
  /* Update linked list. */
  if (state->next)
    state->next->prev = state->prev;

  *state->prev = state->next;
  conn_remove(state->conn);

  /* FIXME: Do any other cleanup here. */
  ll_node_t * curr_unacked_seg_node; 
  ll_node_t * curr_seg_to_send_node; 

  /* Close BDP file */
  //fclose(state->bdp_output_file);

  /* Free all nodes from unacked segments linked list */
  while((curr_unacked_seg_node = ll_front(state->unacked_segs_with_info) ) != NULL)
  {
    free(curr_unacked_seg_node->object);
    ll_remove(state->unacked_segs_with_info, curr_unacked_seg_node);
  }

  /* Free all nodes from segments that still needs to be sent */
  while((curr_seg_to_send_node = ll_front(state->segs_to_send)) != NULL)
  {
    free(curr_seg_to_send_node->object);
    ll_remove(state->segs_to_send, curr_seg_to_send_node);
  }

  ll_destroy(state->unacked_segs_with_info);
  ll_destroy(state->segs_to_send);
  free(state->connect_config); //ctcp_init() said to free it when done using it
  free(state->bbr); //free the bbr struct 
  free(state);
  end_client();
}

/* Called automatically by library when there is more input to read */
void ctcp_read(ctcp_state_t *state) {
  
  uint8_t buf[MAX_SEG_DATA_SIZE];
  int read_results;
  //ctcp_segment_with_info_t *sending_seg;
  /* We have nothing else to read */
  if(state->EOF_FLAG == true)
  {
    return;
  }
  
  /* Reads buffer up till MAX_SEG_DATA_SIZE & repeats */
  while((read_results = conn_input(state->conn, buf, MAX_SEG_DATA_SIZE)) > 0)
  {
    ctcp_prep_data_segment(state, buf, read_results);
    ctcp_send_sliding_window(state);
  }
  
  if(read_results == 0) /* No data available */
  {
    //return; DO nothing
  }
  else if(read_results == -1) /* EOF reached --> send FIN */
  {

    if(state->EOF_FLAG == false && ll_length(state->segs_to_send) == 0)
    {
      state->EOF_FLAG = true;
      ctcp_send_non_data_segment(state, TH_FIN); //--> this method doesn't require ACK
      return;
    }
    
  }
  //ctcp_send_sliding_window(state); //Necessary?

}

void ctcp_receive(ctcp_state_t *state, ctcp_segment_t *segment, size_t len) {

  /* Check for segment being truncated, we ignore it & wait for retransmit */
  uint16_t segment_len = ntohs(segment->len);
  //uint16_t len_of_data = segment_len - ctcp_hdr_size;
  if(len < segment_len)
  {
    free(segment);
    return;
  }

  /* Check for corruption via checksum */
  uint16_t chk_sum_1, chk_sum_2;
  chk_sum_1 = segment->cksum;
  segment->cksum =0;
  chk_sum_2 = cksum(segment,sizeof(ctcp_segment_t)+strlen(segment->data));
  segment->cksum = chk_sum_1;
  if(chk_sum_1 != chk_sum_2)
  {
    free(segment);
    return;
  }

  /* Reject any segments that are not within window boundries */

  /* Handle FIN type of segment */
  if ((segment->flags & TH_FIN) && (state->FIN_FLAG == false))
  {
    state->FIN_FLAG = true;
    //send ACK for FIN
    ctcp_send_non_data_segment(state, TH_ACK);
    //output an EOF by calling conn_output() with legth of 0
    conn_output(state->conn, segment->data, 0);
    return;
  }

  /* Handle ACK type of segment */
  if (segment->flags & TH_ACK) 
  {
    if((state->FIN_FLAG == true) && state->EOF_FLAG)
    {
      ctcp_destroy(state);
    }
    
    //Inspect segments & retransmit ones that have not been acknowledged. (Only rt_timeout milliseconds after last sent)
    ll_node_t *curr_unacked_seg_node = ll_front(state->unacked_segs_with_info);
    ctcp_segment_with_info_t *unacked_seg_with_info;
    for( ; curr_unacked_seg_node != NULL; curr_unacked_seg_node = curr_unacked_seg_node->next)
    {
      unacked_seg_with_info = (ctcp_segment_with_info_t *)(curr_unacked_seg_node->object);
      ctcp_segment_t *curr_unacked_seg = unacked_seg_with_info->curr_ctcp_segment;

      if(ntohl(curr_unacked_seg->seqno) < ntohl(segment->ackno))
      {

        long round_trip_time = current_time() - (long)unacked_seg_with_info->prev_sent_time;
        if(state->bbr->min_rtt == -1)
        {
          state->bbr->min_rtt = state->bbr->rtt_prop = round_trip_time;
        }
        else
        {
          if(round_trip_time <= state->bbr->min_rtt)
          {
            state->bbr->min_rtt = round_trip_time;
            state->bbr->curr_bbr_mode = (state->bbr->curr_bbr_mode == BBR_PROBE_BW) ? BBR_STARTUP : state->bbr->curr_bbr_mode;
          }
        }

        if(state->bbr->rtt_cnt == 0)
        {
          state->bbr->max_bw = state->bbr->btlbw = bbr_update_bw(state->bbr, round_trip_time, ntohs(curr_unacked_seg->len));
        }
        else
        {

          if(unacked_seg_with_info->is_app_limited == false)
          {

            float bandwidth = bbr_update_bw(state->bbr, round_trip_time, ntohs(curr_unacked_seg->len));
            print_bdp_results(state, round_trip_time);

            if(state->bbr->max_bw < bandwidth)
            {
              state->bbr->max_bw = bandwidth;
              state->bbr->startup_bw_arr[2] = state->bbr->startup_bw_arr[1];
              state->bbr->startup_bw_arr[1] = state->bbr->startup_bw_arr[0];
              state->bbr->startup_bw_arr[0] = bandwidth;
            }
          
            if(state->bbr->rtt_cnt%10 == 0)
            {
              state->bbr->btlbw = (state->bbr->btlbw < state->bbr->max_bw) ? state->bbr->max_bw : state->bbr->btlbw;
            }

          }

        }

        /* Update state machine each ack */
        bbr_update_model(state->bbr);
        state->connect_config->send_window = state->bbr->curr_cwnd;
        if(state->bbr->app_limited_until > 0)
        {
          state->bbr->app_limited_until -= ntohs(curr_unacked_seg->len);
        }
        state->bbr->inflight_data -= ntohs(curr_unacked_seg->len);
        state->curr_window_size -= ntohs(curr_unacked_seg->len);
        free(ll_remove(state->unacked_segs_with_info, curr_unacked_seg_node));
        if(ll_length(state->segs_to_send) > 0)
        {
          ctcp_send_sliding_window(state);
        }
      }
      else
      {
        break;
      } 
    } 
  }

  int data_len = ntohs(segment->len) - ctcp_hdr_size;
  state->curr_ackno = ntohl(segment->seqno) + data_len;
  bool duplicate = false;
  if(ntohl(segment->seqno) < state->prev_ackno)
  {
    duplicate = true;
  }
  else
  {
    state->prev_ackno = state->curr_ackno;
  }

  /* Send acknowledgement */
  if(ntohs(segment->len) > ctcp_hdr_size)
  {
    if(!duplicate)
    {
      //ACK the outputted segments
      state->output_buffer = segment->data;
      state->output_len = data_len;
      ctcp_output(state);
    }
  }
  //The received segment MUST BE FREED after you are done with it.
  free(segment);
}

void ctcp_output(ctcp_state_t *state) {
  if(state == NULL)
  {
    return;
  }
  
  size_t bufspace_available;

  if((bufspace_available = conn_bufspace(state->conn)) >= state->output_len)
  {
    ctcp_send_non_data_segment(state, TH_ACK);
    conn_output(state->conn, state->output_buffer, state->output_len);
    state->output_buffer = NULL;
    state->output_len = 0;
  }
  
}

/**
 * Called periodically at specified rate (see the timer field in the
 * ctcp_config_t struct).
 */
void ctcp_timer() {

  if(state_list == NULL)
  {
    return;
  }

  ctcp_state_t *curr_state = state_list;
  
  /* Loop through all the connection states */
  for( ; curr_state != NULL; curr_state = curr_state->next)
  {

    if(curr_state->bbr->min_rtt_filter_window > 0)
    {
      curr_state->timer_time_stamp = (curr_state->timer_time_stamp == 0) ? current_time() : curr_state->timer_time_stamp; 
      curr_state->bbr->min_rtt_filter_window -= (current_time() - curr_state->timer_time_stamp);
      curr_state->timer_time_stamp = current_time();
    }

    if(curr_state->bbr->min_rtt_filter_window <= 0)
    {
      bbr_update_rtt(curr_state->bbr, curr_state->bbr->min_rtt);
      if(curr_state->bbr->min_rtt < curr_state->bbr->rtt_prop)
      {
        curr_state->bbr->rtt_prop = curr_state->bbr->min_rtt;
        curr_state->bbr->rtt_updated_stamp = current_time();
      }

      curr_state->bbr->min_rtt_filter_window = BBR_MIN_RTT_THRESHOLD;
    }

    //Inspect segments & retransmit ones that have not been acknowledged. (Only rt_timeout milliseconds after last sent)
    ll_node_t *curr_unacked_seg_node = ll_front(curr_state->unacked_segs_with_info);
    ctcp_segment_with_info_t *curr_unacked_segment;
    for( ; curr_unacked_seg_node != NULL; curr_unacked_seg_node = curr_unacked_seg_node->next)
    {

      curr_unacked_segment = (ctcp_segment_with_info_t *)(curr_unacked_seg_node->object);

      /* Is this current segment under 5 retransmits & under rt_timeout*/
      long curr_time = current_time();
      if((curr_time - curr_unacked_segment->prev_sent_time > curr_state->connect_config->rt_timeout) &&
        curr_unacked_segment->curr_num_retransmit < MAX_NUM_RETRANSMIT)
      {
        uint16_t segment_len = ntohs(curr_unacked_segment->curr_ctcp_segment->len);
        curr_unacked_segment->prev_sent_time = current_time();
        curr_state->curr_window_size -= segment_len;
        curr_unacked_segment->curr_num_retransmit++;
        conn_send(curr_state->conn, curr_unacked_segment->curr_ctcp_segment, segment_len);
      }

      //After 5 retransmission attempts (so a total of 6 times) for a segment, tear down connection
      if(curr_unacked_segment->curr_num_retransmit >= MAX_NUM_RETRANSMIT)
      {
        ctcp_destroy(curr_state);
        break;
      }

    }

    /* If all conditions are met, destroy the connections */
    if(curr_state->FIN_FLAG && curr_state->EOF_FLAG && (ll_length(curr_state->unacked_segs_with_info) == 0) && (ll_length(curr_state->segs_to_send) == 0))
    {
      if(curr_state->final_packet_time == 0)
      {
        curr_state->final_packet_time = current_time();
      }else if((current_time() - curr_state->final_packet_time) > (2 * curr_state->connect_config->rt_timeout))
      {
        ctcp_destroy(curr_state);
      }
    }
    
  }
}

/*
* Needs to slide window, in-order, make sure that window size is not overwhelmed.
* After buffering up the window, we send it.
*/
void ctcp_send_sliding_window(ctcp_state_t *state)
{
  /* Increasing the sliding window part */
  ll_node_t *curr_seg_to_send_node;
  ctcp_segment_with_info_t *curr_send_segment_wrapper;
  while((state->curr_window_size < state->connect_config->send_window) && (curr_seg_to_send_node = ll_front(state->segs_to_send)) != NULL)
  {
    curr_send_segment_wrapper = (ctcp_segment_with_info_t *)(curr_seg_to_send_node->object);
    ctcp_segment_t * curr_send_segment = (ctcp_segment_t *)(curr_send_segment_wrapper->curr_ctcp_segment);
    ll_add(state->unacked_segs_with_info, curr_send_segment_wrapper);
    state->curr_window_size += ntohs(curr_send_segment->len);
    ll_remove(state->segs_to_send, curr_seg_to_send_node);
  }

  /* If inflight_data is greater than bdp don't send */
  int32_t bytes_to_send = state->curr_window_size - state->bbr->inflight_data;
  int bdp = state->bbr->rtt_prop * state->bbr->btlbw * 2.885;
  bool unable_to_send = (state->bbr->inflight_data >= bdp) ? true : false;

  if(bdp != 0 && unable_to_send)
  {
    return;
  }

  if((bytes_to_send == 0) && ll_length(state->segs_to_send))
  {
    state->bbr->app_limited_until = state->bbr->inflight_data;
    return;
  }

  /* Now we send the window */
  ll_node_t *curr_unacked_seg_node = ll_front(state->unacked_segs_with_info);
  ctcp_segment_with_info_t *curr_unack_segment; 
  for( ; curr_unacked_seg_node != NULL; curr_unacked_seg_node = curr_unacked_seg_node->next)
  {
    curr_unack_segment = (ctcp_segment_with_info_t *)(curr_unacked_seg_node->object);
    ctcp_segment_t *seg = curr_unack_segment->curr_ctcp_segment;
    if(curr_unack_segment->is_in_flight == false)
    {
      state->prev_packet_sent_time = curr_unack_segment->prev_sent_time = current_time(); 
      if(state->bbr->app_limited_until > 0)
      {
        curr_unack_segment->is_app_limited = true;
      }

      /* Wait until it's the right time to send the packet before sending */
      do{
        if(current_time() >= state->bbr->next_packet_send_time)
        {
          state->bbr->probe_bw_data += ntohs(seg->len);
          curr_unack_segment->is_in_flight = true;
          state->bbr->inflight_data += ntohs(seg->len);  
          conn_send(state->conn, seg, ntohs(seg->len));
          break;
        }
      }while(true);

      if(state->bbr->max_bw != 0)
      {
        long segment_len = ntohs(seg->len);
        long curr_btlbw = state->bbr->btlbw;
        double curr_pacing = state->bbr->curr_pacing_gain;
        state->bbr->next_packet_send_time = current_time() + segment_len/(curr_pacing * curr_btlbw);
      }
      else
      {
        state->bbr->next_packet_send_time = 0;
      }

    }
  }
}

void ctcp_prep_data_segment(ctcp_state_t *state, uint8_t* sending_data, uint16_t data_len)
{
  /* The inner segment that holds the data: copy information from sending_data (buffer) */
  ctcp_segment_t *send_segment = (ctcp_segment_t*)calloc(sizeof(ctcp_segment_t) + data_len,1);
  int send_segment_size = sizeof(ctcp_segment_t) + data_len;
  memcpy(send_segment->data,sending_data,data_len);

  /* setting up the necessary field for sending the segment */
  send_segment->seqno = htonl(state->curr_seqno);
  send_segment->ackno = htonl(state->curr_ackno);
  send_segment->len = htons(send_segment_size);
  send_segment->window= htons(state->connect_config->recv_window); //TODO: this probably should be current window size
  send_segment->flags |= TH_ACK;
  send_segment->cksum = 0;
  send_segment->cksum = cksum(send_segment,send_segment_size);

  /* Allocate the wrapper & add in the segment */
  ctcp_segment_with_info_t *send_segment_wrapper = malloc(sizeof(ctcp_segment_with_info_t));
  send_segment_wrapper->prev_sent_time = 0;
  send_segment_wrapper->curr_num_retransmit = 0;
  send_segment_wrapper->is_in_flight = 0;
  send_segment_wrapper->is_app_limited = 0;
  send_segment_wrapper->curr_ctcp_segment = send_segment;

  /* add packet to send queue */
  ll_add(state->segs_to_send, send_segment_wrapper); 
  state->curr_seqno += data_len;

  /* check and clear the segment in the sliding window */
  //ctcp_send_sliding_window(state);
  
}

/*
* Sends an acknowledgement packet without any data within the segment
*/
void ctcp_send_non_data_segment(ctcp_state_t *state, int flags)
{
  ctcp_segment_t send_segment;
  send_segment.seqno = htonl(state->curr_seqno); 
  send_segment.ackno = htonl(state->curr_ackno);
  send_segment.len = htons(ctcp_hdr_size);
  send_segment.flags |= flags;
  //send_segment.flags |= TH_ACK;
  send_segment.window = htons(state->connect_config->recv_window); //TODO: this probably should be current window size
  send_segment.cksum = 0;
  send_segment.cksum = cksum(&send_segment, ctcp_hdr_size);
  conn_send(state->conn, &send_segment, ctcp_hdr_size);
}

void print_bdp_results(ctcp_state_t *state, long round_trip_time)
{
  long bdp = state->bbr->btlbw*round_trip_time*BITS_PER_BYTE;
  fprintf(state->bdp_output_file,"%ld,%ld\n",current_time(),bdp);
  //fflush(state->bdp_output_file);
}