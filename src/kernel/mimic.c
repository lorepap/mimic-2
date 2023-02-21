#include <asm-generic/errno.h>
#include <asm-generic/errno-base.h>
#include "protocol/cubic.h"
#include "protocol/hybla.h"
#include "protocol/bbr.h"
#include "protocol/mimic.h"


#define NETLINK_USER 25

// Netlink comm variables
struct sock *nl_sk = NULL;

static u32 selectedProtocolId = 0;

/*
protocols:
cubic 0
bbr 1
hybla 2
*/

/*
 
 
*/

// Netlink comm APIs
static void nl_recv_msg(struct sk_buff *skb)
{
    struct nlmsghdr *nlh;
	long result;
	int res;
	char *msg;
    nlh = (struct nlmsghdr*)skb->data;

	msg = (char*)nlmsg_data(nlh);
	res = kstrtol(msg, 10, &result);
	if (res!=0)
		printk(KERN_ALERT "Error while converting");
	printk(KERN_INFO "Netlink received msg payload: %ld\n", result);
	
	// Change protocol
	selectedProtocolId = (int)result;

	// // Testing new congestion_ops registration
	// tcp_unregister_congestion_control(&tcp_mimic);
	// tcp_register_congestion_control(&tcp_mimic_test);
	// printk(KERN_INFO "New registration done successfully");

}

static int __init netlink_init(void)
{
    struct netlink_kernel_cfg cfg = {
        .input = nl_recv_msg,
    };

    nl_sk = netlink_kernel_create(&init_net, NETLINK_USER, &cfg);
    if (!nl_sk) {
        printk(KERN_ALERT "Error creating netlink socket.\n");
        return -10;
    }

    printk(KERN_INFO "Netlink socket created successfully.\n");
    return 0;
}

static void __exit netlink_exit(void)
{
    netlink_kernel_release(nl_sk);
    printk(KERN_INFO "Netlink socket released.\n");
}

//////////////////////////////////////////////////////////


static void onInit(struct sock *sk)
{
    bictcp_init(sk);
    // tcp_vegas_init(sk);
	bbr_init(sk);
	hybla_init(sk);
}

static void onPacketsAcked(struct sock *sk, const struct ack_sample *sample)
{
	// NOTE: printk increases overhead (tested with iperf3)
	// printk(KERN_INFO "acked - protocol selected: %d\n", selectedProtocolId);
	switch (selectedProtocolId) 
	{
		case 0: bictcp_acked(sk, sample);
		// case 1: tcp_vegas_pkts_acked(sk, sample);
		case 1: return;
		case 2: return;
		default: bictcp_acked(sk, sample);
	}
}

static u32 onUndoCwnd(struct sock *sk){
	switch (selectedProtocolId)
	{
		case 0: return tcp_reno_undo_cwnd(sk);
		// case 1: return tcp_reno_undo_cwnd(sk);
		case 1: return bbr_undo_cwnd(sk);
		case 2: return tcp_reno_undo_cwnd(sk);
		default: return tcp_reno_undo_cwnd(sk);
	}
}

static u32 onSshthresh(struct sock *sk){
	switch (selectedProtocolId)
	{
    	case 0: return bictcp_recalc_ssthresh(sk);
		// case 1: return tcp_reno_ssthresh(sk);
		case 1: return bbr_ssthresh(sk);
		case 2: return tcp_reno_ssthresh(sk);
		default: return bictcp_recalc_ssthresh(sk);
	}
}

static void onAvoidCongestion(struct sock *sk, u32 ack, u32 acked){
	
	switch (selectedProtocolId)
	{
    	case 0: bictcp_cong_avoid(sk, ack, acked);
		// case 1: tcp_vegas_cong_avoid(sk, ack, acked);
		case 1: return;
		case 2: hybla_cong_avoid(sk, ack, acked);
		default: bictcp_cong_avoid(sk, ack, acked);
	}
}

static void onCongestionEvent(struct sock *sk, enum tcp_ca_event event)
{
	switch (selectedProtocolId)
	{
     	case 0: bictcp_cwnd_event(sk, event);
		// case 1: tcp_vegas_cwnd_event(sk, event);
		case 1: bbr_cwnd_event(sk, event);
		case 2: return;
		default: bictcp_cwnd_event(sk, event);
	}
}

static void onSetState (struct sock *sk, __u8 new_state)
{
	switch (selectedProtocolId)
	{
		case 0:	bictcp_state(sk, new_state);
		case 1: bbr_set_state(sk, new_state);
		case 2: hybla_state(sk, new_state);
		default: bictcp_state(sk, new_state);
	}
}

/* Mimic cong control */
static void onMimicCongControl(struct sock *sk, const struct rate_sample *rs, u32 ack, u32 acked, int flag)
{

	/* bbr support */
	if (selectedProtocolId == 1)
	{
		// printk(KERN_INFO "Running bbr %s", __FUNCTION__);
		bbr_main(sk, rs);
		return;
	}
	
	// printk(KERN_INFO "Running cubic %s", __FUNCTION__);
	/* tcp_cong_avoid */
	if (tcp_in_cwnd_reduction(sk)) {
		/* Reduce cwnd if state mandates */
		tcp_cwnd_reduction(sk, acked, flag);
	} else if (tcp_may_raise_cwnd(sk, flag)) {
		/* Advance cwnd if state allows */
		tcp_cong_avoid(sk, ack, acked);
	}
	tcp_update_pacing_rate(sk);
}

static void tcp_cong_avoid(struct sock *sk, u32 ack, u32 acked)
{
	const struct inet_connection_sock *icsk = inet_csk(sk);

	icsk->icsk_ca_ops->cong_avoid(sk, ack, acked);
	tcp_sk(sk)->snd_cwnd_stamp = tcp_jiffies32;
}

/* Decide wheather to run the increase function of congestion control. */
static inline bool tcp_may_raise_cwnd(const struct sock *sk, const int flag)
{
	/* If reordering is high then always grow cwnd whenever data is
	 * delivered regardless of its ordering. Otherwise stay conservative
	 * and only grow cwnd on in-order delivery (RFC5681). A stretched ACK w/
	 * new SACK or ECE mark may first advance cwnd here and later reduce
	 * cwnd in tcp_fastretrans_alert() based on more states.
	 */
	if (tcp_sk(sk)->reordering >
	    READ_ONCE(sock_net(sk)->ipv4.sysctl_tcp_reordering))
		return flag & FLAG_FORWARD_PROGRESS;

	return flag & FLAG_DATA_ACKED;
}

static void tcp_update_pacing_rate(struct sock *sk)
{
	const struct tcp_sock *tp = tcp_sk(sk);
	u64 rate;

	/* set sk_pacing_rate to 200 % of current rate (mss * cwnd / srtt) */
	rate = (u64)tp->mss_cache * ((USEC_PER_SEC / 100) << 3);

	/* current rate is (cwnd * mss) / srtt
	 * In Slow Start [1], set sk_pacing_rate to 200 % the current rate.
	 * In Congestion Avoidance phase, set it to 120 % the current rate.
	 *
	 * [1] : Normal Slow Start condition is (tp->snd_cwnd < tp->snd_ssthresh)
	 *	 If snd_cwnd >= (tp->snd_ssthresh / 2), we are approaching
	 *	 end of slow start and should slow down.
	 */
	if (tp->snd_cwnd < tp->snd_ssthresh / 2)
		rate *= sock_net(sk)->ipv4.sysctl_tcp_pacing_ss_ratio;
	else
		rate *= sock_net(sk)->ipv4.sysctl_tcp_pacing_ca_ratio;

	rate *= max(tp->snd_cwnd, tp->packets_out);

	if (likely(tp->srtt_us))
		do_div(rate, tp->srtt_us);

	/* WRITE_ONCE() is needed because sch_fq fetches sk_pacing_rate
	 * without any lock. We want to make sure compiler wont store
	 * intermediate values in this location.
	 */
	WRITE_ONCE(sk->sk_pacing_rate, min_t(u64, rate,
					     sk->sk_max_pacing_rate));
}

void tcp_cwnd_reduction(struct sock *sk, int newly_acked_sacked, int flag)
{
	struct tcp_sock *tp = tcp_sk(sk);
	int sndcnt = 0;
	int delta = tp->snd_ssthresh - tcp_packets_in_flight(tp);

	if (newly_acked_sacked <= 0 || WARN_ON_ONCE(!tp->prior_cwnd))
		return;

	tp->prr_delivered += newly_acked_sacked;
	if (delta < 0) {
		u64 dividend = (u64)tp->snd_ssthresh * tp->prr_delivered +
			       tp->prior_cwnd - 1;
		sndcnt = div_u64(dividend, tp->prior_cwnd) - tp->prr_out;
	} else if ((flag & (FLAG_RETRANS_DATA_ACKED | FLAG_LOST_RETRANS)) ==
		   FLAG_RETRANS_DATA_ACKED) {
		sndcnt = min_t(int, delta,
			       max_t(int, tp->prr_delivered - tp->prr_out,
				     newly_acked_sacked) + 1);
	} else {
		sndcnt = min(delta, newly_acked_sacked);
	}
	/* Force a fast retransmit upon entering fast recovery */
	sndcnt = max(sndcnt, (tp->prr_out ? 0 : 1));
	tp->snd_cwnd = tcp_packets_in_flight(tp) + sndcnt;
}

// check this
static u32 onSndbuf_expand(struct sock *sk)
{
	switch (selectedProtocolId)
	{
		case 0:	return 1;
		case 1:	return bbr_sndbuf_expand(sk);
		case 2:	return 1;
		default: return 1;
	}
}

static u32 onMintso_segs(struct sock *sk)
{
	switch (selectedProtocolId)
	{
		case 0:	return 1;
		case 1: return bbr_min_tso_segs(sk);
		case 2:	return 1;
		default: return 1;
	}
}

static struct tcp_congestion_ops tcp_mimic __read_mostly = {
    .init = onInit,
    .pkts_acked = onPacketsAcked,
    .undo_cwnd = onUndoCwnd,
    .ssthresh = onSshthresh,
	.mimic_cong_control = onMimicCongControl,
    .cong_avoid = onAvoidCongestion,
    .cwnd_event = onCongestionEvent,
	.set_state	= onSetState,
	// .cong_control = onCongControl,
	// .sndbuf_expand	= onSndbuf_expand,
	// .min_tso_segs	= onMintso_segs,
    .owner = THIS_MODULE,
    .name = "mimic"
};

static int __init onRegister(void)
{

	netlink_init();

    BUILD_BUG_ON(sizeof(struct arm) > ICSK_CA_PRIV_SIZE);

	/* Precompute a bunch of the scaling factors that are used per-packet
	 * based on SRTT of 100ms
	 */

	beta_scale = 8*(BICTCP_BETA_SCALE+beta) / 3
		/ (BICTCP_BETA_SCALE - beta);

	cube_rtt_scale = (bic_scale * 10);	/* 1024*c/rtt */

	/* calculate the "K" for (wmax-cwnd) = c/rtt * K^3
	 *  so K = cubic_root( (wmax-cwnd)*rtt/c )
	 * the unit of K is bictcp_HZ=2^10, not HZ
	 *
	 *  c = bic_scale >> 10
	 *  rtt = 100ms
	 *
	 * the following code has been designed and tested for
	 * cwnd < 1 million packets
	 * RTT < 100 seconds
	 * HZ < 1,000,00  (corresponding to 10 nano-second)
	 */

	/* 1/c * 2^2*bictcp_HZ * srtt */
	cube_factor = 1ull << (10+3*BICTCP_HZ); /* 2^40 */

	/* divide by bic_scale and by constant Srtt (100ms) */
	do_div(cube_factor, bic_scale * 10);

    return tcp_register_congestion_control(&tcp_mimic);
}

static void __exit onUnRegister(void)
{
	netlink_exit();
    tcp_unregister_congestion_control(&tcp_mimic);
}

module_init(onRegister);
module_exit(onUnRegister);

MODULE_AUTHOR("Lorenzo Pappone");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Mimic");
MODULE_VERSION("1.0");