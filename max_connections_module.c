/* max connections module for nginx
** october 2008, ryan dahl (ry@ndahl.us)
*/


#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <ngx_http_upstream.h>
#include <assert.h>

#define DEFAULT_MAX_CONNECTIONS 2

typedef struct {
  ngx_uint_t max_connections;
  ngx_queue_t waiting_requests;
  ngx_array_t *backends; /* backend servers */
} max_connections_srv_conf_t;

typedef struct {
  struct sockaddr *sockaddr;
  socklen_t socklen;
  ngx_str_t *name;

  ngx_uint_t weight;
  ngx_uint_t  max_fails;
  time_t fail_timeout;

  time_t accessed;
  ngx_uint_t down:1;

  ngx_uint_t connections;
} max_connections_backend_t;

typedef struct {
  max_connections_srv_conf_t *maxconn_cf;
  max_connections_backend_t  *backend;
  ngx_queue_t queue;
  ngx_http_request_t *r;
  int waiting:1;
} max_connections_peer_data_t;


/* forward declarations */
static char * max_connections_command (ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static void * max_connections_create_conf(ngx_conf_t *cf);

static ngx_command_t  max_connections_commands[] =
{ { ngx_string("max_connections")
  , NGX_HTTP_UPS_CONF|NGX_CONF_TAKE1
  , max_connections_command
  , 0
  , 0
  , NULL
  }
, ngx_null_command
};

static ngx_http_module_t max_connections_module_ctx =
/* preconfiguration              */ { NULL 
/* postconfiguration             */ , NULL 
/* create main configuration     */ , NULL 
/* init main configuration       */ , NULL 
/* create server configuration   */ , max_connections_create_conf 
/* merge server configuration    */ , NULL 
/* create location configuration */ , NULL 
/* merge location configuration  */ , NULL 
                                    };


ngx_module_t max_connections_module =
                        { NGX_MODULE_V1
/* module context    */ , &max_connections_module_ctx
/* module directives */ , max_connections_commands
/* module type       */ , NGX_HTTP_MODULE
/* init master       */ , NULL
/* init module       */ , NULL
/* init process      */ , NULL
/* init thread       */ , NULL
/* exit thread       */ , NULL
/* exit process      */ , NULL
/* exit master       */ , NULL
                        , NGX_MODULE_V1_PADDING
                        };

static void
max_connections_dispatch_new_connection (max_connections_srv_conf_t *maxconn_cf)
{
  assert(!ngx_queue_empty(&maxconn_cf->waiting_requests)); 

  ngx_queue_t *last = ngx_queue_last(&maxconn_cf->waiting_requests);
  ngx_queue_remove(last);
  max_connections_peer_data_t *peer_data = ngx_queue_data(last, max_connections_peer_data_t, queue);

  ngx_http_request_t *r = peer_data->r;

  ngx_pfree(r->pool, peer_data); // TODO check return value
  
  ngx_http_upstream_connect(r, r->upstream);
}

static max_connections_backend_t*
max_connections_find_open_upstream (max_connections_srv_conf_t *maxconn_cf)
{
  ngx_uint_t i, min_connections = 10000, min_upstream_index = 0;
  
  max_connections_backend_t *backends = maxconn_cf->backends->elts;
  for(i = 0; i < maxconn_cf->backends->nelts; i++) {
    if(backends[i].connections <= min_connections) {
      min_connections = backends[i].connections;
      min_upstream_index = i;
    }
  }

  assert(min_connections <= maxconn_cf->max_connections);

  max_connections_backend_t *choosen = &backends[min_upstream_index];

  if(min_connections == maxconn_cf->max_connections) 
    /* no open slots */
    return NULL;

  assert(choosen->connections < maxconn_cf->max_connections);

  return &backends[min_upstream_index];
}


static void
max_connections_peer_free (ngx_peer_connection_t *pc, void *data, ngx_uint_t state)
{
  max_connections_peer_data_t *peer_data = data;
  max_connections_srv_conf_t *maxconn_cf = peer_data->maxconn_cf;
  max_connections_backend_t *backend = peer_data->backend;

  assert(ngx_queue_empty(&maxconn_cf->waiting_requests) && "max_connections: temp");

  assert(backend->connections > 0);

  backend->connections--;
  //if(state == 0) { // Connection sucessful

  if(!ngx_queue_empty(&maxconn_cf->waiting_requests)) {
    ngx_log_debug1( NGX_LOG_DEBUG_HTTP
                  , pc->log
                  , 0
                  , "max_connections: something in the queue %V"
                  , peer_data->r->uri
                  );
    max_connections_dispatch_new_connection(maxconn_cf);
  }
  //}

  /* TODO try a differnt backend if error */
  pc->tries = 0;
}


static ngx_int_t
max_connections_peer_get (ngx_peer_connection_t *pc, void *data)
{
  max_connections_peer_data_t *peer_data = data;
  max_connections_srv_conf_t *maxconn_cf = peer_data->maxconn_cf;

  ngx_log_debug1( NGX_LOG_DEBUG_HTTP
                , pc->log
                , 0
                , "max_connections_peer_get try %ui"
                , pc->tries
                );

  pc->cached = 0;
  pc->connection = NULL;

  max_connections_backend_t *backend = 
    max_connections_find_open_upstream(maxconn_cf);

  assert(backend != NULL && "should always be an availible backend in max_connections_peer_get()");
  assert(backend->connections < maxconn_cf->max_connections);

  backend->connections++;

  assert(peer_data->backend == NULL);
  peer_data->backend = backend;

  pc->sockaddr = backend->sockaddr;
  pc->socklen  = backend->socklen;
  pc->name     = backend->name;

  ngx_log_debug1( NGX_LOG_DEBUG_HTTP
                , pc->log
                , 0
                , "max_connections sending client to %V"
                , pc->name
                );
  return NGX_OK;
}

static ngx_int_t
max_connections_peer_init (ngx_http_request_t *r, ngx_http_upstream_srv_conf_t *uscf)
{

  max_connections_srv_conf_t *maxconn_cf = 
    ngx_http_conf_upstream_srv_conf(uscf, max_connections_module);

  ngx_log_debug0( NGX_LOG_DEBUG_HTTP
                , r->connection->log
                , 0
                , "max_connections_peer_init max connections %ui"
                , maxconn_cf->max_connections
                );

  max_connections_peer_data_t *peer_data = 
    ngx_palloc(r->pool, sizeof(max_connections_peer_data_t));
  if(peer_data == NULL) return NGX_ERROR;

  peer_data->backend = NULL;
  peer_data->maxconn_cf = maxconn_cf;
  peer_data->waiting = 0;
  peer_data->r = r;

  r->upstream->peer.free = max_connections_peer_free;
  r->upstream->peer.get  = max_connections_peer_get;
  r->upstream->peer.tries = 1;
  r->upstream->peer.data = peer_data;

  if(max_connections_find_open_upstream(maxconn_cf) == NULL) {
    ngx_log_debug1( NGX_LOG_DEBUG_HTTP
                  , r->connection->log
                  , 0
                  , "max_connections queue request %p"
                  , r
                  );
    ngx_queue_insert_head(&maxconn_cf->waiting_requests, &peer_data->queue);
    return NGX_BUSY;
  }

  return NGX_OK;
}


static ngx_int_t
max_connections_init(ngx_conf_t *cf, ngx_http_upstream_srv_conf_t *uscf)
{
  ngx_log_debug0(NGX_LOG_DEBUG_HTTP, cf->log, 0, "max_connections_init");

  max_connections_srv_conf_t *maxconn_cf = 
    ngx_http_conf_upstream_srv_conf(uscf, max_connections_module);

/* allocate all the max_connections_backend_t. put them in
 * maxconn_cf->backends */
  if(uscf->servers == NULL) return NGX_ERROR;
  ngx_http_upstream_server_t *server = uscf->servers->elts;
  ngx_uint_t i, j;
  ngx_uint_t number_backends = 0;

  for (i = 0; i < uscf->servers->nelts; i++) 
      number_backends += server[i].naddrs;

  ngx_array_t *backends = 
    ngx_array_create(cf->pool, number_backends, sizeof(max_connections_backend_t));
  if (backends == NULL) return NGX_ERROR;

  /* one hostname can have multiple IP addresses in DNS */
  ngx_uint_t n;
  for (n = 0, i = 0; i < uscf->servers->nelts; i++) {
    for (j = 0; j < server[i].naddrs; j++, n++) {
      max_connections_backend_t *backend = ngx_array_push(backends);
      backend->sockaddr     = server[i].addrs[j].sockaddr;
      backend->socklen      = server[i].addrs[j].socklen;
      backend->name         = &(server[i].addrs[j].name);
      backend->max_fails    = server[i].max_fails;
      backend->fail_timeout = server[i].fail_timeout;
      backend->down         = server[i].down;
      backend->weight       = server[i].down ? 0 : server[i].weight;
      backend->connections  = 0;
    }
  }
  maxconn_cf->backends = backends;

  ngx_log_debug1( NGX_LOG_DEBUG_HTTP
                , cf->log
                , 0
                , "max_connections number of upstream"
                );

  uscf->peer.init = max_connections_peer_init;

  ngx_queue_init(&maxconn_cf->waiting_requests);
  assert(ngx_queue_empty(&maxconn_cf->waiting_requests));

  return NGX_OK;
}

static char *
max_connections_command (ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
  ngx_http_upstream_srv_conf_t *uscf = 
    ngx_http_conf_get_module_srv_conf(cf, ngx_http_upstream_module);

  max_connections_srv_conf_t *maxconn_cf = 
    ngx_http_conf_upstream_srv_conf(uscf, max_connections_module);

  uscf->peer.init_upstream = max_connections_init;

  /* read options */
  ngx_str_t *value = cf->args->elts;
  ngx_int_t max_connections = ngx_atoi(value[1].data, value[1].len);

  if (max_connections == NGX_ERROR || max_connections == 0) {
    ngx_conf_log_error( NGX_LOG_EMERG
                      , cf
                      , 0
                      , "invalid value \"%V\" in \"%V\" directive"
                      , &value[1]
                      , &cmd->name
                      );
    return NGX_CONF_ERROR;
  }
  maxconn_cf->max_connections = (ngx_uint_t)max_connections;
  return NGX_CONF_OK;
}

static void *
max_connections_create_conf(ngx_conf_t *cf)
{
    max_connections_srv_conf_t  *conf = 
      ngx_pcalloc(cf->pool, sizeof(max_connections_srv_conf_t));

    if (conf == NULL) return NGX_CONF_ERROR;

    conf->max_connections = DEFAULT_MAX_CONNECTIONS;
    return conf;
}
