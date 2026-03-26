#ifndef __RTP_COMMON_H__
#define __RTP_COMMON_H__

#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>

#define RTP_MARKER_BIT (1 << 7)

struct rtcp_timestamp
{
  uint32_t pos;
  struct timespec ts;
};

struct ntp_timestamp
{
  uint32_t sec;
  uint32_t frac;
};

enum rtp_type
{
  RTP_REALTIME = 96,
  RTP_BUFFERRED = 103,
  RTP_REMOTE_CONTROL_ONLY = 130,
};

// thanks to Mike Brady from shairport-sync
// these are the values coming in on a buffered audio RTP packet's SSRC field
// the apparent significances are as indicated.
// Dolby Atmos seems to be 7P1
enum buffered_ssrc {
  SSRC_NONE = 0,
  ALAC_44100_S16_2 = 0x0000FACE, // this is made up
  ALAC_48000_S24_2 = 0x15000000,
  AAC_44100_F24_2 = 0x16000000,
  AAC_48000_F24_2 = 0x17000000,
  AAC_48000_F24_5P1 = 0x27000000,
  AAC_48000_F24_7P1 = 0x28000000,
};

struct rtp_packet
{
  uint16_t seqnum;     // Sequence number
  int samples;         // Number of samples in the packet

  uint8_t *header;     // Pointer to the RTP header
  size_t header_len;   // Length of RTP header (12 bytes)

  uint8_t *payload;    // Pointer to the RTP payload
  size_t payload_size; // Size of allocated memory for RTP payload
  size_t payload_len;  // Length of payload (must of course not exceed size)

  uint8_t *data;       // Pointer to the complete packet data
  size_t data_size;    // Size of packet data
  size_t data_len;     // Length of actual packet data
};

struct rtcp_packet
{
  uint8_t version; // Always 2
  bool padding;
  uint16_t len;
  uint32_t ssrc;

  uint8_t *payload;
  size_t payload_len;

  enum rtcp_packet_type
    {
      RTCP_PACKET_RR = 201, // RFC 3550
      RTCP_PACKET_APP = 204, // RFC 1889
      RTCP_PACKET_PSFB = 206, // RFC 4585
      RTCP_PACKET_XR = 207, // RFC 3611
    } packet_type;

  union
    {
      struct rtcp_packet_rr
	{
	  uint8_t report_count;
	} rr;
      struct rtcp_packet_app
	{
	  uint8_t subtype;
	  char name[5]; // Zero-terminated
	} app;
      struct rtcp_packet_psfb
	{
	  uint8_t message_type;
	  uint32_t media_src;
	  uint8_t *fci;
	  size_t fci_len;
	} psfb;
      struct rtcp_packet_xr
	{
	  uint8_t block_type;
	  uint8_t block_specific;
	  uint16_t block_len;
	  struct ntp_timestamp ntp;
	} xr;
    };
};

// An RTP session is characterised by all the receivers belonging to the session
// getting the same RTP and RTCP packets. So if you have clients that require
// different sample rates or where only some can accept encrypted payloads then
// you need multiple sessions.
struct rtp_session
{
  uint32_t ssrc_id;
  uint32_t pos;
  uint16_t seqnum;

  struct media_quality quality;

  // Packet buffer (ring buffer), used for retransmission
  struct rtp_packet *pktbuf;
  size_t pktbuf_next;
  size_t pktbuf_size;
  size_t pktbuf_len;

  // Number of samples to elapse before sync'ing. If 0 we set it to the s/r, so
  // we sync once a second. If negative we won't sync.
  int sync_each_nsamples;
  int sync_counter;
  struct rtp_packet sync_packet_next;

  // In addition to being the clock ID, non-zero means this is a PTP session
  uint64_t ptp_clock_id;

  enum rtp_type type;
};


struct rtp_session *
rtp_session_new(struct media_quality *quality, int pktbuf_size, int sync_each_nsamples, uint64_t ptp_clock_id);

void
rtp_session_free(struct rtp_session *session);

void
rtp_session_flush(struct rtp_session *session);


/* Gets the next packet from the packet buffer, pkt->payload will be allocated
 * to a size of payload_len (or larger).
 *
 * @param[in]  session       RTP session
 * @param[in]  payload_len   Length of payload the packet needs to hold
 * @param[in]  samples       Number of samples in packet
 * @return            Pointer to the next packet in the packet buffer
 */
struct rtp_packet *
rtp_packet_next(struct rtp_session *session, size_t payload_len, int samples);

/* Call this after finalizing a packet, i.e. writing the payload and possibly
 * sending. Registers the packet as final, i.e. it can now be retrieved with
 * rtp_packet_get() for retransmission, if required. Also advances RTP position
 * (seqnum and RTP time).
 *
 * @in  session       RTP session
 * @in  pkt           RTP packet to commit
 */
void
rtp_packet_commit(struct rtp_session *session, struct rtp_packet *pkt);

/* Get a previously committed packet from the packet buffer
 *
 * @in  session       RTP session
 * @in  seqnum        Packet sequence number
 */
struct rtp_packet *
rtp_packet_get(struct rtp_session *session, uint16_t seqnum);

bool
rtp_sync_is_time(struct rtp_session *session);

struct rtp_packet *
rtp_sync_packet_next(struct rtp_session *session, struct rtcp_timestamp cur_stamp, char type);

int
rtcp_packet_parse(struct rtcp_packet *pkt, uint8_t *data, size_t size);

#endif  /* !__RTP_COMMON_H__ */
