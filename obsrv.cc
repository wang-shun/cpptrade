
#include "cscpp-config.h"

#include <string>
#include <vector>
#include <stdio.h>
#include <univalue.h>
#include <time.h>
#include <argp.h>
#include <evhtp.h>
#include <assert.h>
#include "Market.h"

using namespace std;
using namespace orderentry;

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

struct HttpApiEntry {
	const char		*path;
	evhtp_callback_cb	cb;
	bool			wantInput;
	bool			jsonInput;
};

class ReqState {
public:
	string			body;
	string			path;
	struct timeval		tstamp;

	ReqState() {
		gettimeofday(&tstamp, NULL);
	}
};

#define PROGRAM_NAME "obsrv"

static const char doc[] =
PROGRAM_NAME " - order book server";

static struct argp_option options[] = {
	{ "bind-address", 1001, "IP/hostname", 0,
	  "Address to which HTTP server is bound" },
	{ "bind-port", 1002, "port", 0,
	  "TCP port to which HTTP server is bound" },
	{ }
};

static error_t parse_opt (int key, char *arg, struct argp_state *state);
static const struct argp argp = { options, parse_opt, NULL, doc };

static const char *opt_bind_address = "0.0.0.0";
static uint16_t opt_bind_port = 7979;

static Market market;

static int64_t
get_content_length (const evhtp_request_t *req)
{
    assert(req != NULL);
    const char *content_len_str = evhtp_kv_find (req->headers_in, "Content-Length");
    if (!content_len_str) {
        return -1;
    }

    return strtoll (content_len_str, NULL, 10);
}

static std::string addressToStr(const struct sockaddr *sockaddr,
				socklen_t socklen)
{
        char name[1025] = "";
        if (!getnameinfo(sockaddr, socklen, name, sizeof(name), NULL, 0, NI_NUMERICHOST))
            return std::string(name);

	return string("");
}

static std::string formatTime(const std::string& fmt, time_t t)
{
	struct tm tm;
	gmtime_r(&t, &tm);

	char timeStr[fmt.size() + 256];
	strftime(timeStr, sizeof(timeStr), fmt.c_str(), &tm);

	return string(timeStr);
}

static std::string httpDateHdr(time_t t)
{
	return formatTime("%a, %d %b %Y %H:%M:%S GMT", t);
}

static std::string isoTimeStr(time_t t)
{
	return formatTime("%FT%TZ", t);
}

static void
logRequest(evhtp_request_t *req, ReqState *state)
{
	assert(req != NULL);
	assert(state != NULL);

	string addrStr = addressToStr(req->conn->saddr,
				      sizeof(struct sockaddr)); // TODO verify

	struct tm tm;
	gmtime_r(&state->tstamp.tv_sec, &tm);

	string timeStr = isoTimeStr(state->tstamp.tv_sec);
	htp_method method = evhtp_request_get_method(req);
	const char *method_name = htparser_get_methodstr_m(method);

	printf("%s - - [%s] \"%s %s\" ? %lld\n",
		addrStr.c_str(),
		timeStr.c_str(),
		method_name,
		req->uri->path->full,
		(long long) get_content_length(req));
}

static evhtp_res
upload_read_cb(evhtp_request_t * req, evbuf_t * buf, void * arg)
{
	ReqState *state = (ReqState *) arg;
	assert(state != NULL);

	size_t bufsz = evbuffer_get_length(buf);
	char *chunk = (char *) malloc(bufsz);
	int rc = evbuffer_remove(buf, chunk, bufsz);
	assert(rc == (int) bufsz);

	state->body.append(chunk, bufsz);

	free(chunk);

	return EVHTP_RES_OK;
}

static evhtp_res
req_finish_cb(evhtp_request_t * req, void * arg)
{
	ReqState *state = (ReqState *) arg;
	assert(state != NULL);

	logRequest(req, state);

	delete state;

	return EVHTP_RES_OK;
}

static void reqInit(evhtp_request_t *req, ReqState *state)
{
	evhtp_headers_add_header(req->headers_out,
		evhtp_header_new("Date",
			 httpDateHdr(state->tstamp.tv_sec).c_str(),
			 0, 1));

	const char *serverVer = PROGRAM_NAME "/" PACKAGE_VERSION;
	evhtp_headers_add_header(req->headers_out,
		evhtp_header_new("Server", serverVer, 0, 0));

	req->cbarg = state;

	evhtp_request_set_hook (req, evhtp_hook_on_request_fini, (evhtp_hook) req_finish_cb, state);
}

static evhtp_res
upload_headers_cb(evhtp_request_t * req, evhtp_headers_t * hdrs, void * arg)
{
	if (evhtp_request_get_method(req) == htp_method_OPTIONS) {
		return EVHTP_RES_OK;
	}

	ReqState *state = new ReqState();
	assert(state != NULL);

	reqInit(req, state);

	evhtp_request_set_hook (req, evhtp_hook_on_read, (evhtp_hook) upload_read_cb, state);

	return EVHTP_RES_OK;
}

static evhtp_res
no_upload_headers_cb(evhtp_request_t * req, evhtp_headers_t * hdrs, void * arg)
{
	if (evhtp_request_get_method(req) == htp_method_OPTIONS) {
		return EVHTP_RES_OK;
	}

	ReqState *state = new ReqState();
	assert(state != NULL);

	reqInit(req, state);

	return EVHTP_RES_OK;
}

void reqDefault(evhtp_request_t * req, void * a)
{
	evhtp_send_reply(req, EVHTP_RES_NOTFOUND);
}

void reqPost(evhtp_request_t * req, void * arg)
{
	ReqState *state = (ReqState *) arg;
	assert(state != NULL);

	UniValue jval;
	if (!jval.read(state->body)) {
		evhtp_send_reply(req, EVHTP_RES_BADREQ);
		return;
	}

	UniValue obj(true);

	string body = obj.write(2) + "\n";

	evbuffer_add(req->buffer_out, body.c_str(), body.size());
	evhtp_send_reply(req, EVHTP_RES_OK);
}

void reqMarketList(evhtp_request_t * req, void * a)
{
	UniValue res(UniValue::VARR);

	vector<string> symbols;
	market.getSymbols(symbols);

	for (vector<string>::iterator t = symbols.begin();
	     t != symbols.end(); t++) {
		res.push_back(*t);
	}

	string body = res.write(2) + "\n";

	evbuffer_add(req->buffer_out, body.c_str(), body.size());
	evhtp_send_reply(req, EVHTP_RES_OK);
}

void reqInfo(evhtp_request_t * req, void * a)
{
	UniValue obj(UniValue::VOBJ);

	obj.pushKV("name", "obsrv");
	obj.pushKV("version", 100);

	string body = obj.write(2) + "\n";

	evbuffer_add(req->buffer_out, body.c_str(), body.size());
	evhtp_send_reply(req, EVHTP_RES_OK);
}

static error_t parse_opt (int key, char *arg, struct argp_state *state)
{
	switch (key) {
	case 1001:	// --bind-address
		opt_bind_address = arg;
		break;

	case 1002:	// --bind-port
		{
		int v = atoi(arg);
		if ((v > 0) && (v < 65536))
			opt_bind_port = (uint16_t) v;
		else
			argp_usage(state);
		break;
		}

	case ARGP_KEY_END:
		break;

	default:
		return ARGP_ERR_UNKNOWN;
	}

	return 0;
}

static const struct HttpApiEntry apiRegistry[] = {
	{ "/info", reqInfo, false, false },
	{ "/marketList", reqMarketList, false, false },
	{ "/post", reqPost, true, true },
};

int main(int argc, char ** argv)
{
	error_t argp_rc = argp_parse(&argp, argc, argv, 0, NULL, NULL);
	if (argp_rc) {
		fprintf(stderr, "%s: argp_parse failed: %s\n",
			argv[0], strerror(argp_rc));
		return EXIT_FAILURE;
	}

	evbase_t * evbase = event_base_new();
	evhtp_t  * htp    = evhtp_new(evbase, NULL);
	evhtp_callback_t *cb = NULL;

	evhtp_set_gencb(htp, reqDefault, NULL);

	for (unsigned int i = 0; i < ARRAY_SIZE(apiRegistry); i++) {
		struct HttpApiEntry *apiEnt = (struct HttpApiEntry *) &apiRegistry[i];
		cb = evhtp_set_cb(htp,
				  apiRegistry[i].path,
				  apiRegistry[i].cb, apiEnt);
		evhtp_callback_set_hook(cb, evhtp_hook_on_headers,
			apiRegistry[i].wantInput ?
				((evhtp_hook) upload_headers_cb) :
				((evhtp_hook) no_upload_headers_cb), apiEnt);
	}

	evhtp_bind_socket(htp, opt_bind_address, opt_bind_port, 1024);
	event_base_loop(evbase, 0);
	return 0;
}
