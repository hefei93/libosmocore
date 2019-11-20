#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <osmocom/core/logging.h>
#include <osmocom/core/select.h>
#include <osmocom/core/application.h>
#include <osmocom/mslookup/mslookup.h>
#include <osmocom/mslookup/mslookup_client.h>
#include <osmocom/mslookup/mslookup_client_mdns.h>
#include <osmocom/mslookup/mdns.h>
#include <osmocom/mslookup/mdns_sock.h>

void *ctx = NULL;

#define TEST_IP OSMO_MSLOOKUP_MDNS_IP4
#define TEST_PORT OSMO_MSLOOKUP_MDNS_PORT

/*
 * Test server (emulates the mDNS server in OsmoHLR) and client
 */
struct osmo_mdns_sock *server_mc;


static void server_reply(struct osmo_mslookup_query *query, uint16_t packet_id)
{
	int rc;
	struct osmo_mslookup_result result = {0};
	struct msgb *msg;

	result.rc = OSMO_MSLOOKUP_RC_OK;
	result.age = 3;
	osmo_sockaddr_str_from_str(&result.host_v4, "42.42.42.42", 444);
	osmo_sockaddr_str_from_str(&result.host_v6, "1122:3344:5566:7788:99aa:bbcc:ddee:ff00", 666);

	msg = msgb_alloc(1024, __func__);
	rc = osmo_mdns_result_encode(ctx, msg, packet_id, query, &result);
	OSMO_ASSERT(rc == 0);
	OSMO_ASSERT(osmo_mdns_sock_send(server_mc, msg) == 0);
}

static int server_recv(struct osmo_fd *osmo_fd, unsigned int what)
{
	int n;
	uint8_t buffer[1024];
	uint16_t packet_id;
	struct osmo_mslookup_query *query;

	fprintf(stderr, "%s\n", __func__);

	/* Parse the message and print it */
	n = read(osmo_fd->fd, buffer, sizeof(buffer));
	OSMO_ASSERT(n >= 0);

	query = osmo_mdns_query_decode(ctx, buffer, n, &packet_id);
	if (!query)
		return -1; /* server receiving own answer is expected */

	fprintf(stderr, "received request\n");
	server_reply(query, packet_id);
	talloc_free(query);
	return n;
}

static void server_init()
{
	fprintf(stderr, "%s\n", __func__);
	server_mc = osmo_mdns_sock_init(ctx, TEST_IP, TEST_PORT, server_recv, NULL, 0);
	OSMO_ASSERT(server_mc);
}

static void server_stop()
{
	fprintf(stderr, "%s\n", __func__);
	OSMO_ASSERT(server_mc);
	osmo_mdns_sock_cleanup(server_mc);
	server_mc = NULL;
}

struct osmo_mslookup_client* client;
struct osmo_mslookup_client_method* client_method;

static void client_init()
{
	fprintf(stderr, "%s\n", __func__);
	client = osmo_mslookup_client_new(ctx);
	OSMO_ASSERT(client);
	client_method = osmo_mslookup_client_add_mdns(client, TEST_IP, TEST_PORT, 1337);
	OSMO_ASSERT(client_method);
}

static void client_recv(struct osmo_mslookup_client *client, uint32_t request_handle,
			const struct osmo_mslookup_query *query, const struct osmo_mslookup_result *result)
{
	char buf[128];
	fprintf(stderr, "%s\n", __func__);
	fprintf(stderr, "client_recv(): %s\n", osmo_mslookup_result_name_b(buf, sizeof(buf), query, result));

	osmo_mslookup_client_request_cleanup(client, request_handle);
}

static void client_query()
{
	struct osmo_mslookup_id id = {.type = OSMO_MSLOOKUP_ID_IMSI,
				      .imsi = "123456789012345"};
	const struct osmo_mslookup_query query = {
		.service = OSMO_MSLOOKUP_SERVICE_HLR_GSUP,
		.id = id,
	};
	struct osmo_mslookup_query_handling handling = {
		.result_timeout_milliseconds = 2000,
		.result_cb = client_recv,
	};

	fprintf(stderr, "%s\n", __func__);
	osmo_mslookup_client_request(client, &query, &handling);
}

static void client_stop()
{
	fprintf(stderr, "%s\n", __func__);
	osmo_mslookup_client_free(client);
	client = NULL;
}
const struct timeval fake_time_start_time = { 0, 0 };

#define fake_time_passes(secs, usecs) do \
{ \
	struct timeval diff; \
	osmo_gettimeofday_override_add(secs, usecs); \
	osmo_clock_override_add(CLOCK_MONOTONIC, secs, usecs * 1000); \
	timersub(&osmo_gettimeofday_override_time, &fake_time_start_time, &diff); \
	LOGP(DLMSLOOKUP, LOGL_DEBUG, "Total time passed: %d.%06d s\n", \
	       (int)diff.tv_sec, (int)diff.tv_usec); \
	osmo_timers_prepare(); \
	osmo_timers_update(); \
} while (0)

static void fake_time_start()
{
	struct timespec *clock_override;

	osmo_gettimeofday_override_time = fake_time_start_time;
	osmo_gettimeofday_override = true;
	clock_override = osmo_clock_override_gettimespec(CLOCK_MONOTONIC);
	OSMO_ASSERT(clock_override);
	clock_override->tv_sec = fake_time_start_time.tv_sec;
	clock_override->tv_nsec = fake_time_start_time.tv_usec * 1000;
	osmo_clock_override_enable(CLOCK_MONOTONIC, true);
	fake_time_passes(0, 0);
}
static void test_server_client()
{
	fprintf(stderr, "-- %s --\n", __func__);
	server_init();
	client_init();
	client_query();

	/* Let the server receive the query and indirectly call server_recv(). As side effect of using the same IP and
	 * port, the client will also receive its own question. The client will dismiss its own question, as it is just
	 * looking for answers. */
	OSMO_ASSERT(osmo_select_main_ctx(1) == 1);

	/* Let the mslookup client receive the answer (also same side effect as above). It does not call the callback
         * (client_recv()) just yet, because it is waiting for the best result within two seconds. */
	OSMO_ASSERT(osmo_select_main_ctx(1) == 1);

	/* Time flies by, client_recv() gets called. */
	fake_time_passes(5, 0);

	server_stop();
	client_stop();
}

/*
 * Run all tests
 */
int main()
{
	talloc_enable_null_tracking();
	ctx = talloc_named_const(NULL, 0, "main");
	osmo_init_logging2(ctx, NULL);

	log_set_print_filename(osmo_stderr_target, 0);
	log_set_print_level(osmo_stderr_target, 0);
	log_set_print_category(osmo_stderr_target, 0);
	log_set_print_category_hex(osmo_stderr_target, 0);
	log_set_use_color(osmo_stderr_target, 0);
	log_set_category_filter(osmo_stderr_target, DLMSLOOKUP, true, LOGL_DEBUG);

	fake_time_start();

	test_server_client();

	log_fini();

	OSMO_ASSERT(talloc_total_blocks(ctx) == 1);
	talloc_free(ctx);
	OSMO_ASSERT(talloc_total_blocks(NULL) == 1);
	talloc_disable_null_tracking();

	return 0;
}