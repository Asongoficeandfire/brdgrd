/* Copyright (C) 2012 Philipp Winter (philipp.winter@kau.se)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 * =======================================================================
 *
 * This program requires iptables rules feeding it with data. See the
 * README for details.
 */
#include <stdio.h>
#include <time.h>
#include <sys/time.h>
#include <glib.h>
#include <stdlib.h>
#include <libnet.h>
#include <string.h>
#include <getopt.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <linux/netfilter.h>
#include <libnetfilter_queue/libnetfilter_queue.h>

/* used for connection logging */
#define ACCEPTED	1
#define REJECTED	0

/* 32bit IP integer to dotted decimal notation suitable for printf() */
#define IP2DEC(k)	((k)->src_ip & 0xff000000U) >> 24, \
					((k)->src_ip & 0x00ff0000U) >> 16, \
					((k)->src_ip & 0x0000ff00U) >> 8, \
					((k)->src_ip & 0x000000ffU)

/* used to print verbose messages to stdout */
#define VRB(...) \
	if (verbose) { \
		printf("[V] "); \
		print(__VA_ARGS__); \
	}

/* used to print debug messages to stdout */
#define DBG(...) \
	if (debug) { \
		printf("[D] "); \
		print(__VA_ARGS__); \
	}

/* the hash table and global config variables */
static GHashTable *host_table;
static int debug = 0;
static int verbose = 0;
static unsigned int retrans_limit = 2;
static unsigned int timeout = 40;
static unsigned int cleanup_threshold = 5;
static int queue_number = 1;
static char *logfile = NULL;

typedef struct timewin {
	uint16_t begin;
	uint16_t end;
} timewin_t;

/* during this time window (in seconds) after every 15-min-interval, we ignore
 * incoming connections and act deaf because chinese probes usually connect
 * during this interval.
 */
timewin_t deaf_window = { 0, 200 };

/* the key for the hash table */
typedef struct hash_key {
	uint32_t src_ip;
	uint16_t src_port;
	uint16_t dummy; /* pad struct to 64 bit */
} hash_key_t;

/* the value for the hash table */
typedef struct hash_value {
	time_t timestamp;
	uint32_t counter;
} hash_val_t;


/* Wrapped by the VRB() and DBG() macros.
 */
static void print( const char *fmt, ... ) {

	va_list ap;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}


/* Log the given hash table entry to the logfile.
 */
void log_connection( hash_key_t *key, hash_val_t *conn, gboolean accepted ) {

	static FILE *fp = NULL;
	static char log_record[100] = { 0 };

	/* return if user does not want logs */
	if (logfile == NULL) {
		return;
	}

	if (fp == NULL) {
		DBG("Opening logfile \"%s\".\n", logfile);
		if ((fp = fopen(logfile, "a")) == NULL) {
			perror("Error: fopen() failed");
			return;
		}
	}

	/* log host to file */
	snprintf(log_record, sizeof(log_record), "%s\t%u.%u.%u.%u:%u\t->\t%lu\t%u\n", \
		accepted ? "ACCEPT" : "DROP", IP2DEC(key), key->src_port, \
		conn->timestamp, conn->counter);
	fwrite(log_record, strnlen(log_record, sizeof(log_record)), 1, fp);
	fflush(fp);
}


/* Generic function to free data from the hash table.
 */
void data_destroy_func( gpointer data ) {

	if (data != NULL) {
		free(data);
	}
}


/* Compares the two given hash keys for equality.
 */
static inline gboolean key_equal( gconstpointer a, gconstpointer b ) {

	return ((((hash_key_t *) a)->src_ip == ((hash_key_t *) b)->src_ip) &&
		(((hash_key_t *) a)->src_port == ((hash_key_t *) b)->src_port));
}


/* This function signalizes to the caller that a hash table entry should be
 * removed if it timed out. It's purpose is to provide a primitive garbage
 * collector.
 */
static gboolean garbage_collect( gpointer key, gpointer value,
		gpointer now) {

	hash_key_t *k = key;
	hash_val_t *conn = value;

	if ((*((time_t *) now) - conn->timestamp) > timeout) {
		log_connection(k, conn, REJECTED);
		return TRUE; /* delete entry */
	} else {
		return FALSE; /* keep entry */
	}
}


/* Prints the given hash table entry to stdout.
 */
static inline void print_entry( gpointer key, gpointer value, gpointer user_data ) {

	hash_key_t *k = key;
	hash_val_t *conn = value;
	struct tm *tmp = NULL;
	char timestr[50] = { 0 };

	if ((tmp = localtime(&(conn->timestamp))) == NULL) {
		DBG("localtime() failed.\n");
		return;
	}

	strftime(timestr, sizeof(timestr), "%F %T", tmp);

	printf("\t(%u.%u.%u.%u:%u) -> (%s / %u)\n", IP2DEC(k), k->src_port,
		timestr, conn->counter);
}


/* Prints a human readable timestamp with millisecond granularity to stdout.
 */
static inline void print_time( void ) {

	struct timeval tv = { 0, 0 };
	struct tm *tmp = NULL;
	time_t now = time(NULL);
	char timestr[50] = { 0 };

	if (gettimeofday(&tv, NULL) == -1) {
		perror("gettimeofday() failed");
		return;
	}

	if ((tmp = localtime(&now)) == NULL) {
		DBG("localtime() failed.\n");
		return;
	}

	strftime(timestr, sizeof(timestr), "%T", tmp);

	VRB("Time: %s.%03ld\n", timestr, tv.tv_usec/1000);
}


/* Quick check if we are dealing with a TCP SYN segment. If not,
 * 0 is returned.
 */
static inline int tcp_syn_segment( struct iphdr *iphdr,
	struct tcphdr *tcphdr ) {

	/* check for IP protocol field */
	if (iphdr->protocol != 6) {
		return 0;
	}

	/* check for set bits in TCP hdr */
	if (tcphdr->urg == 0 &&
		tcphdr->ack == 0 &&
		tcphdr->psh == 0 &&
		tcphdr->rst == 0 &&
		tcphdr->syn == 1 &&
		tcphdr->fin == 0) {
		return 1;
	}

	return 0;
}


/* Callback function which is called for every incoming packet.
 * It issues a verdict for every packet which is either ACCEPT
 * or DROP.
 * This function deals with untrusted network data.
 */
int callback( struct nfq_q_handle *qh, struct nfgenmsg *nfmsg, struct nfq_data *nfa,
	void *data ) {

	struct iphdr *iphdr = NULL;
	struct tcphdr *tcphdr = NULL;
	hash_key_t *key = NULL;
	hash_val_t *conn = NULL;
	struct nfqnl_msg_packet_hdr *ph = NULL;
	unsigned char *packet= NULL;
	time_t now = 0;
	int id = 0;

	ph = nfq_get_msg_packet_hdr(nfa);
	if (ph) {
		id = ntohl(ph->packet_id);
	} else {
		VRB("Dropping packet because nfq_get_msg_packet_hdr() failed.\n");
		return nfq_set_verdict(qh, id, NF_DROP, 0, NULL);
	}

	/* try to get packet (header + payload) */
	if (nfq_get_payload(nfa, (char **) &packet) == -1) {
		VRB("Dropping packet because nfq_get_payload() failed.\n");
		return nfq_set_verdict(qh, id, NF_DROP, 0, NULL);
	}

	/* initialize pointers to IP and TCP headers */
	iphdr = (struct iphdr *) packet;
	/* RFC 791 defines that the IHL's minimum value is 5 */
	if ((iphdr->ihl < 5) || (iphdr->ihl > 15)) {
		VRB("Dropping packet because the IHL in the IP header is invalid.\n");
		return nfq_set_verdict(qh, id, NF_DROP, 0, NULL);
	}
	tcphdr = (struct tcphdr *) (packet + (iphdr->ihl * 4));

	/* check if we are dealing with a TCP SYN segment */
	if (!tcp_syn_segment(iphdr, tcphdr)) {
		fprintf(stderr, "We received something which is no TCP SYN segment. " \
			"Are your iptables rules correct?\n");
		return nfq_set_verdict(qh, id, NF_DROP, 0, NULL);
	}

	/* 1st step: build key for hashtable: src ip || src port */
	if ((key = malloc(sizeof(hash_key_t))) == NULL) {
		VRB("Exiting because malloc() returned NULL.\n");
		exit(1);
	}
	key->src_ip = ntohl(iphdr->saddr);
	key->src_port = ntohs(tcphdr->source);
	key->dummy = 0;

	/* 2nd step: search hash table for key */
	conn = g_hash_table_lookup(host_table, key);

	printf("\n");
	print_time();
	VRB("Incoming SYN segment from %u.%u.%u.%u:%u.\n", IP2DEC(key), key->src_port);

	/* are we supposed to act deaf right now? */
	now = time(NULL);
	if (((now % (60 * 15)) >= deaf_window.begin) &&
		((now % (60 * 15)) <= deaf_window.end)) {
		VRB("DROP - Because we have to act deaf (pssst, we suspect a chinese probe!).\n");
		return nfq_set_verdict(qh, id, NF_DROP, 0, NULL);
	}

	/* the IP 202.108.181.70 was observed to be some sort of chinese 'master'
	 * probe. it seems to be the only IP which shows up regularly for scanning.
	 * we might as well blacklist it here.
	 */
	if (key->src_ip == 3396121926U) {
		VRB("DROP - It's the master probe 202.108.181.70\n");
		return nfq_set_verdict(qh, id, NF_DROP, 0, NULL);
	}

	/* 3rd step: check if host should be allowed to connect */
	if (conn == NULL) {
		/* we haven't seen this host yet - add new entry to hash table */
		DBG("Adding previously unseen " \
			"host (%u.%u.%u.%u:%u) to hash table.\n", IP2DEC(key), key->src_port);
		hash_val_t *new_conn = malloc(sizeof(hash_val_t));
		new_conn->timestamp = time(NULL);
		new_conn->counter = 0; /* no retransmissions yet */

		g_hash_table_insert(host_table, key, new_conn);
	} else {
		DBG("Found host (%u.%u.%u.%u:%u) in hash table.\n",
			IP2DEC(key), key->src_port);
		conn->counter++;

		/* we only accept the packet if it was retransmitted often enough within
		   the allowed time span. */
		if (conn->counter >= retrans_limit) {
			if ((now - (conn->timestamp)) < timeout) {
				VRB("ACCEPT - Because the client retransmitted %u " \
					"times within the timeout.\n", retrans_limit);
				/* log connection and remove host from hash table */
				log_connection(key, conn, ACCEPTED);
				g_hash_table_remove(host_table, key);
				return nfq_set_verdict(qh, id, NF_ACCEPT, 0, NULL);
			} else {
				VRB("The client retransmitted often enough " \
					"but the timer of %u seconds ran out.\n", retrans_limit);
			}
		}
		free(key);
	}

	/* if necessary, remove hash table entries which have timed out */
	if (g_hash_table_size(host_table) > cleanup_threshold) {
		DBG("Triggering garbage collector to remove old hash table entries.\n");
		g_hash_table_foreach_remove(host_table, garbage_collect, &now);
	}

	/* dump hash table for debugging */
	if (debug) {
		g_hash_table_foreach(host_table, print_entry, NULL);
	}

	VRB("DROP - Because the retransmission threshold is not achieved yet.\n");
	return nfq_set_verdict(qh, id, NF_DROP, 0, NULL);
}


/* Help output explaining the user how to use the program.
 */
void help( const char *argv ) {

	printf("\nUsage: %s [OPTIONS] \n\n", argv);
	printf("\tRe-check your iptables rules if the program does not receive any data.\n\n");
	printf("Options:\n");
	printf("\t-h, --help\t\tShow this help message and exit.\n");
	printf("\t-v, --verbose\t\tEnable verbose mode and print much information.\n");
	printf("\t-d, --debug\t\tEnable debug mode and print even more information.\n");
	printf("\t-q, --queue=NUM\t\tThe NFQUEUE number to attach to.\n");
	printf("\t-r, --retrans=NUM\tHow many TCP SYN retransmissions do we require?\n");
	printf("\t-t, --timeout=NUM\tWhen should a ``SYN knocking session'' time out?\n");
	printf("\t-c, --cleanup=NUM\tTrigger garbage collection after the hash table has NUM entries.\n");
	printf("\t-l, --logfile=FILE\tLog all hosts which are not passing the SYN test to FILE.\n");
	printf("\t-b, --dwin-begin=SECS\tAmount of seconds after every 15-min-interval where we begin " \
		"to drop SYNs.\n");
	printf("\t-e, --dwin-end=SECS\tAmount of seconds after every 15-min-interval where we stop " \
		"to drop SYNs.\n\n");
}

/* Initializes libnetfilter_queue. If something fails during the process,
 * 1 is returned. If all is fine, 0 is returned.
 */
int init_libnfq( struct nfq_handle **h, struct nfq_q_handle **qh ) {

	DBG("Opening library handle.\n");
	*h = nfq_open();
	if (!(*h)) {
		fprintf(stderr, "Error: nfq_open() failed.\n");
		return 1;
	}

	DBG("Unbinding existing nf_queue handler for AF_INET (if any).\n");
	if (nfq_unbind_pf(*h, AF_INET) < 0) {
		fprintf(stderr, "Error: nfq_unbind_pf() failed.\n");
		return 1;
	}

	DBG("Binding nfnetlink_queue as nf_queue handler for AF_INET.\n");
	if (nfq_bind_pf(*h, AF_INET) < 0) {
		fprintf(stderr, "Error: nfq_bind_pf() failed.\n");
		return 1;
	}

	DBG("Binding this socket to queue '0'.\n");
	*qh = nfq_create_queue(*h,  queue_number, &callback, NULL);
	if (!(*qh)) {
		fprintf(stderr, "Error: nfq_create_queue() failed.\n");
		return 1;
	}

	DBG("Setting copy_packet mode.\n");
	if (nfq_set_mode(*qh, NFQNL_COPY_PACKET, 0xffff) < 0) {
		fprintf(stderr, "Error: can't set packet_copy mode.\n");
		return 1;
	}

	return 0;
}


int main( int argc, char **argv ) {

	int fd = 0;
	int rv = 0;
	int current_opt = 0;
	int option_index = 0;
	char buf[4096] __attribute__ ((aligned)) = { 0 };
	struct nfq_handle *h = NULL;
	struct nfq_q_handle *qh = NULL;

	struct option long_options[] = {
		{"debug",		no_argument,		&debug,		1},
		{"verbose",		no_argument,		&verbose,	1},
		{"help",		no_argument,		NULL,	'h'},
		{"dwin-begin",	required_argument,	NULL,	'b'},
		{"dwin-end",	required_argument,	NULL,	'e'},
		{"queue",		required_argument,	NULL,	'q'},
		{"retrans",		required_argument,	NULL,	'r'},
		{"timeout",		required_argument,	NULL,	't'},
		{"logfile",		required_argument,	NULL,	'l'},
		{"cleanup",		required_argument,	NULL,	'c'},
		{0, 0, 0, 0}
	};

	/* disable buffering for stdout, so log redirection works more smoothly */
	setvbuf(stdout, NULL, _IONBF, 0);

	/* parse cmdline options */
	while (1) {

		current_opt = getopt_long(argc, argv, "hdvr:t:c:l:q:b:e:", long_options, &option_index);

		/* end of options? */
		if (current_opt == -1) {
			break;
		}

		switch (current_opt) {
			case 'd': debug = 1;
				verbose = 1; /* debug mode implies verbose mode */
				break;
			case 'v': verbose = 1;
				break;
			case 'r': retrans_limit = atoi(optarg);
				break;
			case 't': timeout = atoi(optarg);
				break;
			case 'c': cleanup_threshold = atoi(optarg);
				break;
			case 'b': deaf_window.begin = atoi(optarg);
				break;
			case 'e': deaf_window.end = atoi(optarg);
				break;
			case 'l': logfile = optarg;
				break;
			case 'q': queue_number = atoi(optarg);
				break;
			case 'h': help(argv[0]);
				return 0;
			case '?': help(argv[0]);
				return 1;
		}
	}

	printf("\n!!!\n!!! WARNING - brdgrd has not been audited yet. " \
		"Don't use it in productive environments!\n!!!\n\n");

	/* dump configuration to stdout for the user to verify */
	VRB("Configuration:\n\ttimeout = %ds\n\tSYN retransmissions = %d\n\tcleanup threshold " \
		"= %d\n\tnetfilter queue = %d\n\tlogfile = %s\n\tdeaf window begin = %d\n\tdeaf window " \
		"end = %d\n", timeout, retrans_limit, cleanup_threshold, queue_number, logfile ?
		logfile : "n/a", deaf_window.begin, deaf_window.end);

	DBG("Creating hash table to keep track of connecting hosts.\n");
	host_table = g_hash_table_new_full(g_int64_hash, key_equal, data_destroy_func, data_destroy_func);

	/* exit if initialization failed */
	if (init_libnfq(&h, &qh) != 0) {
		fprintf(stderr, "Exiting because libnetfilter_queue init failed.\n");
		return 1;
	}

	fd = nfq_fd(h);

	VRB("Waiting for incoming packets...\n");
	while ((rv = recv(fd, buf, sizeof(buf), 0)) && rv >= 0) {
		nfq_handle_packet(h, buf, rv);
	}

	DBG("Unbinding from queue 0.\n");
	nfq_destroy_queue(qh);

	DBG("Closing library handle.\n");
	nfq_close(h);

	return 0;
}