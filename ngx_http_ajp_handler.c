
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <ngx_http_ajp.h>
#include <ngx_http_ajp_header.h>
#include <ngx_http_ajp_handler.h>


static ngx_int_t ngx_http_ajp_eval(ngx_http_request_t *r,
    ngx_http_ajp_loc_conf_t *alcf);
#if (NGX_HTTP_CACHE)
static ngx_int_t ngx_http_ajp_create_key(ngx_http_request_t *r);
#endif
static ngx_int_t ngx_http_ajp_create_request(ngx_http_request_t *r);
static ngx_int_t ngx_http_ajp_reinit_request(ngx_http_request_t *r);
static ngx_int_t ngx_http_ajp_process_header(ngx_http_request_t *r);
static ngx_int_t ngx_http_ajp_input_filter(ngx_event_pipe_t *p,
    ngx_buf_t *buf);
static void ngx_http_ajp_abort_request(ngx_http_request_t *r);
static void ngx_http_ajp_finalize_request(ngx_http_request_t *r,
    ngx_int_t rc);

static ngx_int_t ngx_http_upstream_send_request_body(ngx_http_request_t *r, 
    ngx_http_upstream_t *u);
static ngx_chain_t *ajp_data_msg_send_body(ngx_http_request_t *r, size_t max_size,
    ngx_chain_t **body);
static void ngx_http_upstream_send_request_body_handler(ngx_http_request_t *r,
    ngx_http_upstream_t *u);
static void ngx_http_upstream_dummy_handler(ngx_http_request_t *r,
    ngx_http_upstream_t *u);

static ngx_int_t ngx_http_ajp_input_filter_save_tiny_buffer(ngx_http_request_t *r,
    ngx_buf_t *buf);
static void ngx_http_ajp_end_response(ngx_http_ajp_ctx_t *a, ngx_event_pipe_t *p, int reuse);

ngx_int_t
ngx_http_ajp_handler(ngx_http_request_t *r)
{
    ngx_int_t                 rc;
    ngx_http_upstream_t      *u;
    ngx_http_ajp_ctx_t       *a;
    ngx_http_ajp_loc_conf_t  *alcf;

    if (r->subrequest_in_memory) {
        ngx_log_error(NGX_LOG_ALERT, r->connection->log, 0,
                "ngx_ajp_module does not support "
                "subrequest in memory");
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    if (ngx_http_upstream_create(r) != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    a = ngx_pcalloc(r->pool, sizeof(ngx_http_ajp_ctx_t));
    if (a == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    a->state = ngx_http_ajp_st_init_state;

    ngx_http_set_ctx(r, a, ngx_http_ajp_module);

    alcf = ngx_http_get_module_loc_conf(r, ngx_http_ajp_module);

    if (alcf->ajp_lengths) {
        if (ngx_http_ajp_eval(r, alcf) != NGX_OK) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }
    }

    u = r->upstream;

    u->schema.len = sizeof("ajp://") - 1;
    u->schema.data = (u_char *) "ajp://";

    u->output.tag = (ngx_buf_tag_t) &ngx_http_ajp_module;

    u->conf = &alcf->upstream;

#if (NGX_HTTP_CACHE)
    u->create_key = ngx_http_ajp_create_key;
#endif
    u->create_request = ngx_http_ajp_create_request;
    u->reinit_request = ngx_http_ajp_reinit_request;
    u->process_header = ngx_http_ajp_process_header;
    u->abort_request = ngx_http_ajp_abort_request;
    u->finalize_request = ngx_http_ajp_finalize_request;

    u->buffering = 1;

    u->pipe = ngx_pcalloc(r->pool, sizeof(ngx_event_pipe_t));
    if (u->pipe == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    u->pipe->input_filter = ngx_http_ajp_input_filter;
    u->pipe->input_ctx = r;
    u->pipe->keepalive = 1;

    rc = ngx_http_read_client_request_body(r, ngx_http_upstream_init);

    if (rc >= NGX_HTTP_SPECIAL_RESPONSE) {
        return rc;
    }

    return NGX_DONE;
}


static ngx_int_t
ngx_http_ajp_eval(ngx_http_request_t *r, ngx_http_ajp_loc_conf_t *alcf)
{
    ngx_url_t  u;

    ngx_memzero(&u, sizeof(ngx_url_t));

    if (ngx_http_script_run(r, &u.url, alcf->ajp_lengths->elts, 0,
                            alcf->ajp_values->elts)
        == NULL)
    {
        return NGX_ERROR;
    }

    u.no_resolve = 1;

    if (ngx_parse_url(r->pool, &u) != NGX_OK) {
         if (u.err) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                          "%s in upstream \"%V\"", u.err, &u.url);
        }

        return NGX_ERROR;
    }

    if (u.no_port) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                      "no port in upstream \"%V\"", &u.url);
        return NGX_ERROR;
    }

    r->upstream->resolved = ngx_pcalloc(r->pool,
                                        sizeof(ngx_http_upstream_resolved_t));
    if (r->upstream->resolved == NULL) {
        return NGX_ERROR;
    }

    if (u.addrs && u.addrs[0].sockaddr) {
        r->upstream->resolved->sockaddr = u.addrs[0].sockaddr;
        r->upstream->resolved->socklen = u.addrs[0].socklen;
        r->upstream->resolved->naddrs = 1;
        r->upstream->resolved->host = u.addrs[0].name;

    } else {
        r->upstream->resolved->host = u.host;
        r->upstream->resolved->port = u.port;
    }

    return NGX_OK;
}


#if (NGX_HTTP_CACHE)

static ngx_int_t
ngx_http_ajp_create_key(ngx_http_request_t *r)
{
    ngx_str_t                    *key;
    ngx_http_ajp_loc_conf_t      *alcf;

    key = ngx_array_push(&r->cache->keys);
    if (key == NULL) {
        return NGX_ERROR;
    }

    alcf = ngx_http_get_module_loc_conf(r, ngx_http_ajp_module);

    if (ngx_http_complex_value(r, &alcf->cache_key, key) != NGX_OK) {
        return NGX_ERROR;
    }

    return NGX_OK;
}

#endif


static ngx_int_t
ngx_http_ajp_create_request(ngx_http_request_t *r)
{
    ajp_msg_t                    *msg;
    ngx_chain_t                  *cl;
    ngx_http_ajp_ctx_t           *a;
    ngx_http_ajp_loc_conf_t      *alcf;

    a = ngx_http_get_module_ctx(r, ngx_http_ajp_module);
    alcf = ngx_http_get_module_loc_conf(r, ngx_http_ajp_module);

    if (a == NULL || alcf == NULL) {
        return NGX_ERROR;
    }

    msg = ajp_msg_reuse(&a->msg);

    if (NGX_OK != ajp_msg_create_buffer(r->pool,
                alcf->ajp_header_packet_buffer_size_conf, msg)) {
        return NGX_ERROR;
    }

    if (NGX_OK != ajp_marshal_into_msgb(msg, r, alcf)) {
        return NGX_ERROR;
    }

    ajp_msg_end(msg);

    cl = ngx_alloc_chain_link(r->pool);
    if (cl == NULL) {
        return NGX_ERROR;
    }

    cl->buf = msg->buf;
    cl->buf->flush = 1;

    a->state = ngx_http_ajp_st_forward_request_sent;

    r->upstream->request_bufs = cl;

    if (alcf->upstream.pass_request_body) {
        a->body = r->upstream->request_bufs;
        cl->next = ajp_data_msg_send_body(r,
                alcf->max_ajp_data_packet_size_conf, &a->body);

        if (a->body) {
            a->state = ngx_http_ajp_st_request_body_data_sending;
        }
        else {
            a->state = ngx_http_ajp_st_request_send_all_done;
        }

    } else {
        a->state = ngx_http_ajp_st_request_send_all_done;
        cl->next = NULL;
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_ajp_reinit_request(ngx_http_request_t *r)
{
    ngx_http_ajp_ctx_t           *a;
    ngx_http_ajp_loc_conf_t      *alcf;

    a = ngx_http_get_module_ctx(r, ngx_http_ajp_module);
    alcf = ngx_http_get_module_loc_conf(r, ngx_http_ajp_module);

    if (a == NULL || alcf == NULL) {
        return NGX_ERROR;
    }

    a->state = ngx_http_ajp_st_init_state;
    a->length = 0;

    ajp_msg_reuse(&a->msg);

    a->save = NULL;
    a->body = NULL;

    return NGX_OK;
}


static ngx_int_t
ngx_http_ajp_process_header(ngx_http_request_t *r)
{
    uint16_t                      length;
    u_char                       *pos, type, reuse;
    ngx_int_t                     rc;
    ngx_buf_t                    *buf;
    ajp_msg_t                    *msg;
    ngx_http_upstream_t          *u;
    ngx_http_ajp_ctx_t           *a;
    ngx_http_ajp_loc_conf_t      *alcf;


    a = ngx_http_get_module_ctx(r, ngx_http_ajp_module);
    alcf = ngx_http_get_module_loc_conf(r, ngx_http_ajp_module);

    if (a == NULL || alcf == NULL) {
        return NGX_ERROR;
    }

    ngx_log_error(NGX_LOG_DEBUG, r->connection->log, 0,
            "ngx_http_ajp_process_header: state(%d)", a->state);

    u = r->upstream;

    msg = ajp_msg_reuse(&a->msg);
        
    buf = msg->buf = &u->buffer;

    while (buf->pos < buf->last) {

        ngx_log_error(NGX_LOG_DEBUG, r->connection->log, 0,
                "ngx_http_ajp_process_header: parse response, pos:%p, last:%p", 
                buf->pos, buf->last);

        /* save the position for returning NGX_AGAIN */
        pos = buf->pos;

        if (ngx_buf_size(msg->buf) < AJP_HEADER_LEN + 1) {

            if (buf->last == buf->end) {
                ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                        "ngx_http_ajp_process_header: too small buffer for the ajp packet.\n");
                return NGX_ERROR;
            }

            /*
             * The first buffer, there should be have 
             * enough buffer room, so I do't save it.
             * */
            return NGX_AGAIN;
        }

        ajp_parse_begin(msg);
        type = ajp_parse_type(msg);

        switch (type) {
            case CMD_AJP13_GET_BODY_CHUNK:

                /* just move the buffer's postion */
                ajp_msg_get_uint8(msg, (u_char *)&type);
                rc = ajp_msg_get_uint16(msg, &length);
                if (rc == AJP_EOVERFLOW) {
                    buf->pos = pos;
                    return NGX_AGAIN;
                }

                rc = ngx_http_upstream_send_request_body(r, u);
                if (rc != NGX_OK) {
                    return rc;
                }

                break;

            case CMD_AJP13_SEND_HEADERS:

                ajp_msg_get_uint8(msg, (u_char *)&type);
                rc = ajp_unmarshal_response(msg, r, alcf);

                if (rc == NGX_OK) {
                    a->state = ngx_http_ajp_st_response_parse_headers_done;
                    return NGX_OK;
                }
                else if (rc == AJP_EOVERFLOW) {
                    /* 
                     * It's an uncomplete AJP packet, move back to the header of packet, 
                     * and parse the header again in next call
                     * */
                    buf->pos = pos;
                    a->state = ngx_http_ajp_st_response_recv_headers;
                }
                else {
                    return  NGX_ERROR;
                }

                break;

            case CMD_AJP13_SEND_BODY_CHUNK:
                buf->pos = pos;
                a->state = ngx_http_ajp_st_response_body_data_sending;

                /* input_filter function will process these data */
                return NGX_OK;

                break;

            case CMD_AJP13_END_RESPONSE:
                ajp_msg_get_uint8(msg, &type);
                rc = ajp_msg_get_uint8(msg, &reuse);
                if (rc == AJP_EOVERFLOW) {
                    buf->pos = pos;
                    return NGX_AGAIN;
                }

                ngx_http_ajp_end_response(a, u->pipe, reuse);

                buf->last_buf = 1;
                return NGX_OK;

                break;

            default:
                break;
        }
    }

    return NGX_AGAIN;
}


static ngx_int_t
ngx_http_upstream_send_request_body(ngx_http_request_t *r, ngx_http_upstream_t *u)
{
    ngx_int_t                     rc;
    ngx_chain_t                  *cl;
    ajp_msg_t                    *msg;
    ngx_connection_t             *c;
    ngx_http_ajp_ctx_t           *a;
    ngx_http_ajp_loc_conf_t      *alcf;

    c = u->peer.connection;

    a = ngx_http_get_module_ctx(r, ngx_http_ajp_module);
    alcf = ngx_http_get_module_loc_conf(r, ngx_http_ajp_module);

    if (a->state > ngx_http_ajp_st_request_body_data_sending) {
        ngx_log_error(NGX_LOG_WARN, r->connection->log, 0,
                "ngx_http_upstream_send_request_body: bad state(%d)", a->state);
    }

    cl = ajp_data_msg_send_body(r, alcf->max_ajp_data_packet_size_conf, &a->body);

    if (u->output.in == NULL && u->output.busy == NULL) {
        if (cl == NULL) {
            /*If there is no more data in the body (i.e. the servlet container is
              trying to read past the end of the body), the server will send back
              an "empty" packet, which is a body packet with a payload length of 0.
              (0x12,0x34,0x00,0x00) */
            msg = ajp_msg_reuse(&a->msg);

            if (ajp_alloc_data_msg(r->pool, msg) != NGX_OK) {
                return NGX_ERROR;
            }

            ajp_data_msg_end(msg, 0);

            cl = ngx_alloc_chain_link(r->pool);
            if (cl == NULL) {
                return NGX_ERROR;
            }

            cl->buf = msg->buf;
            cl->next = NULL;
        }
    }

    if (a->body) {
        a->state = ngx_http_ajp_st_request_body_data_sending;
    }
    else {
        a->state = ngx_http_ajp_st_request_send_all_done;
    }

    c->log->action = "sending request body again to upstream";

    rc = ngx_output_chain(&u->output, cl);

    if (rc == NGX_ERROR) {
        return NGX_ERROR;
    }

    if (c->write->timer_set) {
        ngx_del_timer(c->write);
    }

    if (rc == NGX_AGAIN) {
        ngx_add_timer(c->write, u->conf->send_timeout);

        if (ngx_handle_write_event(c->write, u->conf->send_lowat) != NGX_OK) {
            return NGX_ERROR;
        }

        u->write_event_handler = ngx_http_upstream_send_request_body_handler;

        return NGX_AGAIN;
    }

    /* rc == NGX_OK */

    if (c->tcp_nopush == NGX_TCP_NOPUSH_SET) {
        if (ngx_tcp_push(c->fd) == NGX_ERROR) {
            ngx_log_error(NGX_LOG_CRIT, c->log, ngx_socket_errno,
                    ngx_tcp_push_n " failed");
            return NGX_ERROR;
        }

        c->tcp_nopush = NGX_TCP_NOPUSH_UNSET;
    }

    ngx_add_timer(c->read, u->conf->read_timeout);

    if (ngx_handle_write_event(c->write, 0) != NGX_OK) {
        return NGX_ERROR;
    }

    u->write_event_handler = ngx_http_upstream_dummy_handler;

    return NGX_OK;
}


ngx_chain_t *
ajp_data_msg_send_body(ngx_http_request_t *r, size_t max_size, ngx_chain_t **body)
{
    size_t                    size;
    ngx_buf_t                *b_in, *b_out;
    ngx_chain_t              *out, *cl, *in;
    ajp_msg_t                *msg;
    ngx_http_ajp_ctx_t       *a;

    a = ngx_http_get_module_ctx(r, ngx_http_ajp_module);

    if (*body == NULL || a == NULL) {
        return NULL;
    }

    msg = ajp_msg_reuse(&a->msg);

    if (ajp_alloc_data_msg(r->pool, msg) != NGX_OK) {
        return NULL;
    }

    out = cl = ngx_alloc_chain_link(r->pool);
    if (cl == NULL) {
        return NULL;
    }

    cl->buf = msg->buf;

    max_size -= AJP_HEADER_SZ;
    size = 0;
    in = *body;

    b_out = NULL;
    while (in) {
        b_in = in->buf;

        b_out = ngx_alloc_buf(r->pool);
        if (b_out == NULL) {
            return NULL;
        }
        ngx_memcpy(b_out, b_in, sizeof(ngx_buf_t));

        if (b_in->in_file) {
            if ((size_t)(b_in->file_last - b_in->file_pos) <= (max_size - size)){
                b_out->file_pos = b_in->file_pos;
                b_out->file_last = b_in->file_pos = b_in->file_last;

                size += b_out->file_last - b_out->file_pos;
            }
            else if ((size_t)(b_in->file_last - b_in->file_pos) > (max_size - size)) {
                b_out->file_pos = b_in->file_pos;
                b_in->file_pos += max_size - size;
                b_out->file_last = b_in->file_pos;

                size += b_out->file_last - b_out->file_pos;
            }
        }
        else {
            if ((size_t)(b_in->last - b_in->pos) <= (max_size - size)){
                b_out->pos = b_in->pos;
                b_out->last = b_in->pos = b_in->last;

                size += b_out->last - b_out->pos;
            }
            else if ((size_t)(b_in->last - b_in->pos) > (max_size - size)) {
                b_out->pos = b_in->pos;
                b_in->pos += max_size - size;
                b_out->last = b_in->pos;

                size += b_out->last - b_out->pos;
            }
        }

        cl->next = ngx_alloc_chain_link(r->pool);
        if (cl->next == NULL) {
            return NULL;
        }

        cl = cl->next;
        cl->buf = b_out;

        if (size >= max_size) {
            break;
        }
        else {
            in = in->next;
        }
    }

    *body = in;
    cl->next = NULL;
    
    ajp_data_msg_end(msg, size);

    return out;
}


static void
ngx_http_upstream_send_request_body_handler(ngx_http_request_t *r,
    ngx_http_upstream_t *u)
{
    ngx_int_t rc;

    rc = ngx_http_upstream_send_request_body(r, u);

    if (rc == NGX_ERROR) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                "ngx_http_upstream_send_request_body error");
    }
}


static void
ngx_http_upstream_dummy_handler(ngx_http_request_t *r,
    ngx_http_upstream_t *u)
{
    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "ajp upstream dummy handler");
    return;
}


static ngx_int_t
ngx_http_ajp_input_filter(ngx_event_pipe_t *p, ngx_buf_t *buf)
{
    size_t                   size, offset;
    u_char                   reuse, type, save_used;
    ngx_int_t                rc;
    ngx_buf_t               *b, **prev, *sb;
    ajp_msg_t               *msg;
    ngx_chain_t             *cl;
    ngx_http_request_t      *r;
    ngx_http_ajp_ctx_t      *a;

    if (buf->pos == buf->last) {
        return NGX_OK;
    }

    r = p->input_ctx;
    a = ngx_http_get_module_ctx(r, ngx_http_ajp_module);

    ngx_log_error(NGX_LOG_DEBUG, r->connection->log, 0,
            "ngx_http_ajp_input_filter: state(%d)", a->state);

    b = NULL;
    prev = &buf->shadow;

    save_used = 0;
    while(buf->pos < buf->last) {

        /* This a new data packet */
        if (a->length == 0) {
            if (ngx_buf_size(buf) < (AJP_HEADER_LEN + 1)) {
                ngx_http_ajp_input_filter_save_tiny_buffer(r, buf);
                break;
            }

            msg = ajp_msg_reuse(&a->msg);

            if ((a->save != NULL) && ngx_buf_size(a->save->buf)) {
                sb = a->save->buf;
                size = sb->last - sb->pos;

                offset = size = AJP_HEADER_SAVE_SZ - size;
                if ((size_t)ngx_buf_size(buf) >= size) {
                    ngx_memcpy(sb->last, buf->pos, size);
                    sb->last += size;
                }
                else {
                    ngx_http_ajp_input_filter_save_tiny_buffer(r, buf);
                    break;
                }

                msg->buf = sb;
                save_used = 1;
            }
            else {
                msg->buf = buf;
                offset = 0;
            }

            ajp_parse_begin(msg);
            type = ajp_parse_type(msg);

            switch (type) {
                case CMD_AJP13_SEND_BODY_CHUNK:
                    a->state = ngx_http_ajp_st_response_body_data_sending;

                    ajp_msg_get_uint8(msg, &type);
                    rc = ajp_msg_get_uint16(msg, (uint16_t *)&a->length);
                    if (rc != NGX_OK) {
                        return NGX_ERROR;
                    }

                    buf->pos += offset;

                    break;

                case CMD_AJP13_END_RESPONSE:
                    ajp_msg_get_uint8(msg, &type);
                    ajp_msg_get_uint8(msg, &reuse);

                    ngx_http_ajp_end_response(a, p, reuse);
                    buf->pos = buf->last;

                    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, p->log, 0,
                            "input filter packet with End response, reuse:%d", reuse);

                    return NGX_OK;

                default:

                    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
                            "ngx_http_ajp_input_filter: bad_packet_type(%d), %s\n",
                            type, ajp_msg_dump(r->pool, msg, (u_char *)"bad type"));
                    return NGX_ERROR;
            }
        }

        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, p->log, 0,
                       "input filter packet, length:%z, buffer_size:%z",
                       a->length, ngx_buf_size(buf));

        if (p->free) {
            b = p->free->buf;
            p->free = p->free->next;

        } else {
            b = ngx_alloc_buf(p->pool);
            if (b == NULL) {
                return NGX_ERROR;
            }
        }

        ngx_memzero(b, sizeof(ngx_buf_t));

        b->pos = buf->pos;
        b->start = buf->start;
        b->end = buf->end;
        b->tag = p->tag;
        b->temporary = 1;
        b->recycled = 1;

        *prev = b;
        prev = &b->shadow;

        cl = ngx_alloc_chain_link(p->pool);
        if (cl == NULL) {
            return NGX_ERROR;
        }

        cl->buf = b;
        cl->next = NULL;

        if (p->in) {
            *p->last_in = cl;
        } else {
            p->in = cl;
        }
        p->last_in = &cl->next;

        /* STUB */ b->num = buf->num;

        ngx_log_debug2(NGX_LOG_DEBUG_EVENT, p->log, 0,
                       "input buf #%d %p", b->num, b->pos);

        if (buf->pos + a->length < buf->last) {
            buf->pos += a->length;
            b->last = buf->pos;

            /* The last byte of this message always seems to be
               0x00 and is not part of the chunk. */
            if (buf->pos < buf->last) {
                buf->pos++;
            }

            a->length = 0;
        }
        else {
            a->length -= buf->last - buf->pos;
            buf->pos = buf->last;
            b->last = buf->last;
            break;
        }
    }

    if (save_used) {
        sb = a->save->buf;
        sb->last = sb->pos = sb->start; 
    }

    ngx_log_debug2(NGX_LOG_DEBUG_EVENT, p->log, 0,
            "free buf %p %z", buf->pos, ngx_buf_size(buf));

    if (b) {
        b->shadow = buf;
        b->last_shadow = 1;

        ngx_log_debug2(NGX_LOG_DEBUG_EVENT, p->log, 0,
                       "input buf %p %z", b->pos, ngx_buf_size(buf));

        return NGX_OK;
    }

    /* there is no data record in the buf, add it to free chain */

    if (ngx_event_pipe_add_free_buf(p, buf) != NGX_OK) {
        return NGX_ERROR;
    }

    return NGX_OK;
}


/* 
 * Save too tiny buffer which even does not contain the packet's
 * length. Concatenate it with next buffer. 
 */
static ngx_int_t
ngx_http_ajp_input_filter_save_tiny_buffer(ngx_http_request_t *r,
    ngx_buf_t *buf)
{
    size_t                   size;
    ngx_buf_t               *sb;
    ngx_http_ajp_ctx_t      *a;

    a = ngx_http_get_module_ctx(r, ngx_http_ajp_module);

    /* no buffer space any more */
    if (buf->last == buf->end) {
        if (a->save == NULL) {
            a->save = ngx_alloc_chain_link(r->pool);
            if (a->save == NULL) {
                return NGX_ERROR;
            }

            sb = ngx_create_temp_buf(r->pool, AJP_HEADER_SAVE_SZ);
            if (sb == NULL) {
                return NGX_ERROR;
            }

            a->save->buf = sb;
            a->save->next = NULL;
        }

        sb = a->save->buf;

        size = buf->last - buf->pos;
        ngx_memcpy(sb->last, buf->pos, size);

        sb->last += size;
        buf->pos += size;
    }

    return NGX_OK;
}

static void
ngx_http_ajp_end_response(ngx_http_ajp_ctx_t *a, ngx_event_pipe_t *p, int reuse) 
{
    a->ajp_reuse = reuse;
    p->keepalive = reuse ? 1 : 0;
    p->upstream_done = 1;
    a->state = ngx_http_ajp_st_response_end;
}

static void
ngx_http_ajp_abort_request(ngx_http_request_t *r)
{
    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "abort http ajp request");
    return;
}


static void
ngx_http_ajp_finalize_request(ngx_http_request_t *r, ngx_int_t rc)
{
    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                   "finalize http ajp request");

    return;
}

