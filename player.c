/*
 * Slave-clocked ALAC stream player. This file is part of Shairport.
 * Copyright (c) James Laird 2011, 2013
 * All rights reserved.
 *
 * Modifications for audio synchronisation
 * and related work, copyright (c) Mike Brady 2014
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <pthread.h>
#include <math.h>
#include <sys/stat.h>
#include <signal.h>
#include <sys/syslog.h>
#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <inttypes.h>

#include "config.h"

#ifdef HAVE_LIBPOLARSSL
#include <polarssl/aes.h>
#include <polarssl/havege.h>
#endif

#ifdef HAVE_LIBSSL
#include <openssl/aes.h>
#endif

#ifdef HAVE_LIBSOXR
#include <soxr.h>
#endif

#include "common.h"
#include "player.h"
#include "rtp.h"
#include "rtsp.h"

#include "alac.h"

// parameters from the source
static unsigned char *aesiv;
#ifdef HAVE_LIBSSL
static AES_KEY aes;
#endif
static int sampling_rate, frame_size;

static int bytes_per_audio_frame = 4;

// The maximum frame size change there can be is +/- 1;
static int max_frame_size_change = 1;
// #define FRAME_BYTES(frame_size) (4 * frame_size)
// maximal resampling shift - conservative
//#define OUTFRAME_BYTES(frame_size) (4 * (frame_size + 3))

#ifdef HAVE_LIBPOLARSSL
static aes_context dctx;
#endif

//static pthread_t player_thread = NULL;
static int please_stop;
static int encrypted; // Normally the audio is encrypted, but it may not be

static int connection_state_to_output; // if true, then play incoming stuff; if false drop everything

static alac_file *decoder_info;

// debug variables
static int late_packet_message_sent;
static uint64_t packet_count = 0;
static int32_t last_seqno_read;

// interthread variables
static int fix_volume = 0x10000;
static pthread_mutex_t vol_mutex = PTHREAD_MUTEX_INITIALIZER;

// default buffer size
// needs to be a power of 2 because of the way BUFIDX(seqno) works
#define BUFFER_FRAMES 512
#define MAX_PACKET 2048

// DAC buffer occupancy stuff
#define DAC_BUFFER_QUEUE_MINIMUM_LENGTH 600

typedef struct audio_buffer_entry { // decoded audio packets
  int ready;
  uint32_t timestamp;
  seq_t sequence_number;
  signed short *data;
  int length; // the length of the decoded data
} abuf_t;
static abuf_t audio_buffer[BUFFER_FRAMES];
#define BUFIDX(seqno) ((seq_t)(seqno) % BUFFER_FRAMES)

// mutex-protected variables
static seq_t ab_read, ab_write;
static int ab_buffering = 1, ab_synced = 0;
static uint32_t first_packet_timestamp = 0;
static int flush_requested = 0;
static uint32_t flush_rtp_timestamp;
static uint64_t time_of_last_audio_packet;
static int shutdown_requested;

// mutexes and condition variables
static pthread_mutex_t ab_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t flush_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t flowcontrol;

static int64_t first_packet_time_to_play, time_since_play_started; // nanoseconds

static audio_parameters audio_information;

// stats
static uint64_t missing_packets, late_packets, too_late_packets, resend_requests;

static void ab_resync(void) {
  int i;
  for (i = 0; i < BUFFER_FRAMES; i++) {
    audio_buffer[i].ready = 0;
    audio_buffer[i].sequence_number = 0;
  }
  ab_synced = 0;
  last_seqno_read = -1;
  ab_buffering = 1;
}

// the sequence number is a 16-bit unsigned number which wraps pretty often
// to work out if one seqno is 'after' another therefore depends whether wrap has occurred
// this function works out the actual ordinate of the seqno, i.e. the distance up from
// the zeroth element, at ab_read, taking due account of wrap.

static inline seq_t SUCCESSOR(seq_t x) {
  uint32_t p = x & 0xffff;
  p += 1;
  p = p & 0xffff;
  return p;
}

static inline seq_t PREDECESSOR(seq_t x) {
  uint32_t p = (x & 0xffff) + 0x10000;
  p -= 1;
  p = p & 0xffff;
  return p;
}

// anything with ORDINATE in it must be proctected by the ab_mutex
static inline int32_t ORDINATE(seq_t x) {
  int32_t p = x; // int32_t from seq_t, i.e. uint16_t, so okay
  int32_t q = ab_read; // int32_t from seq_t, i.e. uint16_t, so okay
  int32_t t = (p + 0x10000 - q) & 0xffff;
  // we definitely will get a positive number in t at this point, but it might be a
  // positive alias of a negative number, i.e. x might actually be "before" ab_read
  // So, if the result is greater than 32767, we will assume its an
  // alias and subtract 65536 from it
  if (t >= 32767) {
    // debug(1,"OOB: %u, ab_r: %u, ab_w: %u",x,ab_read,ab_write);
    t -= 65536;
  }
  return t;
}

// wrapped number between two seq_t.
int32_t seq_diff(seq_t a, seq_t b) {
  int32_t diff = ORDINATE(b) - ORDINATE(a);
  return diff;
}

// the sequence numbers will wrap pretty often.
// this returns true if the second arg is after the first
static inline int seq_order(seq_t a, seq_t b) {
  int32_t d = ORDINATE(b) - ORDINATE(a);
  return d > 0;
}

static inline seq_t seq_sum(seq_t a, seq_t b) {
  uint32_t p = a & 0xffff;
  uint32_t q = b & 0x0ffff;
  uint32_t r = (a + b) & 0xffff;
  return r;
}

// now for 32-bit wrapping in timestamps

// this returns true if the second arg is strictly after the first
// on the assumption that the gap between them is never greater than (2^31)-1
// Represent a and b in 64 bits
static inline int seq32_order(uint32_t a, uint32_t b) {
  if (a == b)
    return 0;
  int64_t A = a & 0xffffffff;
  int64_t B = b & 0xffffffff;
  int64_t C = B - A;
  // if bit 31 is set, it means either b is before (i.e. less than) a or
  // b is (2^31)-1 ahead of a.

  // If we assume the gap between b and a should never reach 2 billion, then
  // bit 31 == 0 means b is strictly after a
  return (C & 0x80000000) == 0;
}

static int alac_decode(short *dest, int *destlen, uint8_t *buf, int len) {
  // parameters: where the decoded stuff goes, its length in samples,
  // the incoming packet, the length of the incoming packet in bytes
  // destlen should contain the allowed max number of samples on entry
  
  if (len>MAX_PACKET) {
    warn("Incoming audio packet size is too large at %d; it should not exceed %d.",len,MAX_PACKET);
    return -1;
  }
  unsigned char packet[MAX_PACKET];
  unsigned char packetp[MAX_PACKET];
  // assert(len <= MAX_PACKET);
  int reply = 0; //everything okay
  int outsize=bytes_per_audio_frame*(*destlen); // the size the output should be, in bytes
  int toutsize = outsize;

  if (encrypted) {
    unsigned char iv[16];
    int aeslen = len & ~0xf;
    memcpy(iv, aesiv, sizeof(iv));
#ifdef HAVE_LIBPOLARSSL
    aes_crypt_cbc(&dctx, AES_DECRYPT, aeslen, iv, buf, packet);
#endif
#ifdef HAVE_LIBSSL
    AES_cbc_encrypt(buf, packet, aeslen, &aes, iv, AES_DECRYPT);
#endif
    memcpy(packet + aeslen, buf + aeslen, len - aeslen);
    alac_decode_frame(decoder_info, packet, dest, &outsize);
  } else {
    alac_decode_frame(decoder_info, buf, dest, &outsize);
  }

  if(outsize>toutsize) {
    debug(2,"Output from alac_decode larger (%d bytes, not frames) than expected (%d bytes) -- truncated, but buffer overflow possible! Encrypted = %d.",outsize, toutsize, encrypted);
    reply = -1; // output packet is the wrong size
 }
  *destlen = outsize / bytes_per_audio_frame;
  if ((outsize % bytes_per_audio_frame)!=0)
    debug(1,"Number of audio frames (%d) does not correspond exactly to the number of bytes (%d) and the audio frame size (%d).",*destlen,outsize,bytes_per_audio_frame);
  return reply;
}

static int init_decoder(int32_t fmtp[12]) {
  alac_file *alac;

  frame_size = fmtp[1]; // stereo samples
  sampling_rate = fmtp[11];

  int sample_size = fmtp[3];
  if (sample_size != 16)
    die("only 16-bit samples supported!");

  alac = alac_create(sample_size, 2);
  if (!alac)
    return 1;
  decoder_info = alac;

  alac->setinfo_max_samples_per_frame = frame_size;
  alac->setinfo_7a = fmtp[2];
  alac->setinfo_sample_size = sample_size;
  alac->setinfo_rice_historymult = fmtp[4];
  alac->setinfo_rice_initialhistory = fmtp[5];
  alac->setinfo_rice_kmodifier = fmtp[6];
  alac->setinfo_7f = fmtp[7];
  alac->setinfo_80 = fmtp[8];
  alac->setinfo_82 = fmtp[9];
  alac->setinfo_86 = fmtp[10];
  alac->setinfo_8a_rate = fmtp[11];
  alac_allocate_buffers(alac);
  return 0;
}

static void free_decoder(void) { alac_free(decoder_info); }

static void init_buffer(void) {
  int i;
  for (i = 0; i < BUFFER_FRAMES; i++)
    audio_buffer[i].data = malloc(bytes_per_audio_frame*(frame_size+max_frame_size_change));
  ab_resync();
}

static void free_buffer(void) {
  int i;
  for (i = 0; i < BUFFER_FRAMES; i++)
    free(audio_buffer[i].data);
}

void player_put_packet(seq_t seqno, uint32_t timestamp, uint8_t *data, int len) {

  // ignore a request to flush that has been made before the first packet...
  if (packet_count==0) {
    pthread_mutex_lock(&flush_mutex);
    flush_requested = 0;
    flush_rtp_timestamp = 0;
    pthread_mutex_unlock(&flush_mutex);
  }
  
  pthread_mutex_lock(&ab_mutex);
  packet_count++;
  time_of_last_audio_packet = get_absolute_time_in_fp();
  if (connection_state_to_output) { // if we are supposed to be processing these packets

//    if (flush_rtp_timestamp != 0)
//    	debug(1,"Flush_rtp_timestamp is %u",flush_rtp_timestamp);

    if ((flush_rtp_timestamp != 0) &&
        ((timestamp == flush_rtp_timestamp) || seq32_order(timestamp, flush_rtp_timestamp))) {
      debug(3, "Dropping flushed packet in player_put_packet, seqno %u, timestamp %u, flushing to "
               "timestamp: %u.",
            seqno, timestamp, flush_rtp_timestamp);
    } else {
      if ((flush_rtp_timestamp != 0x0) &&
          (!seq32_order(timestamp,
                        flush_rtp_timestamp))) // if we have gone past the flush boundary time
        flush_rtp_timestamp = 0x0;

      abuf_t *abuf = 0;

      if (!ab_synced) {
        debug(2, "syncing to seqno %u.", seqno);
        ab_write = seqno;
        ab_read = seqno;
        ab_synced = 1;
      }
      if (ab_write == seqno) { // expected packet
        abuf = audio_buffer + BUFIDX(seqno);
        ab_write = SUCCESSOR(seqno);
      } else if (seq_order(ab_write, seqno)) { // newer than expected
        // if (ORDINATE(seqno)>(BUFFER_FRAMES*7)/8)
        // debug(1,"An interval of %u frames has opened, with ab_read: %u, ab_write: %u and seqno:
        // %u.",seq_diff(ab_read,seqno),ab_read,ab_write,seqno);
        int32_t gap = seq_diff(ab_write, seqno);
        if (gap <= 0)
          debug(1, "Unexpected gap size: %d.", gap);
        int i;
        for (i = 0; i < gap; i++) {
          abuf = audio_buffer + BUFIDX(seq_sum(ab_write, i));
          abuf->ready = 0; // to be sure, to be sure
          abuf->timestamp = 0;
          abuf->sequence_number = 0;
        }
        // debug(1,"N %d s %u.",seq_diff(ab_write,PREDECESSOR(seqno))+1,ab_write);
        abuf = audio_buffer + BUFIDX(seqno);
//        rtp_request_resend(ab_write, gap);
//        resend_requests++;
        ab_write = SUCCESSOR(seqno);
      } else if (seq_order(ab_read, seqno)) { // late but not yet played
        late_packets++;
        abuf = audio_buffer + BUFIDX(seqno);
      } else { // too late.
        too_late_packets++;
        /*
        if (!late_packet_message_sent) {
                debug(1, "too-late packet received: %u; ab_read: %u; ab_write: %u.", seqno, ab_read,
        ab_write);
                late_packet_message_sent=1;
        }
        */
      }
      // pthread_mutex_unlock(&ab_mutex);

      if (abuf) {
        int datalen = frame_size;
        if (alac_decode(abuf->data, &datalen, data, len)==0) {
					abuf->ready = 1;
					abuf->length = datalen;
					abuf->timestamp = timestamp;
					abuf->sequence_number = seqno;

          if (config.playback_mode==ST_mono) {
            signed short *v = abuf->data;
            int i;
            int both;
            for (i=frame_size;i;i--) {
              int both = *v + *(v+1);
              if (both > INT16_MAX) {
                both = INT16_MAX;
              } else if (both < INT16_MIN) {
                both = INT16_MIN;
              }
              short sboth = (short)both;
              *v++ = sboth;
              *v++ = sboth;
            }
          }

        } else {
        	debug(1,"Bad audio packet detected and discarded.");
					abuf->ready = 0;
					abuf->timestamp = 0;
					abuf->sequence_number = 0;        
        }
      }

      // pthread_mutex_lock(&ab_mutex);
    }
    int rc = pthread_cond_signal(&flowcontrol);
    if (rc)
      debug(1, "Error signalling flowcontrol.");
  }
  pthread_mutex_unlock(&ab_mutex);
}

int32_t rand_in_range(int32_t exclusive_range_limit) {
  static uint32_t lcg_prev = 12345;
	// returns a pseudo random integer in the range 0 to (exclusive_range_limit-1) inclusive
	int64_t sp = lcg_prev;
	int64_t rl = exclusive_range_limit;
	lcg_prev = lcg_prev * 69069 + 3; // crappy psrg
	sp = sp*rl; // 64 bit calculation. INtersting part if above the 32 rightmost bits;
	return sp >> 32;  
}

static inline short dithered_vol(short sample) {
  long out;

  out = (long)sample * fix_volume;
  if (fix_volume < 0x10000) {
  
  	// add a TPDF dither -- see http://www.users.qwest.net/%7Evolt42/cadenzarecording/DitherExplained.pdf
  	// and the discussion around https://www.hydrogenaud.io/forums/index.php?showtopic=16963&st=25
  	
  	// I think, for a 32 --> 16 bits, the range of
  	// random numbers needs to be from -2^16 to 2^16, i.e. from -65536 to 65536 inclusive, not from -32768 to +32767
  	
  	// See the original paper at http://www.ece.rochester.edu/courses/ECE472/resources/Papers/Lipshitz_1992.pdf
  	// by Lipshitz, Wannamaker and Vanderkooy, 1992.
  	
  	long tpdf = rand_in_range(65536+1) - rand_in_range(65536+1);
  	// Check there's no clipping -- if there is, 
  	if (tpdf>=0) {
  		if (LONG_MAX-tpdf>=out)
    		out += tpdf;
    	else
    		out = LONG_MAX;
    } else {
    	if (LONG_MIN-tpdf<=out)
    		out += tpdf;
    	else
    		out = LONG_MIN;
    }
  }
  return out >> 16;
}

// get the next frame, when available. return 0 if underrun/stream reset.
static abuf_t *buffer_get_frame(void) {
  int16_t buf_fill;
  uint64_t local_time_now;
  // struct timespec tn;
  abuf_t *abuf = 0;
  int i;
  abuf_t *curframe;
  int notified_buffer_empty = 0; // diagnostic only

  pthread_mutex_lock(&ab_mutex);
  int wait;
  long dac_delay = 0; // long because alsa returns a long 
  do {
    // get the time
    local_time_now = get_absolute_time_in_fp(); // type okay

    // if config.timeout (default 120) seconds have elapsed since the last audio packet was
    // received, then we should stop.
    // config.timeout of zero means don't check..., but iTunes may be confused by a long gap
    // followed by a resumption...

    if ((time_of_last_audio_packet != 0) && (shutdown_requested == 0) &&
        (config.dont_check_timeout == 0)) {
      uint64_t ct = config.timeout; // go from int to 64-bit int
      if ((local_time_now > time_of_last_audio_packet) &&
          (local_time_now - time_of_last_audio_packet >= ct << 32)) {
        debug(1, "As Yeats almost said, \"Too long a silence / can make a stone of the heart\"");
        rtsp_request_shutdown_stream();
        shutdown_requested = 1;
      }
    }
    int rco = get_requested_connection_state_to_output();

    if (connection_state_to_output != rco) {
      connection_state_to_output = rco;
      // change happening
      if (connection_state_to_output == 0) { // going off
        pthread_mutex_lock(&flush_mutex);
        flush_requested = 1;
        pthread_mutex_unlock(&flush_mutex);
      }
    }

    pthread_mutex_lock(&flush_mutex);
    if (flush_requested == 1) {
      if (config.output->flush)
        config.output->flush();
      ab_resync();
      first_packet_timestamp = 0;
      first_packet_time_to_play = 0;
      time_since_play_started = 0;
      flush_requested = 0;
    }
    pthread_mutex_unlock(&flush_mutex);
    uint32_t flush_limit = 0;
    if (ab_synced) {
      do {
        curframe = audio_buffer + BUFIDX(ab_read);
        if ((ab_read!=ab_write) && (curframe->ready)) { // it could be synced and empty, under exceptional circumstances, with the frame unused, thus apparently ready

          if (curframe->sequence_number != ab_read) {
            // some kind of sync problem has occurred.
            if (BUFIDX(curframe->sequence_number) == BUFIDX(ab_read)) {
              // it looks like some kind of aliasing has happened
              if (seq_order(ab_read, curframe->sequence_number)) {
                ab_read = curframe->sequence_number;
                debug(1, "Aliasing of buffer index -- reset.");
              }
            } else {
              debug(1, "Inconsistent sequence numbers detected");
            }
          }

          if ((flush_rtp_timestamp != 0) &&
              ((curframe->timestamp == flush_rtp_timestamp) ||
               seq32_order(curframe->timestamp, flush_rtp_timestamp))) {
            debug(1, "Dropping flushed packet seqno %u, timestamp %u", curframe->sequence_number,
                  curframe->timestamp);
            curframe->ready = 0;
            flush_limit++;
            ab_read = SUCCESSOR(ab_read);
          }
          if ((flush_rtp_timestamp != 0) &&
              (!seq32_order(curframe->timestamp,
                            flush_rtp_timestamp))) // if we have gone past the flush boundary time
            flush_rtp_timestamp = 0;
        }
      } while ((flush_rtp_timestamp != 0) && (flush_limit <= 8820) && (curframe->ready == 0));

      if (flush_limit == 8820) {
        debug(1, "Flush hit the 8820 frame limit!");
        flush_limit = 0;
      }

      curframe = audio_buffer + BUFIDX(ab_read);

      if (curframe->ready) {
        notified_buffer_empty=0; // at least one buffer now -- diagnostic only.
        if (ab_buffering) { // if we are getting packets but not yet forwarding them to the player
          int have_sent_prefiller_silence; // set true when we have send some silent frames to the DAC
          uint32_t reference_timestamp;
          uint64_t reference_timestamp_time,remote_reference_timestamp_time;
          get_reference_timestamp_stuff(&reference_timestamp, &reference_timestamp_time, &remote_reference_timestamp_time);
          if (first_packet_timestamp == 0) { // if this is the very first packet
            // debug(1,"First frame seen, time %u, with %d
            // frames...",curframe->timestamp,seq_diff(ab_read, ab_write));
            if (reference_timestamp) { // if we have a reference time
              // debug(1,"First frame seen with timestamp...");
              first_packet_timestamp = curframe->timestamp; // we will keep buffering until we are
                                                            // supposed to start playing this
              have_sent_prefiller_silence = 0;

              // Here, calculate when we should start playing. We need to know when to allow the
              // packets to be sent to the player.
              // We will send packets of silence from now until that time and then we will send the
              // first packet, which will be followed by the subsequent packets.

              // we will get a fix every second or so, which will be stored as a pair consisting of
              // the time when the packet with a particular timestamp should be played, neglecting
              // latencies, etc.

              // It probably won't  be the timestamp of our first packet, however, so we might have
              // to do some calculations.

              // To calculate when the first packet will be played, we figure out the exact time the
              // packet should be played according to its timestamp and the reference time.
              // We then need to add the desired latency, typically 88200 frames.

              // Then we need to offset this by the backend latency offset. For example, if we knew
              // that the audio back end has a latency of 100 ms, we would
              // ask for the first packet to be emitted 100 ms earlier than it should, i.e. -4410
              // frames, so that when it got through the audio back end,
              // if would be in sync. To do this, we would give it a latency offset of -100 ms, i.e.
              // -4410 frames.

              int64_t delta = ((int64_t)first_packet_timestamp - (int64_t)reference_timestamp)+config.latency+config.audio_backend_latency_offset; // uint32_t to int64_t is okay and int32t to int64t promotion is okay.
              
              if (delta>=0) {
                uint64_t delta_fp_sec = (delta << 32) / 44100; // int64_t which is positive
                first_packet_time_to_play=reference_timestamp_time+delta_fp_sec;
              } else {
                int64_t abs_delta = -delta;
                uint64_t delta_fp_sec = (abs_delta << 32) / 44100; // int64_t which is positive
                first_packet_time_to_play=reference_timestamp_time-delta_fp_sec;              
              }

              if (local_time_now >= first_packet_time_to_play) {
                debug(
                    1,
                    "First packet is late! It should have played before now. Flushing 0.1 seconds");
                player_flush(first_packet_timestamp + 4410);
              }
            }
          }

          if (first_packet_time_to_play != 0) {
            // recalculate first_packet_time_to_play -- the latency might change
            int64_t delta = ((int64_t)first_packet_timestamp - (int64_t)reference_timestamp)+config.latency+config.audio_backend_latency_offset; // uint32_t to int64_t is okay and int32t to int64t promotion is okay.
            
            if (delta>=0) {
              uint64_t delta_fp_sec = (delta << 32) / 44100; // int64_t which is positive
              first_packet_time_to_play=reference_timestamp_time+delta_fp_sec;
            } else {
              int64_t abs_delta = -delta;
              uint64_t delta_fp_sec = (abs_delta << 32) / 44100; // int64_t which is positive
              first_packet_time_to_play=reference_timestamp_time-delta_fp_sec;              
            }

            int64_t max_dac_delay = 4410;
            int64_t filler_size = max_dac_delay; // 0.1 second -- the maximum we'll add to the DAC

            if (local_time_now >= first_packet_time_to_play) {
              // we've gone past the time...
              // debug(1,"Run past the exact start time by %llu frames, with time now of %llx, fpttp
              // of %llx and dac_delay of %d and %d packets;
              // flush.",(((tn-first_packet_time_to_play)*44100)>>32)+dac_delay,tn,first_packet_time_to_play,dac_delay,seq_diff(ab_read,
              // ab_write));

              if (config.output->flush)
                config.output->flush();
              ab_resync();
              first_packet_timestamp = 0;
              first_packet_time_to_play = 0;
              time_since_play_started = 0;
            } else {
              // first_packet_time_to_play is definitely later than local_time_now
              if ((config.output->delay) && (have_sent_prefiller_silence != 0)) {
          			int resp = config.output->delay(&dac_delay);
                if (resp != 0) {
                  debug(1, "Error %d getting dac_delay in buffer_get_frame.",resp);
                  dac_delay = 0;
                }              
              } else
                dac_delay = 0;
              int64_t gross_frame_gap =
                  ((first_packet_time_to_play - local_time_now) * 44100) >> 32;
              int64_t exact_frame_gap = gross_frame_gap - dac_delay;
              if (exact_frame_gap < 0) {
                // we've gone past the time...
                // debug(1,"Run a bit past the exact start time by %lld frames, with time now of
                // %llx, fpttp of %llx and dac_delay of %d and %d packets;
                // flush.",-exact_frame_gap,tn,first_packet_time_to_play,dac_delay,seq_diff(ab_read,
                // ab_write));
                if (config.output->flush)
                  config.output->flush();
                ab_resync();
                first_packet_timestamp = 0;
                first_packet_time_to_play = 0;
              } else {
                int64_t fs = filler_size;
                if (fs > (max_dac_delay - dac_delay))
                  fs = max_dac_delay - dac_delay;
                if (fs<0) {
                  debug(2,"frame size (fs) < 0 with max_dac_delay of %lld and dac_delay of %ld",max_dac_delay, dac_delay);
                  fs=0;
                }
                if ((exact_frame_gap <= fs) || (exact_frame_gap <= frame_size * 2)) {
                  fs = exact_frame_gap;
                  // debug(1,"Exact frame gap is %llu; play %d frames of silence. Dac_delay is %d,
                  // with %d packets, ab_read is %04x, ab_write is
                  // %04x.",exact_frame_gap,fs,dac_delay,seq_diff(ab_read,
                  // ab_write),ab_read,ab_write);
                  ab_buffering = 0;
                }
                signed short *silence;
                //if (fs==0)
                //  debug(2,"Zero length silence buffer needed with gross_frame_gap of %lld and dac_delay of %lld.",gross_frame_gap,dac_delay);
                // the fs (number of frames of silence to play) can be zero in the DAC doesn't start ouotputting frames for a while -- it could get loaded up but not start responding for many milliseconds.
                if (fs!=0) {
                  silence = malloc(bytes_per_audio_frame*fs);
                  if (silence==NULL)
                    debug(1,"Failed to allocate %d byte silence buffer.",fs);
                  else {
                    memset(silence, 0, bytes_per_audio_frame*fs);
                    // debug(1,"Exact frame gap is %llu; play %d frames of silence. Dac_delay is %d,
                    // with %d packets.",exact_frame_gap,fs,dac_delay,seq_diff(ab_read, ab_write));
                    config.output->play(silence, fs);
                    free(silence);
                    have_sent_prefiller_silence = 1;
                  }
                }
              }
            }
          }
          if (ab_buffering == 0) {
            // not the time of the playing of the first frame
            uint64_t reference_timestamp_time; // don't need this...
            get_reference_timestamp_stuff(&play_segment_reference_frame, &reference_timestamp_time, &play_segment_reference_frame_remote_time);
#ifdef CONFIG_METADATA
            send_ssnc_metadata('prsm', NULL, 0, 0); // "resume", but don't wait if the queue is locked
#endif
          }              
        }
      }
    }

    // Here, we work out whether to release a packet or wait
    // We release a buffer when the time is right.

    // To work out when the time is right, we need to take account of (1) the actual time the packet
    // should be released,
    // (2) the latency requested, (3) the audio backend latency offset and (4) the desired length of
    // the audio backend's buffer

    // The time is right if the current time is later or the same as
    // The packet time + (latency + latency offset - backend_buffer_length).
    // Note: the last three items are expressed in frames and must be converted to time.

    int do_wait = 0; // don't wait unless we can really prove we must
    if ((ab_synced) && (curframe) && (curframe->ready) && (curframe->timestamp)) {
    	do_wait = 1; // if the current frame exists and is ready, then wait unless it's time to let it go...
      uint32_t reference_timestamp;
      uint64_t reference_timestamp_time,remote_reference_timestamp_time;
      get_reference_timestamp_stuff(&reference_timestamp, &reference_timestamp_time, &remote_reference_timestamp_time); // all types okay
      if (reference_timestamp) { // if we have a reference time
        uint32_t packet_timestamp = curframe->timestamp; // types okay
        int64_t delta = (int64_t)packet_timestamp - (int64_t)reference_timestamp; // uint32_t to int64_t is okay.
        int64_t offset = config.latency + config.audio_backend_latency_offset -
                         config.audio_backend_buffer_desired_length; // all arguments are int32_t, so expression promotion okay
        int64_t net_offset = delta + offset; // okay
        uint64_t time_to_play = reference_timestamp_time; // type okay
        if (net_offset >= 0) {
          uint64_t net_offset_fp_sec = (net_offset << 32) / 44100; // int64_t which is positive
          time_to_play += net_offset_fp_sec; // using the latency requested...
          // debug(2,"Net Offset: %lld, adjusted: %lld.",net_offset,net_offset_fp_sec);
        } else {
          int64_t abs_net_offset = -net_offset;
          uint64_t net_offset_fp_sec = (abs_net_offset << 32) / 44100; // int64_t which is positive
          time_to_play -= net_offset_fp_sec;
          // debug(2,"Net Offset: %lld, adjusted: -%lld.",net_offset,net_offset_fp_sec);
        }

        if (local_time_now >= time_to_play) {
          do_wait = 0;
        }
      }
    }
    if (do_wait==0)
      if ((ab_synced!=0) && (ab_read==ab_write)) { // the buffer is empty!
        if (notified_buffer_empty==0) {
          debug(1,"Buffers exhausted.");
          notified_buffer_empty=1;
        }
        do_wait=1;
      }
    wait = (ab_buffering || (do_wait != 0) || (!ab_synced)) && (!please_stop);

    if (wait) {
      uint64_t time_to_wait_for_wakeup_fp =
          ((uint64_t)1 << 32) / 44100;       // this is time period of one frame
      time_to_wait_for_wakeup_fp *= 4 * 352; // four full 352-frame packets
      time_to_wait_for_wakeup_fp /= 3; // four thirds of a packet time

#ifdef COMPILE_FOR_LINUX_AND_FREEBSD_AND_CYGWIN
      uint64_t time_of_wakeup_fp = local_time_now + time_to_wait_for_wakeup_fp;
      uint64_t sec = time_of_wakeup_fp >> 32;
      uint64_t nsec = ((time_of_wakeup_fp & 0xffffffff) * 1000000000) >> 32;

      struct timespec time_of_wakeup;
      time_of_wakeup.tv_sec = sec;
      time_of_wakeup.tv_nsec = nsec;

      pthread_cond_timedwait(&flowcontrol, &ab_mutex, &time_of_wakeup);
// int rc = pthread_cond_timedwait(&flowcontrol,&ab_mutex,&time_of_wakeup);
// if (rc!=0)
//  debug(1,"pthread_cond_timedwait returned error code %d.",rc);
#endif
#ifdef COMPILE_FOR_OSX
      uint64_t sec = time_to_wait_for_wakeup_fp >> 32;
      ;
      uint64_t nsec = ((time_to_wait_for_wakeup_fp & 0xffffffff) * 1000000000) >> 32;
      struct timespec time_to_wait;
      time_to_wait.tv_sec = sec;
      time_to_wait.tv_nsec = nsec;
      pthread_cond_timedwait_relative_np(&flowcontrol, &ab_mutex, &time_to_wait);
#endif
    }
  } while (wait);

  if (please_stop) {
    pthread_mutex_unlock(&ab_mutex);
    return 0;
  }

  seq_t read = ab_read;

  // check if t+8, t+16, t+32, t+64, t+128, ... (buffer_start_fill / 2)
  // packets have arrived... last-chance resend

  if (!ab_buffering) {
    for (i = 8; i < (seq_diff(ab_read, ab_write) / 2); i = (i * 2)) {
      seq_t next = seq_sum(ab_read, i);
      abuf = audio_buffer + BUFIDX(next);
      if (!abuf->ready) {
        rtp_request_resend(next, 1);
        // debug(1,"Resend %u.",next);
        resend_requests++;
      }
    }
  }


  if (!curframe->ready) {
    // debug(1, "Supplying a silent frame for frame %u", read);
    missing_packets++;
    memset(curframe->data, 0, bytes_per_audio_frame*frame_size);
    curframe->timestamp = 0;
  }
  curframe->ready = 0;
  ab_read = SUCCESSOR(ab_read);
  pthread_mutex_unlock(&ab_mutex);
  return curframe;
}

static inline short shortmean(short a, short b) {
  long al = (long)a;
  long bl = (long)b;
  long longmean = (al + bl) / 2;
  short r = (short)longmean;
  if (r != longmean)
    debug(1, "Error calculating average of two shorts");
  return r;
}

// stuff: 1 means add 1; 0 means do nothing; -1 means remove 1
static int stuff_buffer_basic(short *inptr, int length, short *outptr, int stuff) {
  if ((stuff > 1) || (stuff < -1) || (length <100)) {
    // debug(1, "Stuff argument to stuff_buffer must be from -1 to +1 and length >100.");
    return length;
  }
  int i;
  int stuffsamp = length;
  if (stuff)
    //      stuffsamp = rand() % (length - 1);
    stuffsamp =
        (rand() % (length - 2)) + 1; // ensure there's always a sample before and after the item

  pthread_mutex_lock(&vol_mutex);
  for (i = 0; i < stuffsamp; i++) { // the whole frame, if no stuffing
    *outptr++ = dithered_vol(*inptr++);
    *outptr++ = dithered_vol(*inptr++);
  };
  if (stuff) {
    if (stuff == 1) {
      // debug(3, "+++++++++");
      // interpolate one sample
      //*outptr++ = dithered_vol(((long)inptr[-2] + (long)inptr[0]) >> 1);
      //*outptr++ = dithered_vol(((long)inptr[-1] + (long)inptr[1]) >> 1);
      *outptr++ = dithered_vol(shortmean(inptr[-2], inptr[0]));
      *outptr++ = dithered_vol(shortmean(inptr[-1], inptr[1]));
    } else if (stuff == -1) {
      // debug(3, "---------");
      inptr++;
      inptr++;
    }
    
    // if you're removing, i.e. stuff < 0, copy that much less over. If you're adding, do all the rest.
    int remainder = length;
    if (stuff<0)
      remainder = remainder+stuff; // don't run over the correct end of the output buffer

    for (i = stuffsamp; i < remainder; i++) {
      *outptr++ = dithered_vol(*inptr++);
      *outptr++ = dithered_vol(*inptr++);
    }
  }
  pthread_mutex_unlock(&vol_mutex);

  return length + stuff;
}

#ifdef HAVE_LIBSOXR
// stuff: 1 means add 1; 0 means do nothing; -1 means remove 1
static int stuff_buffer_soxr(short *inptr, int length, short *outptr, int stuff) {
  if ((stuff > 1) || (stuff < -1) || (length < 100)) {
    // debug(1, "Stuff argument to sox_stuff_buffer must be from -1 to +1 and length must be > 100.");
    return length;
  }
  int i;
  short *ip, *op;
  ip = inptr;
  op = outptr;

  if (stuff) {
    // debug(1,"Stuff %d.",stuff);
    soxr_io_spec_t io_spec;
    io_spec.itype = SOXR_INT16_I;
    io_spec.otype = SOXR_INT16_I;
    io_spec.scale = 1.0; // this seems to crash if not = 1.0
    io_spec.e = NULL;
    io_spec.flags = 0;

    size_t odone;

    soxr_error_t error = soxr_oneshot(length, length + stuff, 2, /* Rates and # of chans. */
                                      inptr, length, NULL,           /* Input. */
                                      outptr, length + stuff, &odone, /* Output. */
                                      &io_spec,    /* Input, output and transfer spec. */
                                      NULL, NULL); /* Default configuration.*/

    if (error)
      die("soxr error: %s\n", "error: %s\n", soxr_strerror(error));

    if (odone > length + 1)
      die("odone = %d!\n", odone);

    const int gpm = 5;

    // keep the first (dpm) samples, to mitigate the Gibbs phenomenon
    for (i = 0; i < gpm; i++) {
      *op++ = *ip++;
      *op++ = *ip++;
    }

    // keep the last (dpm) samples, to mitigate the Gibbs phenomenon
    op = outptr + (length + stuff - gpm) * sizeof(short);
    ip = inptr + (length - gpm) * sizeof(short);
    for (i = 0; i < gpm; i++) {
      *op++ = *ip++;
      *op++ = *ip++;
    }

    // finally, adjust the volume, if necessary
    if (fix_volume != 65536.0) {
      // pthread_mutex_lock(&vol_mutex);
      op = outptr;
      for (i = 0; i < length + stuff; i++) {
        *op = dithered_vol(*op);
        op++;
        *op = dithered_vol(*op);
        op++;
      };
      // pthread_mutex_unlock(&vol_mutex);
    }

  } else { // the whole frame, if no stuffing

    // pthread_mutex_lock(&vol_mutex);
    for (i = 0; i < length; i++) {
      *op++ = dithered_vol(*ip++);
      *op++ = dithered_vol(*ip++);
    };
    // pthread_mutex_unlock(&vol_mutex);
  }
  return length + stuff;
}
#endif

typedef struct stats { // statistics for running averages
  int64_t sync_error, correction, drift;
} stats_t;

static void *player_thread_func(void *arg) {
	struct inter_threads_record itr;
	itr.please_stop = 0;
	// create and start the timing, control and audio receiver threads
	pthread_t rtp_audio_thread, rtp_control_thread, rtp_timing_thread;
	pthread_create(&rtp_audio_thread, NULL, &rtp_audio_receiver, (void *)&itr);
  pthread_create(&rtp_control_thread, NULL, &rtp_control_receiver, (void *)&itr);
  pthread_create(&rtp_timing_thread, NULL, &rtp_timing_receiver, (void *)&itr);

		session_corrections = 0;
		play_segment_reference_frame = 0; // zero signals that we are not in a play segment
	// check that there are enough buffers to accommodate the desired latency and the latency offset
	
	int maximum_latency = config.latency+config.audio_backend_latency_offset;
	if ((maximum_latency+(352-1))/352 + 10 > BUFFER_FRAMES)
		die("Not enough buffers available for a total latency of %d frames. A maximum of %d 352-frame packets may be accommodated.",maximum_latency,BUFFER_FRAMES);
  connection_state_to_output = get_requested_connection_state_to_output();
// this is about half a minute
#define trend_interval 3758
  stats_t statistics[trend_interval];
  int number_of_statistics, oldest_statistic, newest_statistic;
  int at_least_one_frame_seen = 0;
  int64_t tsum_of_sync_errors, tsum_of_corrections, tsum_of_insertions_and_deletions,
      tsum_of_drifts;
  int64_t previous_sync_error, previous_correction;
  int64_t minimum_dac_queue_size = INT64_MAX;
  int32_t minimum_buffer_occupancy = INT32_MAX;
  int32_t maximum_buffer_occupancy = INT32_MIN;

  time_t playstart = time(NULL);

  buffer_occupancy = 0;

  int play_samples;
  int64_t current_delay;
  int play_number = 0;
  time_of_last_audio_packet = 0;
  shutdown_requested = 0;
  number_of_statistics = oldest_statistic = newest_statistic = 0;
  tsum_of_sync_errors = tsum_of_corrections = tsum_of_insertions_and_deletions = tsum_of_drifts = 0;

  const int print_interval = trend_interval; // don't ask...
  // I think it's useful to keep this prime to prevent it from falling into a pattern with some
  // other process.

  char rnstate[256];
  initstate(time(NULL), rnstate, 256);

  signed short *inbuf, *outbuf, *silence;
  outbuf = malloc(bytes_per_audio_frame*(frame_size+max_frame_size_change));
  if (outbuf==NULL)
    debug(1,"Failed to allocate memory for an output buffer.");
  silence = malloc(bytes_per_audio_frame*frame_size);
  if (silence==NULL)
    debug(1,"Failed to allocate memory for a silence buffer.");
  memset(silence, 0, bytes_per_audio_frame*frame_size);
  late_packet_message_sent = 0;
  first_packet_timestamp = 0;
  missing_packets = late_packets = too_late_packets = resend_requests = 0;
  flush_rtp_timestamp = 0; // it seems this number has a special significance -- it seems to be used
                           // as a null operand, so we'll use it like that too
  int sync_error_out_of_bounds = 0; // number of times in a row that there's been a serious sync error

  if (config.statistics_requested) {
    if ((config.output->delay)) {
      if (config.no_sync==0) {
        inform("sync error in frames, "
               "net correction in ppm, "
               "corrections in ppm, "
               "total packets, "
               "missing packets, "
               "late packets, "
               "too late packets, "
               "resend requests, "
               "min DAC queue size, "
               "min buffer occupancy, "
               "max buffer occupancy");
      } else {
        inform("sync error in frames, "
               "total packets, "
               "missing packets, "
               "late packets, "
               "too late packets, "
               "resend requests, "
               "min DAC queue size, "
               "min buffer occupancy, "
               "max buffer occupancy");
      }
    } else {
      inform("total packets, "
             "missing packets, "
             "late packets, "
             "too late packets, "
             "resend requests, "
             "min buffer occupancy, "
             "max buffer occupancy");
    }
  }

  uint64_t tens_of_seconds = 0;
  while (!please_stop) {
    abuf_t *inframe = buffer_get_frame();
    if (inframe) {
      if (inframe->data) {
        play_number++;
        // if it's a supplied silent frame, let us know...
        if (inframe->timestamp == 0) {
          // debug(1,"Player has a supplied silent frame.");
          last_seqno_read =
              (SUCCESSOR(last_seqno_read) & 0xffff); // manage the packet out of sequence minder
          if (inframe->data==NULL)
            debug(1,"NULL inframe->data to play -- skipping it.");
          else {
            if (inframe->length==0)
              debug(1,"empty frame to play -- skipping it.");
            else
              config.output->play(inframe->data, inframe->length);
          }
        } else {
          // We have a frame of data. We need to see if we want to add or remove a frame from it to
          // keep in sync.
          // So we calculate the timing error for the first frame in the DAC.
          // If it's ahead of time, we add one audio frame to this frame to delay a subsequent frame
          // If it's late, we remove an audio frame from this frame to bring a subsequent frame
          // forward in time

          at_least_one_frame_seen = 1;

          uint32_t reference_timestamp;
          uint64_t reference_timestamp_time,remote_reference_timestamp_time;
          get_reference_timestamp_stuff(&reference_timestamp, &reference_timestamp_time, &remote_reference_timestamp_time); // types okay

          int64_t rt, nt;
          rt = reference_timestamp; // uint32_t to int64_t
          nt = inframe->timestamp; // uint32_t to int64_t

          uint64_t local_time_now = get_absolute_time_in_fp(); // types okay
          // struct timespec tn;
          // clock_gettime(CLOCK_MONOTONIC,&tn);
          // uint64_t
          // local_time_now=((uint64_t)tn.tv_sec<<32)+((uint64_t)tn.tv_nsec<<32)/1000000000;

          int64_t td = 0;
          int64_t td_in_frames = 0;
          if (local_time_now >= reference_timestamp_time) {
          // debug(1,"td is %lld.",td);
             td = local_time_now - reference_timestamp_time; // this is the positive value. Conversion is positive uint64_t to int64_t, thus okay
            td_in_frames = (td * 44100) >> 32;
          } else {
            td = reference_timestamp_time - local_time_now; // this is the absolute value, which should be negated. Conversion is positive uint64_t to int64_t, thus okay.
            td_in_frames = (td * 44100) >> 32; //use the absolute td value for the present. Types okay
            td_in_frames = -td_in_frames;
            td = -td; // should be okay, as the range of values should be very small w.r.t 64 bits
          }

          // This is the timing error for the next audio frame in the DAC, if applicable
          int64_t sync_error = 0;

          int amount_to_stuff = 0;

          // check sequencing
          if (last_seqno_read == -1)
            last_seqno_read = inframe->sequence_number; //int32_t from seq_t, i.e. uint16_t, so okay.
          else {
            last_seqno_read = SUCCESSOR(last_seqno_read); // int32_t from seq_t, i.e. uint16_t, so okay.
            if (inframe->sequence_number != last_seqno_read) { //seq_t, ei.e. uint16_t and int32_t, so okay
              debug(
                  1,
                  "Player: packets out of sequence: expected: %u, got: %u, with ab_read: %u and ab_write: %u.",
                  last_seqno_read, inframe->sequence_number,ab_read,ab_write);
              last_seqno_read = inframe->sequence_number; // reset warning...
            }
          }

					buffer_occupancy = seq_diff(ab_read, ab_write); // int32_t from int32

					if (buffer_occupancy < minimum_buffer_occupancy)
						minimum_buffer_occupancy = buffer_occupancy;

					if (buffer_occupancy > maximum_buffer_occupancy)
						maximum_buffer_occupancy = buffer_occupancy;

          // here, we want to check (a) if we are meant to do synchronisation,
          // (b) if we have a delay procedure, (c) if we can get the delay.

          // If any of these are false, we don't do any synchronisation stuff

					int resp = -1; // use this as a flag -- if negative, we can't rely on a real known delay
          current_delay = -1; // use this as a failure flag
 
					if (config.output->delay) {
						long l_delay; 
						resp = config.output->delay(&l_delay);
						current_delay = l_delay;
						if (resp==0) { // no error
							if (current_delay<0) {
								debug(1,"Underrun of %d frames reported, but ignored.",current_delay);
								current_delay=0; // could get a negative value if there was underrun, but ignore it.
							}
							if (current_delay < minimum_dac_queue_size) {
								minimum_dac_queue_size = current_delay; // update for display later
							}
						} else { 
							debug(2, "Delay error %d when checking running latency.",resp);
						}
					}

          if (resp >= 0) {

            // this is the actual delay, including the latency we actually want, which will
            // fluctuate a good bit about a potentially rising or falling trend.
            int64_t delay = td_in_frames + rt - (nt - current_delay); // all int64_t
            

            // This is the timing error for the next audio frame in the DAC.
            sync_error = delay - config.latency; // int64_t from int64_t - int32_t, so okay

            // if (llabs(sync_error)>352*512)
            //  debug(1,"Very large sync error: %lld",sync_error);

            // before we finally commit to this frame, check its sequencing and timing

            // require a certain error before bothering to fix it...
            if (sync_error > config.tolerance) { // int64_t > int, okay
              amount_to_stuff = -1;
            }
            if (sync_error < -config.tolerance) {
              amount_to_stuff = 1;
            }

            // only allow stuffing if there is enough time to do it -- check DAC buffer...
            if (current_delay < DAC_BUFFER_QUEUE_MINIMUM_LENGTH) {
              // debug(1,"DAC buffer too short to allow stuffing.");
              amount_to_stuff = 0;
            }

            // try to keep the corrections definitely below 1 in 1000 audio frames
            
            // calculate the time elapsed since the play session started.
            
            if (amount_to_stuff) {
              if ((local_time_now) && (first_packet_time_to_play) && (local_time_now >= first_packet_time_to_play)) {

                int64_t tp = (local_time_now - first_packet_time_to_play)>>32; // seconds int64_t from uint64_t which is always positive, so ok
                
                if (tp<5)
                  amount_to_stuff = 0; // wait at least five seconds
                else if (tp<30) {
                  if ((random() % 1000) > 352) // keep it to about 1:1000 for the first thirty seconds
                    amount_to_stuff = 0;
                }
              }
            }
            
            if (config.no_sync!=0)
              amount_to_stuff = 0 ; // no stuffing if it's been disabled
                        
            if ((amount_to_stuff == 0) && (fix_volume == 0x10000)) {
              // if no stuffing needed and no volume adjustment, then
              // don't send to stuff_buffer_* and don't copy to outbuf; just send directly to the
              // output device...
              if (inframe->data==NULL)
                debug(1,"NULL inframe->data to play -- skipping it.");
              else {
                if (inframe->length==0)
                  debug(1,"empty frame to play -- skipping it (2).");
                else
                  config.output->play(inframe->data, inframe->length);
              }
            } else {
#ifdef HAVE_LIBSOXR
              switch (config.packet_stuffing) {
              case ST_basic:
                //                if (amount_to_stuff) debug(1,"Basic stuff...");
                play_samples = stuff_buffer_basic(inframe->data, inframe->length, outbuf, amount_to_stuff);
                break;
              case ST_soxr:
                //                if (amount_to_stuff) debug(1,"Soxr stuff...");
                play_samples = stuff_buffer_soxr(inframe->data, inframe->length, outbuf, amount_to_stuff);
                break;
              }
#else
              //          if (amount_to_stuff) debug(1,"Standard stuff...");
              play_samples = stuff_buffer_basic(inframe->data, inframe->length, outbuf, amount_to_stuff);
#endif

              /*
              {
                int co;
                int is_silent=1;
                short *p = outbuf;
                for (co=0;co<play_samples;co++) {
                  if (*p!=0)
                    is_silent=0;
                  p++;
                }
                if (is_silent)
                  debug(1,"Silence!");
              }
              */
              
              
              if (outbuf==NULL)
                debug(1,"NULL outbuf to play -- skipping it.");
              else {
                if (play_samples==0)
                  debug(1,"play_samples==0 skipping it (1).");
                else
                  config.output->play(outbuf, play_samples);
              }
            }

            // check for loss of sync
            // timestamp of zero means an inserted silent frame in place of a missing frame
            
            // not too sure if abs() is implemented for int64_t, so we'll do it manually
            int64_t abs_sync_error = sync_error;
            if (abs_sync_error<0)
              abs_sync_error = -abs_sync_error;
            
            if ((config.no_sync==0) && (inframe->timestamp != 0) && (!please_stop) && (config.resyncthreshold != 0) &&
                (abs_sync_error > config.resyncthreshold)) {
              sync_error_out_of_bounds++;
              // debug(1,"Sync error out of bounds: Error: %lld; previous error: %lld; DAC: %lld;
              // timestamp: %llx, time now
              // %llx",sync_error,previous_sync_error,current_delay,inframe->timestamp,local_time_now);
              if (sync_error_out_of_bounds > 3) {
                debug(1, "Lost sync with source for %d consecutive packets -- flushing and "
                         "resyncing. Error: %lld.",
                      sync_error_out_of_bounds, sync_error);
                sync_error_out_of_bounds = 0;
                player_flush(nt);
              }
            } else {
              sync_error_out_of_bounds = 0;
            }
          } else {
            // if there is no delay procedure, or it's not working or not allowed, there can be no synchronising
            
            if (fix_volume == 0x10000)
            
              if (inframe->data==NULL)
                debug(1,"NULL inframe->data to play -- skipping it.");
              else {
                if (inframe->length==0)
                  debug(1,"empty frame to play -- skipping it (3).");
                else
                  config.output->play(inframe->data, inframe->length);
              }
            else {
              play_samples = stuff_buffer_basic(inframe->data, inframe->length, outbuf, 0); // no stuffing, but volume adjustment

              if (outbuf==NULL)
                debug(1,"NULL outbuf to play -- skipping it.");
              else {
                if (inframe->length==0)
                  debug(1,"empty frame to play -- skipping it (4).");
                else
                  config.output->play(outbuf, play_samples);
              }
            }
          }

          // mark the frame as finished
          inframe->timestamp = 0;
          inframe->sequence_number = 0;

          // debug(1,"Sync error %lld frames. Amount to stuff %d." ,sync_error,amount_to_stuff);

          // new stats calculation. We want a running average of sync error, drift, adjustment,
          // number of additions+subtractions
          
          // this is a misleading hack -- the statistics should include some data on the number of valid samples and the number of times sync wasn't checked due to non availability of a delay figure.
          // for the present, stats are only updated when sync has been checked
          if (sync_error!=-1) {
            if (number_of_statistics == trend_interval) {
              // here we remove the oldest statistical data and take it from the summaries as well
              tsum_of_sync_errors -= statistics[oldest_statistic].sync_error;
              tsum_of_drifts -= statistics[oldest_statistic].drift;
              if (statistics[oldest_statistic].correction > 0)
                tsum_of_insertions_and_deletions -= statistics[oldest_statistic].correction;
              else
                tsum_of_insertions_and_deletions += statistics[oldest_statistic].correction;
              tsum_of_corrections -= statistics[oldest_statistic].correction;
              oldest_statistic = (oldest_statistic + 1) % trend_interval;
              number_of_statistics--;
            }

            statistics[newest_statistic].sync_error = sync_error;
            statistics[newest_statistic].correction = amount_to_stuff;

            if (number_of_statistics == 0)
              statistics[newest_statistic].drift = 0;
            else
              statistics[newest_statistic].drift =
                  sync_error - previous_sync_error - previous_correction;

            previous_sync_error = sync_error;
            previous_correction = amount_to_stuff;

            tsum_of_sync_errors += sync_error;
            tsum_of_drifts += statistics[newest_statistic].drift;
            if (amount_to_stuff > 0) {
              tsum_of_insertions_and_deletions += amount_to_stuff;
            } else {
              tsum_of_insertions_and_deletions -= amount_to_stuff;
            }
            tsum_of_corrections += amount_to_stuff;
            session_corrections += amount_to_stuff;


            newest_statistic = (newest_statistic + 1) % trend_interval;
            number_of_statistics++;
            }
        }
        if (play_number % print_interval == 0) {
          // we can now calculate running averages for sync error (frames), corrections (ppm),
          // insertions plus deletions (ppm), drift (ppm)
          double moving_average_sync_error = (1.0 * tsum_of_sync_errors) / number_of_statistics;
          double moving_average_correction = (1.0 * tsum_of_corrections) / number_of_statistics;
          double moving_average_insertions_plus_deletions =
              (1.0 * tsum_of_insertions_and_deletions) / number_of_statistics;
          double moving_average_drift = (1.0 * tsum_of_drifts) / number_of_statistics;
          // if ((play_number/print_interval)%20==0)
          if (config.statistics_requested) {
            if (at_least_one_frame_seen) {
            	if ((config.output->delay)) {
                if (config.no_sync==0) {
                  inform("%*.1f,"  /* Sync error inf frames */
                         "%*.1f,"  /* net correction in ppm */
                         "%*.1f,"  /* corrections in ppm */
                         "%*d,"    /* total packets */
                         "%*llu,"  /* missing packets */
                         "%*llu,"  /* late packets */
                         "%*llu,"  /* too late packets */
                         "%*llu,"  /* resend requests */
                         "%*lli,"  /* min DAC queue size */
                         "%*d,"    /* min buffer occupancy */
                         "%*d",    /* max buffer occupancy */
                         10, moving_average_sync_error,
                         10, moving_average_correction * 1000000 / 352,
                         10, moving_average_insertions_plus_deletions * 1000000 / 352,
                         12, play_number,
                         7, missing_packets,
                         7, late_packets,
                         7, too_late_packets,
                         7, resend_requests,
                         7, minimum_dac_queue_size,
                         5, minimum_buffer_occupancy,
                         5, maximum_buffer_occupancy);
                } else {
                  inform("%*.1f,"  /* Sync error inf frames */
                         "%*d,"    /* total packets */
                         "%*llu,"  /* missing packets */
                         "%*llu,"  /* late packets */
                         "%*llu,"  /* too late packets */
                         "%*llu,"  /* resend requests */
                         "%*lli,"  /* min DAC queue size */
                         "%*d,"    /* min buffer occupancy */
                         "%*d",    /* max buffer occupancy */
                         10, moving_average_sync_error,
                         12, play_number,
                         7, missing_packets,
                         7, late_packets,
                         7, too_late_packets,
                         7, resend_requests,
                         7, minimum_dac_queue_size,
                         5, minimum_buffer_occupancy,
                         5, maximum_buffer_occupancy);
                } 
              } else {
                inform("%*.1f,"  /* Sync error inf frames */
                       "%*d,"    /* total packets */
                       "%*llu,"  /* missing packets */
                       "%*llu,"  /* late packets */
                       "%*llu,"  /* too late packets */
                       "%*llu,"  /* resend requests */
                       "%*d,"    /* min buffer occupancy */
                       "%*d",    /* max buffer occupancy */
                       10, moving_average_sync_error,
                       12, play_number,
                       7, missing_packets,
                       7, late_packets,
                       7, too_late_packets,
                       7, resend_requests,
                       5, minimum_buffer_occupancy,
                       5, maximum_buffer_occupancy);
              }
            } else {
              inform("No frames received in the last sampling interval.");
            }
          }
          minimum_dac_queue_size = INT64_MAX;   // hack reset
          maximum_buffer_occupancy = INT32_MIN; // can't be less than this
          minimum_buffer_occupancy = INT32_MAX; // can't be more than this
          at_least_one_frame_seen = 0;
        }
      }
    }
  }

  if (config.statistics_requested) {
     int rawSeconds = (int) difftime( time( NULL ), playstart );
     int elapsedHours = rawSeconds / 3600;
     int elapsedMin = (rawSeconds / 60) % 60;
     int elapsedSec = rawSeconds % 60;
     inform( "Playback Stopped. Total playing time %02d:%02d:%02d\n", elapsedHours, elapsedMin, elapsedSec );
  }

  if (config.output->stop)
  	config.output->stop();
  usleep(100000); // allow this time to (?) allow the alsa subsystem to finish cleaning up after itself. 50 ms seems too short
  free(outbuf);
  free(silence);
  debug(1,"Shut down audio, control and timing threads");
  itr.please_stop = 1;
  pthread_kill(rtp_audio_thread, SIGUSR1);
  pthread_kill(rtp_control_thread, SIGUSR1);
  pthread_kill(rtp_timing_thread, SIGUSR1);
  pthread_join(rtp_timing_thread, NULL);
  debug(1,"timing thread joined");
  pthread_join(rtp_audio_thread, NULL);
  debug(1,"audio thread joined");
  pthread_join(rtp_control_thread, NULL);
  debug(1,"control thread joined");
  debug(1,"Player thread exit");
  return 0;
}

// takes the volume as specified by the airplay protocol
void player_volume(double airplay_volume) {

  // The volume ranges -144.0 (mute) or -30 -- 0. See
  // http://git.zx2c4.com/Airtunes2/about/#setting-volume
  // By examination, the -30 -- 0 range is linear on the slider; i.e. the slider is calibrated in 30
  // equal increments. Since the human ear's response is roughly logarithmic, we imagine these to
  // be power dB, i.e. from -30dB to 0dB.
    
  // We may have a hardware mixer, and if so, we will give it priority.
  // If a desired volume range is given, then we will try to accommodate it from
  // the top of the hardware mixer's range downwards.
  
  // If no desired volume range is given, we will use the native resolution of the hardware mixer, if any,
  // or failing that, the software mixer. The software mixer has a range of from -96.3 dB up to 0 dB,
  // corresponding to a multiplier of 1 to 65535.
 
  // Otherwise, we will accommodate the desired volume range in the combination of the software and hardware mixer
  // Intuitively (!), it seems best to give the hardware mixer as big a role as possible, so
  // we will use its full range and then accommodate the rest of the attenuation in software.
  // A problem is that we don't know whether the lowest hardware volume actually mutes the output
  // so we must assume that it does, and for this reason, the volume control goes at the "bottom" of the adjustment range
    
  // The dB range of a value from 1 to 65536 is about 96.3 dB (log10 of 65536 is 4.8164).
  // Since the levels correspond with amplitude, they correspond to voltage, hence voltage dB,
  // or 20 times the log of the ratio. Then multiplied by 100 for convenience.
  // Thus, we ask our vol2attn function for an appropriate dB between -96.3 and 0 dB and translate
  // it back to a number.
  
  int32_t hw_min_db, hw_max_db, hw_range_db, range_to_use, min_db, max_db; // hw_range_db is a flag; if 0 means no mixer
  
  int32_t sw_min_db = -9630;
  int32_t sw_max_db = 0;
  int32_t sw_range_db = sw_max_db - sw_min_db;
  int32_t desired_range_db; // this is used as a flag; if 0 means no desired range
  
  if (config.volume_range_db)
    desired_range_db = (int32_t)trunc(config.volume_range_db*100);
  else
    desired_range_db = 0;
  
  if (config.output->parameters) {
    // have a hardware mixer
    config.output->parameters(&audio_information);
    hw_max_db = audio_information.maximum_volume_dB;
    hw_min_db = audio_information.minimum_volume_dB;
    hw_range_db = hw_max_db-hw_min_db;
  } else {
    // don't have a hardware mixer
    hw_max_db = hw_min_db = hw_range_db = 0;
  }
  
  if (desired_range_db) {
    // debug(1,"An attenuation range of %d is requested.",desired_range_db);
    // we have a desired volume range.
    if (hw_range_db) {
    // we have a hardware mixer
      if (hw_range_db>=desired_range_db) {
        // the hardware mixer can accommodate the desired range
        max_db = hw_max_db;
        min_db = max_db - desired_range_db;
      } else {
        if ((hw_range_db+sw_range_db)<desired_range_db) {
          inform("The volume attenuation range %f is greater than can be accommodated by the hardware and software -- set to %f.",config.volume_range_db,hw_range_db+sw_range_db);
          desired_range_db=hw_range_db+sw_range_db;
        }
        min_db = hw_min_db;
        max_db = min_db + desired_range_db;
      }
    } else {
      // we have a desired volume range and no hardware mixer
      if (sw_range_db<desired_range_db) {
        inform("The volume attenuation range %f is greater than can be accommodated by the software -- set to %f.",config.volume_range_db,sw_range_db);
        desired_range_db=sw_range_db;      
      }
      max_db = sw_max_db;
      min_db = max_db - desired_range_db;
    }
  } else {
    // we do not have a desired volume range, so use the mixer's volume range, if there is one.
    // debug(1,"No attenuation range requested.");
    if (hw_range_db) {
      min_db = hw_min_db;
      max_db = hw_max_db;
    } else {
      min_db = sw_min_db;
      max_db = sw_max_db;
    }
  }
  
  double hardware_attenuation, software_attenuation;
  double scaled_attenuation = hw_min_db+sw_min_db;
    
  // now, we can map the input to the desired output volume
  if (airplay_volume==-144.0) {    
    // do a mute   
    // needed even with hardware mute, as when sound is unmuted it might otherwise be very loud.
  	hardware_attenuation = hw_min_db;
  	software_attenuation = sw_min_db;
  	if (config.output->mute)
  		config.output->mute(1); // use real mute if it's there
    
  } else {
  	if (config.output->mute)
  		config.output->mute(0); // unmute mute if it's there  
    scaled_attenuation = vol2attn(airplay_volume, max_db, min_db);
    if (hw_range_db) {
      // if there is a hardware mixer
      if (scaled_attenuation<=hw_max_db) {
        // the attenuation is so low that's it's in the hardware mixer's range
        // debug(1,"Attenuation all taken care of by the hardware mixer.");
        hardware_attenuation = scaled_attenuation;
        software_attenuation = sw_max_db - (max_db-hw_max_db); // e.g. if the hw_max_db  is +4 and the max is +40, this will be -36 (all by 100, of course)
      } else {
        // debug(1,"Attenuation taken care of by hardware and software mixer.");
        hardware_attenuation = hw_max_db; // the hardware mixer is turned up full
        software_attenuation = sw_max_db - (max_db-scaled_attenuation);
      }
    } else {
      // if there is no hardware mixer, the scaled_volume is the software volume
      // debug(1,"Attenuation all taken care of by the software mixer.");
      software_attenuation = scaled_attenuation;
    }
  }
  
  if ((config.output->volume) && (hw_range_db)) {
	  config.output->volume(hardware_attenuation); // otherwise set the output to the lowest value
	  //debug(1,"Hardware attenuation set to %f for airplay volume of %f.",hardware_attenuation,airplay_volume);
  }
  double temp_fix_volume = 65536.0 * pow(10, software_attenuation / 2000);
 	// debug(1,"Software attenuation set to %f, i.e %f out of 65,536, for airplay volume of %f",software_attenuation,temp_fix_volume,airplay_volume);
  
  pthread_mutex_lock(&vol_mutex);
  fix_volume = temp_fix_volume;
  pthread_mutex_unlock(&vol_mutex);

#ifdef CONFIG_METADATA
  char *dv = malloc(128); // will be freed in the metadata thread
  if (dv) {
    memset(dv, 0, 128);
    snprintf(dv, 127, "%.2f,%.2f,%.2f,%.2f", airplay_volume,
             scaled_attenuation / 100.0,
             min_db / 100.0,
             max_db / 100.0);
    send_ssnc_metadata('pvol', dv, strlen(dv), 1);
  }
#endif
}

void player_flush(uint32_t timestamp) {
  debug(3,"Flush requested up to %u. It seems as if 0 is special.",timestamp);
  pthread_mutex_lock(&flush_mutex);
  flush_requested = 1;
  // if (timestamp!=0)
  flush_rtp_timestamp = timestamp; // flush all packets up to (and including?) this
  pthread_mutex_unlock(&flush_mutex);
  play_segment_reference_frame = 0;
#ifdef CONFIG_METADATA
  send_ssnc_metadata('pfls', NULL, 0, 1);
#endif
}

int player_play(stream_cfg *stream, pthread_t *player_thread) {

#ifdef CONFIG_LCM
        shairport_lcm_start();
#endif

	//if (*player_thread!=NULL)
	//	die("Trying to create a second player thread for this RTSP session");
  packet_count = 0;
  encrypted = stream->encrypted;
  if (config.buffer_start_fill > BUFFER_FRAMES)
    die("specified buffer starting fill %d > buffer size %d", config.buffer_start_fill,
        BUFFER_FRAMES);
  if (encrypted) {
#ifdef HAVE_LIBPOLARSSL
    memset(&dctx, 0, sizeof(aes_context));
    aes_setkey_dec(&dctx, stream->aeskey, 128);
#endif

#ifdef HAVE_LIBSSL
    AES_set_decrypt_key(stream->aeskey, 128, &aes);
#endif
    aesiv = stream->aesiv;
  }
  init_decoder(stream->fmtp);
  // must be after decoder init
  init_buffer();
  please_stop = 0;
  command_start();
#ifdef CONFIG_METADATA
  send_ssnc_metadata('pbeg', NULL, 0, 1);
#endif

// set the flowcontrol condition variable to wait on a monotonic clock
#ifdef COMPILE_FOR_LINUX_AND_FREEBSD_AND_CYGWIN
  pthread_condattr_t attr;
  pthread_condattr_init(&attr);
  pthread_condattr_setclock(&attr, CLOCK_MONOTONIC); // can't do this in OS X, and don't need it.
  int rc = pthread_cond_init(&flowcontrol, &attr);
#endif
#ifdef COMPILE_FOR_OSX
  int rc = pthread_cond_init(&flowcontrol, NULL);
#endif
  if (rc)
    debug(1, "Error initialising condition variable.");
  config.output->start(sampling_rate);
  size_t size = (PTHREAD_STACK_MIN + 256 * 1024);
  pthread_attr_t tattr;
  pthread_attr_init(&tattr);
  rc = pthread_attr_setstacksize(&tattr, size);
  if (rc)
    debug(1, "Error setting stack size for player_thread: %s", strerror(errno));
  pthread_create(player_thread, &tattr, player_thread_func, NULL);
  pthread_attr_destroy(&tattr);
  return 0;
}

void player_stop(pthread_t *player_thread) {
#ifdef CONFIG_LCM
	shairport_lcm_stop();
#endif
	//if (*thread==NULL)
	//	debug(1,"Trying to stop a non-existent player thread");
	// else {
		please_stop = 1;
		pthread_cond_signal(&flowcontrol); // tell it to give up
		pthread_join(*player_thread, NULL);
	#ifdef CONFIG_METADATA
		send_ssnc_metadata('pend', NULL, 0, 1);
	#endif
		command_stop();
		free_buffer();
		free_decoder();
		int rc = pthread_cond_destroy(&flowcontrol);
		if (rc)
			debug(1, "Error destroying condition variable.");
	//	}
}
