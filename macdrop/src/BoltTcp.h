#ifndef BOLTTCP_H
#define BOLTTCP_H

#include <stdint.h>

#ifdef __m68k__
#include <MacTypes.h>
#include <MacTCP.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Extended Device Manager csCodes on ".IPP" */
#define kBoltGetCapabilities  100
#define kBoltPoll             101
#define kBoltRecvZeroCopy     102
#define kBoltSendZeroCopy     103
#define kBoltGetDebugStats    104

#define BOLT_MAGIC            0x424F4C54UL  /* 'BOLT' */
#define BOLT_VERSION          0x0031        /* 0.1.33 Alpha */
#define BOLT_BUILD_TAG        "0.1.33 Alpha"

#define BOLT_TIER_SONIC       1
#define BOLT_TIER_ENET        2
#define BOLT_TIER_DP8390      3

/* Socket / stream flags for SetSocketFlags & SendZeroCopy */
#define BOLT_FLAG_SKIP_RX_CSUM 0x01
#define BOLT_MODE_TICKLESS     0x02
#define BOLT_FLAG_NODELAY      0x04

/* Stream states returned by BoltFastAPI.GetStreamState() (matching MacTCP connectionState) */
#define BOLT_TCP_STATE_CLOSED      0
#define BOLT_TCP_STATE_LISTEN      2
#define BOLT_TCP_STATE_SYN_RCVD    4
#define BOLT_TCP_STATE_SYN_SENT    6
#define BOLT_TCP_STATE_ESTABLISHED 8
#define BOLT_TCP_STATE_FIN_WAIT1   10
#define BOLT_TCP_STATE_FIN_WAIT2   12
#define BOLT_TCP_STATE_CLOSE_WAIT  14
#define BOLT_TCP_STATE_CLOSING     16
#define BOLT_TCP_STATE_LAST_ACK    18
#define BOLT_TCP_STATE_TIME_WAIT   20

#ifndef __MACTYPES__
typedef int16_t OSErr;
typedef unsigned long StreamPtr;
#endif

/* Returned by PBControl(ippRefNum, csCode = 100) */
typedef struct BoltFastAPI {
    uint32_t magic;              /* 'BOLT' (0x424F4C54) */
    uint16_t version;            /* BOLT_VERSION (e.g. 0x0031 for 0.1.33 Alpha) */
    uint16_t link_tier;          /* 1 = SONIC DMA, 2 = .ENET Fallback, 3 = DP8390 Direct SRAM */

    /* Direct C-calling-convention function pointers (no PBControl trap overhead) */
    void     (*Poll)(void);
    uint16_t (*RecvZeroCopy)(StreamPtr stream, const uint8_t **out_dma_ptr);
    void     (*RecvRelease)(StreamPtr stream, uint16_t bytes_consumed);
    OSErr    (*SendZeroCopy)(StreamPtr stream, const uint8_t *buf, uint16_t len, uint8_t flags);
    void     (*SetSocketFlags)(StreamPtr stream, uint8_t flags);
    uint8_t  (*GetStreamState)(StreamPtr stream);
} BoltFastAPI;

/* Parameter block overlay for csCode = 100..104 */
typedef struct BoltControlPB {
    void     *qLink;
    short     qType;
    short     ioTrap;
    void     *ioCmdAddr;
    void     *ioCompletion;
    short     ioResult;
    void     *ioNamePtr;
    short     ioVRefNum;
    short     ioCRefNum;
    short     csCode;
    union {
        struct {
            BoltFastAPI *apiOut;
        } caps;
        struct {
            uint32_t  stream;
            const uint8_t *outPtr;
            uint16_t  outLen;
            uint16_t  consumedLen;
        } rx;
        struct {
            uint32_t  stream;
            const uint8_t *buf;
            uint16_t  len;
            uint8_t   flags;
            uint8_t   reserved;
        } tx;
        struct {
            void     *statsOut;
            uint16_t  statsSize;
        } dbg;
    } p;
} BoltControlPB;

typedef struct BoltDebugStats {
    uint8_t  active_tier;        /* 1 = SONIC, 2 = .ENET, 3 = DP8390 */
    uint8_t  sonic_slot;         /* 0x09..0x0E, or 0x00 if fallback */
    uint16_t sonic_rev;          /* DP83932 Silicon Revision register (SR) value */
    uint32_t phys_rx_ring_addr;  /* Physical RAM address from GetPhysical() */
    uint32_t rx_packets_total;
    uint32_t rx_fastpath_hits;   /* Bypassed C TCP state machine */
    uint32_t rx_slowpath_hits;   /* Out-of-order, SYN/FIN, or TCP options */
    uint32_t tx_zerocopy_pkts;   /* Sent via multi-fragment TDA / WDS gather */
    uint32_t irq_count;          /* Total hardware interrupts fired */
    uint32_t poll_calls;         /* Total Poll() invocations */
    uint16_t rx_ring_high_water; /* Max simultaneous occupied DMA slots (of 32) */
    uint16_t rx_ring_overflows;  /* SONIC ISR RBE (Receive Buffers Exhausted) events */
    uint32_t tx_ok_cnt;
    uint32_t tx_err_cnt;
    int16_t  last_tx_err;
    uint8_t  last_rx_drop;
    uint8_t  last_b14;
    uint8_t  last_b15;
    uint16_t rx_arp_cnt;
    uint16_t rx_ip_cnt;
    uint16_t last_rx_etype;
    uint16_t last_rx_len;
    uint16_t last_cs_code;
    uint16_t err_cs_code;
    int16_t  err_cs_val;
    uint16_t tcp_create_cnt;
    uint16_t tcp_open_cnt;
    uint16_t tcp_send_cnt;
    uint32_t dbg_tcp_ooo;        /* SlowPath: out-of-order (seq > rcv_nxt) */
    uint32_t dbg_tcp_dup;        /* SlowPath: duplicate/old (seq < rcv_nxt) */
    uint32_t dbg_tcp_bad_csum;   /* Dropped due to bad TCP checksum */
    uint32_t dbg_tcp_no_match;   /* Unmatched segments -> SendBareRst */
    uint32_t dbg_sram_retry;     /* DP8390 SRAM re-reads triggered on bad TCP csum */
    uint32_t dbg_sram_healed;    /* Bad TCP checksums recovered by slow SRAM re-read */
} BoltDebugStats;

/* Configuration persisted in Preferences:BoltTcp Prefs */
#define BOLT_PREFS_MAGIC  0x42434647UL /* 'BCFG' */
#define BOLT_PREFS_VER    1

typedef struct BoltPrefs {
    uint32_t magic;
    uint16_t version;
    uint8_t  use_dhcp;           /* 1 = DHCP, 0 = Static IP */
    uint8_t  enable_coalesce;    /* 1 = SONIC WT1:WT0 IRQ coalescing */
    uint32_t static_ip;          /* Host-order IPv4 (same as big-endian on 68k) */
    uint32_t static_mask;
    uint32_t static_gw;
    uint32_t static_dns;
    uint16_t rx_window;          /* Advertised TCP RX window (default 32768) */
    uint8_t  prefer_enet;        /* 1 = Force Tier 2 .ENET even if SONIC present */
    uint8_t  reserved;
} BoltPrefs;

/* Extended csCode to read/reload live status & prefs from Control Panel */
#define kBoltGetConfig        105
#define kBoltSetConfig        106
#define kBoltGetDebugLog      107

enum {
    BOLT_LOG_TICK_1S   = 1, /* 1-second active delta snapshot */
    BOLT_LOG_TCP_OPEN  = 2, /* TCP active/passive open (ev_sub=stream, ev_val=remote_port) */
    BOLT_LOG_TCP_CLOSE = 3, /* TCP close/abort/release (ev_sub=stream, ev_val=state) */
    BOLT_LOG_DP_RESET  = 4, /* DP8390 reset / OVW (ev_sub=reason, ev_val=info) */
    BOLT_LOG_WND_FULL  = 5, /* RX ring full / zero-window ACK (ev_sub=stream, ev_val=rx_count) */
    BOLT_LOG_SRAM_FIX  = 6, /* DP8390 SRAM csum retry (ev_sub=1 healed / 0 fail, ev_val=ethLen) */
    BOLT_LOG_BAD_CSUM  = 7, /* TCP csum fail (ev_sub=cur_rx_page, ev_val=tcp_len) */
    BOLT_LOG_TX_ERR    = 8  /* Link TX error (ev_sub=phase/slot, ev_val=OSErr) */
};

#define BOLT_LOG_CAP          256

typedef struct BoltLogEntry {
    uint32_t ticks;
    uint16_t rx_delta;
    uint16_t tx_delta;
    uint16_t ooo_delta;
    uint16_t cs_delta;
    uint16_t dup_delta;
    uint16_t rst_delta;
    uint8_t  ev_type;
    uint8_t  ev_sub;
    uint16_t ev_val;
} BoltLogEntry;

typedef struct BoltDebugLogSnapshot {
    uint32_t     total_logged;
    uint32_t     now_ticks;
    uint16_t     count;
    uint16_t     head;
    BoltLogEntry entries[BOLT_LOG_CAP];
} BoltDebugLogSnapshot;

typedef struct BoltLiveConfig {
    BoltPrefs prefs;
    uint32_t  active_ip;
    uint32_t  active_mask;
    uint32_t  active_gw;
    uint32_t  active_dns;
    uint8_t   active_tier;
    uint8_t   sonic_slot;
    uint16_t  sonic_rev;
    uint8_t   mac_addr[6];
    uint8_t   dhcp_state;        /* 0=disabled, 1=discovering, 2=bound, 3=fallback */
    uint8_t   debug_build;       /* 1 if compiled with BOLT_DEBUG=1 */
    uint32_t  phys_rx_ring;
    int16_t   last_link_err;
    uint32_t  boot_diag;
} BoltLiveConfig;

void  BoltPrefsInitDefaults(BoltPrefs *p);
OSErr BoltPrefsLoad(BoltPrefs *p);
OSErr BoltPrefsSave(const BoltPrefs *p);

#ifdef __cplusplus
}
#endif

#endif /* BOLTTCP_H */
