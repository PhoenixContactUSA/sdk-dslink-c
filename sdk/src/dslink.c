#define LOG_TAG "dslink"
#include "dslink/log.h"

#include <argtable3.h>
#include <string.h>
#include <wslay/wslay.h>
#include <jansson.h>
#include <openssl/err.h>

#include "dslink/handshake.h"
#include "dslink/utils.h"
#include "dslink/ws.h"

#define SECONDS_TO_MILLIS(count) count * 1000

#define DSLINK_CONNECT_FATAL_ERROR 0
#define DSLINK_COULDNT_CONNECT 2
#define DSLINK_DISCONNECTED 3
#define DSLINK_DISCONNECTED_ONPURPOSE 4


#define DSLINK_RESPONDER_MAP_INIT(var, type) \
    responder->var = dslink_calloc(1, sizeof(Map)); \
    if (!responder->var) { \
        goto cleanup; \
    } \
    if (dslink_map_init(responder->var, \
            dslink_map_##type##_cmp, \
            dslink_map_##type##_key_len_cal, \
            dslink_map_hash_key) != 0) { \
        dslink_free(responder->var); \
        responder->var = NULL; \
        goto cleanup; \
    }

#define DSLINK_REQUESTER_MAP_INIT(var, type) \
    requester->var = dslink_calloc(1, sizeof(Map)); \
    if (!requester->var) { \
        goto cleanup; \
    } \
    if (dslink_map_init(requester->var, \
            dslink_map_##type##_cmp, \
            dslink_map_##type##_key_len_cal, \
            dslink_map_hash_key) != 0) { \
        dslink_free(requester->var); \
        requester->var = NULL; \
        goto cleanup; \
    }

static inline
void dslink_print_help() {
    printf("See --help for usage\n");
}

void dslink_free_handle(uv_handle_t *handle) {
    dslink_free(handle);
}

static
int dslink_parse_opts(int argc,
                      char **argv,
                      DSLinkConfig *config) {
    int ret = 0;

    json_t *json = NULL;

    struct arg_lit *help;
    struct arg_str *broker, *token, *log, *name;
    struct arg_end *end;

    void *argTable[] = {
        help = arg_lit0("h", "help", "Displays this help menu"),
        broker = arg_str0("b", "broker", "url", "Sets the broker URL to connect to"),
        token = arg_str0("t", "token", NULL, "Sets the token"),
        log = arg_str0("l", "log", "log type", "Sets the logging level"),
        name = arg_str0("n", "name", NULL, "Sets the dslink name"),
        end = arg_end(6)
    };

    if (arg_nullcheck(argTable) != 0) {
        return DSLINK_ALLOC_ERR;
    }

    int errs = arg_parse(argc, argv, argTable);

    if (help->count > 0) {
        printf("Usage: <opts>\n");
        arg_print_glossary(stdout, argTable, " %-25s %s\n");
        ret = 1;
        goto exit;
    }

    if (errs > 0) {
        dslink_print_help();
        arg_print_errors(stdout, end, ":");
        ret = 1;
        goto exit;
    }

    const char *brokerUrl;
    json = dslink_read_dslink_json();

    if (broker->count > 0) {
        brokerUrl = broker->sval[0];
    } else {
        json_t *str = dslink_json_raw_get_config(json, "broker");
        if (json_is_string(str)) {
            brokerUrl = json_string_value(str);
        } else {
            brokerUrl = "http://127.0.0.1:8080/conn";
        }
    }
    config->broker_url = dslink_url_parse(brokerUrl);

    if (token->count > 0) {
        config->token = token->sval[0];
    } else if (json) {
        json_t *str = dslink_json_raw_get_config(json, "token");
        if (str) {
            config->token = json_string_value(str);
        }
    }

    if (name->count > 0) {
        config->name = name->sval[0];
    } else if (json) {
        json_t *str = dslink_json_raw_get_config(json, "name");
        if (str) {
            config->name = json_string_value(str);
        }
    }

    if (!config->broker_url) {
        log_fatal("Failed to parse broker url\n");
        ret = 1;
        goto exit;
    }

    if (log->count > 0) {
        const char *lvl = log->sval[0];
        if (dslink_log_set_lvl(lvl) != 0) {
            printf("Invalid log level: %s\n", lvl);
            dslink_print_help();
            ret = 1;
            goto exit;
        }
    } else {
        json_t *lvl = dslink_json_raw_get_config(json, "log");
        if (json_is_string(lvl)) {
            if (dslink_log_set_lvl(json_string_value(lvl)) != 0) {
                printf("Invalid log level: %s\n", json_string_value(lvl));
                dslink_print_help();
                ret = 1;
                goto exit;
            }
        }
    }

exit:
    arg_freetable(argTable, sizeof(argTable) / sizeof(argTable[0]));
    if (json) {
        json_decref(json);
    }
    return ret;
}

static
int dslink_init_responder_node_tree(Responder *responder) {
    responder->super_root = dslink_node_create(NULL, "/", "node");
    if (!responder->super_root) {
        return DSLINK_ALLOC_ERR;
    }
    return 0;
}

int dslink_reset_responder_node_tree(DSLink *link) {
    if(!(link->is_responder && link->responder))
        return 1;

    if (link->responder->super_root) {
        dslink_node_tree_free(link, link->responder->super_root);
    }

    return dslink_init_responder_node_tree(link->responder);
}

static void dslink_destroy_responder_maps(Responder *responder) {
    if (responder->open_streams) {
        dslink_map_free(responder->open_streams);
        dslink_free(responder->open_streams);
        responder->open_streams = NULL;
    }
    if (responder->list_subs) {
        dslink_map_free(responder->list_subs);
        dslink_free(responder->list_subs);
        responder->list_subs = NULL;
    }
    if (responder->value_path_subs) {
        dslink_map_free(responder->value_path_subs);
        dslink_free(responder->value_path_subs);
        responder->value_path_subs = NULL;
    }
    if (responder->value_sid_subs) {
        dslink_map_free(responder->value_sid_subs);
        dslink_free(responder->value_sid_subs);
        responder->value_sid_subs = NULL;
    }
}

static
int dslink_init_responder_maps(Responder *responder) {

    DSLINK_RESPONDER_MAP_INIT(open_streams, uint32)
    DSLINK_RESPONDER_MAP_INIT(list_subs, str)
    DSLINK_RESPONDER_MAP_INIT(value_path_subs, str)
    DSLINK_RESPONDER_MAP_INIT(value_sid_subs, uint32)
    return 0;
cleanup:
    dslink_destroy_responder_maps(responder);
    return DSLINK_ALLOC_ERR;
}

static
void dslink_destroy_responder(DSLink *link) {
    if (link->is_responder) {
        if (link->responder->super_root) {
            dslink_node_tree_free(link, link->responder->super_root);
        }

        dslink_destroy_responder_maps(link->responder);

        dslink_free(link->responder);
        link->responder = NULL;
    }
}

static
int dslink_init_requester(Requester *requester) {
    DSLINK_REQUESTER_MAP_INIT(open_streams, uint32)
    DSLINK_REQUESTER_MAP_INIT(list_subs, str)
    DSLINK_REQUESTER_MAP_INIT(request_handlers, uint32)
    DSLINK_REQUESTER_MAP_INIT(value_handlers, uint32)

    requester->rid = dslink_malloc(sizeof(uint32_t));
    *requester->rid = 0;
    requester->sid = dslink_malloc(sizeof(uint32_t));
    *requester->sid = 0;

    return 0;
    cleanup:
    if (requester->open_streams) {
        dslink_map_free(requester->open_streams);
        dslink_free(requester->open_streams);
        requester->open_streams = NULL;
    }

    if (requester->list_subs) {
        dslink_map_free(requester->list_subs);
        dslink_free(requester->list_subs);
        requester->list_subs = NULL;
    }

    if (requester->value_handlers) {
        dslink_map_free(requester->value_handlers);
        dslink_free(requester->value_handlers);
        requester->value_handlers = NULL;
    }

    if (requester->rid) {
        dslink_free(requester->rid);
        requester->rid = NULL;
    }

    if (requester->sid) {
        dslink_free(requester->sid);
        requester->sid = NULL;
    }

    return DSLINK_ALLOC_ERR;
}

static
int handle_config(DSLinkConfig *config, const char *name, int argc, char **argv) {
    memset(config, 0, sizeof(DSLinkConfig));
    config->name = name;

    int ret = 0;
    if ((ret = dslink_parse_opts(argc, argv, config)) != 0) {
        if (ret == DSLINK_ALLOC_ERR) {
            log_fatal("Failed to allocate memory during argument parsing\n");
        }
        return ret;
    }

    return ret;
}

int dslink_handle_key(DSLink *link) {
    int ret = 0;
    if((ret = dslink_crypto_ecdh_init_context(&link->key)) != 0){
        log_fatal("Failed to init key");
    } else{
        if ((ret = dslink_handshake_key_pair_fs(&link->key, ".key")) != 0) {
            if (ret == DSLINK_CRYPT_KEY_DECODE_ERR) {
                log_fatal("Failed to decode existing key\n");
            } else if (ret == DSLINK_OPEN_FILE_ERR) {
                log_fatal("Failed to write generated key to disk\n");
            } else if (ret == DSLINK_CRYPT_KEY_PAIR_GEN_ERR) {
                log_fatal("Failed to generated key\n");
            } else {
                log_fatal("Unknown error occurred during key handling: %d\n", ret);
            }
        }
    }
    return ret;
}

void dslink_close(DSLink *link) {
    link->closing = 1;
    wslay_event_queue_close(link->_ws, WSLAY_CODE_NORMAL_CLOSURE, NULL, 0);
    uv_stop(&link->loop);
}

static
void dslink_link_clear(DSLink *link) {
    if (link->_ws) {
        wslay_event_context_free(link->_ws);
    }

    if (link->link_data) {
        json_decref(link->link_data);
    }
}

void dslink_link_free(DSLink *link) {
    dslink_link_clear(link);
    dslink_free(link);
}

json_t *dslink_read_dslink_json() {
    json_error_t err;
    json_t *json = json_load_file("dslink.json", JSON_DECODE_ANY, &err);

    if (!json) {
        log_warn("Failed to load dslink.json: %s\n", err.text);
        return NULL;
    }

    if (!json_is_object(json)) {
        log_warn("Failed to load dslink.json: Root is not a JSON object.\n");
        return NULL;
    }

    return json;
}

json_t *dslink_json_raw_get_config(json_t *json, const char *key) {
    if (!json_is_object(json)) {
        return NULL;
    }

    json_t *configs = json_object_get(json, "configs");

    if (!json_is_object(configs)) {
        return NULL;
    }

    json_t *section = json_object_get(configs, key);

    if (!json_is_object(section)) {
        return NULL;
    }

    json_t *value = json_object_get(section, "value");

    if (value) {
        return value;
    }

    json_t *defaultValue = json_object_get(section, "default");

    if (defaultValue) {
        return defaultValue;
    }

    return NULL;
}

json_t *dslink_json_get_config(DSLink *link, const char *key) {
    if (!link) {
        return NULL;
    }

    return dslink_json_raw_get_config(link->dslink_json, key);
}

static
int dslink_init_do(DSLink *link, DSLinkCallbacks *cbs) {
    link->closing = 0;

    *link->msg = 0;

    json_t *handshake = NULL;
    char *dsId = NULL;
    Socket *sock = NULL;

    int ret = DSLINK_CONNECT_FATAL_ERROR;
    if (dslink_handle_key(link) != 0) {
        goto exit;
    }

    if (link->is_responder) {
        if (dslink_init_responder_maps(link->responder) != 0) {
            log_fatal("Failed to initialize responder maps\n");
            goto exit;
        }
    }

    if (link->is_requester) {
        link->requester = dslink_calloc(1, sizeof(Requester));
        if (!link->requester) {
            log_fatal("Failed to create requester\n");
            goto exit;
        }

        if (dslink_init_requester(link->requester) != 0) {
            log_fatal("Failed to initialize requester\n");
            goto exit;
        }
    }

    if (cbs->init_cb) {
        cbs->init_cb(link);
    }

    link->initialized = 1;

    if ((ret = dslink_handshake_generate(link, &handshake, &dsId)) != 0) {
        log_fatal("Handshake failed: %d\n", ret);
        ret = DSLINK_COULDNT_CONNECT;
        goto exit;
    }

    const char *uri = json_string_value(json_object_get(handshake, "wsUri"));
    const char *tKey = json_string_value(json_object_get(handshake, "tempKey"));
    const char *salt = json_string_value(json_object_get(handshake, "salt"));

    const char *format = json_string_value(json_object_get(handshake, "format"));
    link->is_msgpack = 0;
    if(format != NULL && strcmp(format, "msgpack") == 0)
        link->is_msgpack = 1;

    const char* format_str = link->is_msgpack?"msgpack":"json";
    log_info("Format was decided as %s by server\n", format_str);

    if (!(uri && ((tKey && salt) || link->config.token))) {
        log_fatal("Handshake didn't return the "
                      "necessary parameters to complete\n");
        ret = DSLINK_COULDNT_CONNECT;
        goto exit;
    }

    if ((ret = dslink_handshake_connect_ws(link->config.broker_url, &link->key, uri,
                                           tKey, salt, dsId, link->config.token, format_str, &sock)) != 0) {
        log_fatal("Failed to connect to the broker: %s:%d with error code %d\n",
                  link->config.broker_url->host,
                  link->config.broker_url->port,ret);

        ret = DSLINK_COULDNT_CONNECT;
        goto exit;
    } else {
        log_info("Successfully connected to the broker: %s:%d\n",
                 link->config.broker_url->host,
                 link->config.broker_url->port);
    }

    link->_socket = sock;

    if (cbs->on_connected_cb) {
        cbs->on_connected_cb(link);
    }
    link->first_conn = 1;

    dslink_handshake_handle_ws(link, cbs->on_requester_ready_cb);

    log_warn("Disconnected from the broker\n")
    if (cbs->on_disconnected_cb) {
        cbs->on_disconnected_cb(link);
    }

    if (link->closing != 1) {
        ret = DSLINK_DISCONNECTED;
    } else {
        ret = DSLINK_DISCONNECTED_ONPURPOSE;
    }

    exit:
    if (link->is_responder) {
        dslink_destroy_responder_maps(link->responder);
    }

    if (link->is_requester) {
        if (link->requester->list_subs) {
            dslink_map_free(link->requester->list_subs);
            dslink_free(link->requester->list_subs);
            link->requester->list_subs = NULL;
        }

        if (link->requester->request_handlers) {
            dslink_map_free(link->requester->request_handlers);
            dslink_free(link->requester->request_handlers);
            link->requester->request_handlers = NULL;
        }

        if (link->requester->open_streams) {
            dslink_map_free(link->requester->open_streams);
            dslink_free(link->requester->open_streams);
            link->requester->open_streams = NULL;
        }

        if (link->requester->value_handlers) {
            dslink_map_free(link->requester->value_handlers);
            dslink_free(link->requester->value_handlers);
            link->requester->value_handlers = NULL;
        }

        if (link->requester->rid) {
            dslink_free(link->requester->rid);
            link->requester->rid = NULL;
        }

        if (link->requester->sid) {
            dslink_free(link->requester->sid);
            link->requester->sid = NULL;
        }

        dslink_free(link->requester);
        link->requester = NULL;
    }

    dslink_crypto_ecdh_deinit_context(&link->key);
    dslink_socket_close(&sock);
    DSLINK_CHECKED_EXEC(dslink_free, dsId);
    DSLINK_CHECKED_EXEC(json_delete, handshake);

    return ret;
}

//thread-safe API async handle callbacks
void dslink_async_get_node_value(uv_async_t *async_handle) {

    DSLink *link = (DSLink*)(async_handle->loop->data);
    if(!link) {
        log_warn("Async get node callback: DSLink not found!\n");
    } else {

        DSLinkAsyncGetData *async_data = (DSLinkAsyncGetData*)async_handle->data;
        if (!async_handle->data || !async_data->node_path) {
            log_warn("Async get node callback: async_data not found!\n");
        } else {
            DSNode *node = dslink_node_get_path(link->responder->super_root, async_data->node_path);
            if (node) {
                if (async_data->callback) {
                    async_data->callback(json_copy(node->value), async_data->callback_data);
                }

            } else {
                log_info("Async get node callback: Node not found in the path: %s\n",async_data->node_path);
            }
            //free async_data which is allocated in API func
            dslink_free(async_data->node_path);
            dslink_free(async_data);
        }
        uv_sem_post(&link->async_get_data_sem);
    }
}

void dslink_async_set_node_value(uv_async_t *async_handle) {

    DSLink *link = (DSLink *) (async_handle->loop->data);
    if (!link) {
        log_warn("Async set node callback: DSLink not found!\n");
    } else {
        DSLinkAsyncSetData *async_data = (DSLinkAsyncSetData *) async_handle->data;

        if (!async_handle->data || !async_data->node_path || !async_data->set_value) {
            log_warn("Async set node callback: async_data not found!\n");
        } else {
            DSNode *node = dslink_node_get_path(link->responder->super_root, async_data->node_path);
            int ret;
            if (node) {
                //json value's ref count must be 1 and should not be used in the other thread anymore
                if (dslink_node_update_value(link, node, async_data->set_value) == 0)
                    ret = 0;
                else
                    ret = -1;
            } else {
                log_info("Async set node callback: Node not found in the path: %s\n", async_data->node_path);
                ret = -1;
            }
            if (async_data->callback) {
                async_data->callback(ret, async_data->callback_data);
            }

            //free async_data which is allocated in API func
            dslink_free(async_data->node_path);
            json_decref(async_data->set_value);
            dslink_free(async_data);
        }
        uv_sem_post(&link->async_set_data_sem);
    }
}

void dslink_async_run(uv_async_t *async_handle) {


    DSLink *link = (DSLink*)(async_handle->loop->data);
    if(!link) {
        log_warn("Async run callback: DSLink not found!\n");
    } else {
        DSLinkAsyncRunData *async_data = (DSLinkAsyncRunData*)async_handle->data;
        if (!async_handle->data) {
            log_warn("Async run callback: async_data not found!\n");
        } else {
            if (async_data->callback) {
                async_data->callback(link, async_data->callback_data);
            }
            //free async_data which is allocated in API func
            dslink_free(async_data);
        }
        uv_sem_post(&link->async_run_data_sem);
    }
}

void dslink_handle_reconnect(uv_timer_t* handle) {
    DSLink *link = handle->data;
    uv_stop(&link->loop);
}

int dslink_init(int argc, char **argv,
                const char *name, uint8_t isRequester,
                uint8_t isResponder, DSLinkCallbacks *cbs) {
    DSLink *link = dslink_malloc(sizeof(DSLink));
    bzero(link, sizeof(DSLink));
    uv_loop_init(&link->loop);
    link->loop.data = link;
    link->initialized = 0;
    link->first_conn = 0;

    link->main_thread_id = uv_thread_self();

    dslink_crypto_fips_mode_set(1);

    fflush(stdout);
    //thread-safe API async handle set
    if(uv_async_init(&link->loop, &link->async_get, dslink_async_get_node_value)) {
        log_warn("Async handle init error\n");
    }
    if(uv_async_init(&link->loop, &link->async_set, dslink_async_set_node_value)) {
        log_warn("Async handle init error\n");
    }
    if(uv_async_init(&link->loop, &link->async_run, dslink_async_run)) {
        log_warn("Async handle init error\n");
    }

    link->async_close = 0;
    uv_sem_init(&link->async_set_data_sem,1);
    uv_sem_init(&link->async_get_data_sem,1);
    uv_sem_init(&link->async_run_data_sem,1);

    link->is_responder = isResponder;
    link->is_requester = isRequester;

    if (handle_config(&link->config, name, argc, argv) != 0) {
        return 1;
    }


    int ret = 0;
    link->msg = dslink_malloc(sizeof(uint32_t));

    link->dslink_json = dslink_read_dslink_json();

    if (link->is_responder) {
        link->responder = dslink_calloc(1, sizeof(Responder));

        if (!link->responder) {
            log_fatal("Failed to create responder\n");
            goto exit;
        }

        if (dslink_init_responder_node_tree(link->responder) != 0) {
            log_fatal("Failed to initialize responder node tree\n");
            goto exit;
        }
    }

    link->reconnectTimer = dslink_calloc(1, sizeof(uv_timer_t));
    link->reconnectTimer->data = link;
    uv_timer_init(&link->loop, link->reconnectTimer);
    while(1) {
        ret = dslink_init_do(link, cbs);
        if (ret == DSLINK_CONNECT_FATAL_ERROR) {
            log_warn("Error occurred while connecting to broker!\n");
            goto exit;
        } else if (ret == DSLINK_DISCONNECTED_ONPURPOSE) {
            log_info("DSLink closed on purpose\n");
            goto exit;
        } else if (ret == DSLINK_DISCONNECTED) {
            log_info("DSLink disconnected from broker\n");
        } else /*means DSLINK_COULDNT_CONNECT*/ {
            log_info("DSLink couldn't connect to broker!\n");
        }

        dslink_link_clear(link);
        uv_timer_start(link->reconnectTimer, dslink_handle_reconnect, 5000, 0);
        uv_run(&link->loop,UV_RUN_DEFAULT);
        log_info("Attempting to reconnect...\n");
    }


exit:
    if(link->reconnectTimer) {
        uv_timer_stop(link->reconnectTimer);
        uv_close((uv_handle_t *)link->reconnectTimer, dslink_free_handle);
        link->reconnectTimer = NULL;
    }

    link->async_close = 1;
    uv_sem_post(&link->async_set_data_sem);
    uv_sem_post(&link->async_get_data_sem);
    uv_sem_post(&link->async_run_data_sem);

    dslink_destroy_responder(link);

    if (link->dslink_json) {
        json_decref(link->dslink_json);
    }

    if (link->msg) {
        dslink_free(link->msg);
    }

    uv_sem_destroy(&link->async_set_data_sem);
    uv_sem_destroy(&link->async_get_data_sem);
    uv_sem_destroy(&link->async_run_data_sem);

    uv_close((uv_handle_t*)&link->async_set,NULL);
    uv_close((uv_handle_t*)&link->async_get,NULL);
    uv_close((uv_handle_t*)&link->async_run,NULL);

    uv_loop_close(&link->loop);
    dslink_link_free(link);

    return ret;
}
