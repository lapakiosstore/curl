/***************************************************************************
 *                                  _   _ ____  _
 *  Project                     ___| | | |  _ \| |
 *                             / __| | | | |_) | |
 *                            | (__| |_| |  _ <| |___
 *                             \___|\___/|_| \_\_____|
 *
 * Copyright (C) 1998 - 2019, Daniel Stenberg, <daniel@haxx.se>, et al.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution. The terms
 * are also available at https://curl.haxx.se/docs/copyright.html.
 *
 * You may opt to use, copy, modify, merge, publish, distribute and/or sell
 * copies of the Software, and permit persons to whom the Software is
 * furnished to do so, under the terms of the COPYING file.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ***************************************************************************/

#include "curl_setup.h"

#ifdef USE_QUICHE
#include <quiche.h>
#include <openssl/err.h>
#include "urldata.h"
#include "sendf.h"
#include "strdup.h"
#include "rand.h"
#include "quic.h"
#include "strcase.h"
#include "multiif.h"

/* The last 3 #include files should be in this order */
#include "curl_printf.h"
#include "curl_memory.h"
#include "memdebug.h"

#define DEBUG_HTTP3
#ifdef DEBUG_HTTP3
#define H3BUGF(x) x
#else
#define H3BUGF(x) do { } WHILE_FALSE
#endif

#define QUIC_MAX_STREAMS (256*1024)
#define QUIC_MAX_DATA (1*1024*1024)
#define QUIC_IDLE_TIMEOUT 60 * 1000 /* milliseconds */

static CURLcode process_ingress(struct connectdata *conn,
                                curl_socket_t sockfd);

static CURLcode flush_egress(struct connectdata *conn, curl_socket_t sockfd);

static CURLcode http_request(struct connectdata *conn, const void *mem,
                             size_t len);
static Curl_recv h3_stream_recv;
static Curl_send h3_stream_send;


static int quiche_getsock(struct connectdata *conn, curl_socket_t *socks)
{
  struct SingleRequest *k = &conn->data->req;
  int bitmap = GETSOCK_BLANK;

  socks[0] = conn->sock[FIRSTSOCKET];

  /* in a HTTP/2 connection we can basically always get a frame so we should
     always be ready for one */
  bitmap |= GETSOCK_READSOCK(FIRSTSOCKET);

  /* we're still uploading or the HTTP/2 layer wants to send data */
  if((k->keepon & (KEEP_SEND|KEEP_SEND_PAUSE)) == KEEP_SEND)
    bitmap |= GETSOCK_WRITESOCK(FIRSTSOCKET);

  return bitmap;
}

static int quiche_perform_getsock(const struct connectdata *conn,
                                  curl_socket_t *socks)
{
  return quiche_getsock((struct connectdata *)conn, socks);
}

static CURLcode quiche_disconnect(struct connectdata *conn,
                                  bool dead_connection)
{
  (void)conn;
  (void)dead_connection;
  return CURLE_OK;
}

static unsigned int quiche_conncheck(struct connectdata *conn,
                                     unsigned int checks_to_perform)
{
  (void)conn;
  (void)checks_to_perform;
  return CONNRESULT_NONE;
}

static const struct Curl_handler Curl_handler_h3_quiche = {
  "HTTPS",                              /* scheme */
  ZERO_NULL,                            /* setup_connection */
  Curl_http,                            /* do_it */
  Curl_http_done,                       /* done */
  ZERO_NULL,                            /* do_more */
  ZERO_NULL,                            /* connect_it */
  ZERO_NULL,                            /* connecting */
  ZERO_NULL,                            /* doing */
  quiche_getsock,                       /* proto_getsock */
  quiche_getsock,                       /* doing_getsock */
  ZERO_NULL,                            /* domore_getsock */
  quiche_perform_getsock,                       /* perform_getsock */
  quiche_disconnect,                    /* disconnect */
  ZERO_NULL,                            /* readwrite */
  quiche_conncheck,                     /* connection_check */
  PORT_HTTP,                            /* defport */
  CURLPROTO_HTTPS,                      /* protocol */
  PROTOPT_SSL | PROTOPT_STREAM          /* flags */
};

CURLcode Curl_quic_connect(struct connectdata *conn, curl_socket_t sockfd,
                           const struct sockaddr *addr, socklen_t addrlen)
{
  CURLcode result;
  struct quicsocket *qs = &conn->quic;
  (void)addr;
  (void)addrlen;

  infof(conn->data, "Connecting socket %d over QUIC\n", sockfd);

  qs->cfg = quiche_config_new(QUICHE_PROTOCOL_VERSION);
  if(!qs->cfg)
    return CURLE_FAILED_INIT; /* TODO: better return code */

  quiche_config_set_idle_timeout(qs->cfg, QUIC_IDLE_TIMEOUT);
  quiche_config_set_initial_max_data(qs->cfg, QUIC_MAX_DATA);
  quiche_config_set_initial_max_stream_data_bidi_local(qs->cfg, QUIC_MAX_DATA);
  quiche_config_set_initial_max_stream_data_bidi_remote(qs->cfg,
                                                        QUIC_MAX_DATA);
  quiche_config_set_initial_max_stream_data_uni(qs->cfg, QUIC_MAX_DATA);
  quiche_config_set_initial_max_streams_bidi(qs->cfg, QUIC_MAX_STREAMS);
  quiche_config_set_initial_max_streams_uni(qs->cfg, QUIC_MAX_STREAMS);
  quiche_config_set_application_protos(qs->cfg,
                                       (uint8_t *)
                                       QUICHE_H3_APPLICATION_PROTOCOL,
                                       sizeof(QUICHE_H3_APPLICATION_PROTOCOL)
                                       - 1);

  result = Curl_rand(conn->data, qs->scid, sizeof(qs->scid));
  if(result)
    return result;

  qs->conn = quiche_connect(conn->host.name, (const uint8_t *) qs->scid,
                            sizeof(qs->scid), qs->cfg);
  if(!qs->conn)
    return CURLE_FAILED_INIT; /* TODO: better return code */

  result = flush_egress(conn, sockfd);
  if(result)
    return CURLE_FAILED_INIT; /* TODO: better return code */

  infof(conn->data, "Sent QUIC client Initial, ALPN: %s\n",
        QUICHE_H3_APPLICATION_PROTOCOL + 1);

  return CURLE_OK;
}

CURLcode Curl_quic_is_connected(struct connectdata *conn, int sockindex,
                                bool *done)
{
  CURLcode result;
  struct quicsocket *qs = &conn->quic;
  curl_socket_t sockfd = conn->sock[sockindex];

  result = process_ingress(conn, sockfd);
  if(result)
    return result;

  result = flush_egress(conn, sockfd);
  if(result)
    return result;

  if(quiche_conn_is_established(qs->conn)) {
    conn->recv[sockindex] = h3_stream_recv;
    conn->send[sockindex] = h3_stream_send;
    *done = TRUE;
    conn->handler = &Curl_handler_h3_quiche;
    DEBUGF(infof(conn->data, "quiche established connection!\n"));
  }

  return CURLE_OK;
}

static CURLcode process_ingress(struct connectdata *conn, int sockfd)
{
  ssize_t recvd;
  struct quicsocket *qs = &conn->quic;
  static uint8_t buf[65535];

  do {
    recvd = recv(sockfd, buf, sizeof(buf), 0);
    if((recvd < 0) && ((errno == EAGAIN) || (errno == EWOULDBLOCK)))
      break;

    if(recvd < 0) {
      failf(conn->data, "quiche: recv() unexpectedly returned %d", recvd);
      return CURLE_RECV_ERROR;
    }

    recvd = quiche_conn_recv(qs->conn, buf, recvd);
    if(recvd == QUICHE_ERR_DONE)
      break;

    if(recvd < 0) {
      failf(conn->data, "quiche_conn_recv() == %d", recvd);
      return CURLE_RECV_ERROR;
    }
  } while(1);

  return CURLE_OK;
}

static CURLcode flush_egress(struct connectdata *conn, int sockfd)
{
  ssize_t sent;
  struct quicsocket *qs = &conn->quic;
  static uint8_t out[1200];

  do {
    sent = quiche_conn_send(qs->conn, out, sizeof(out));
    if(sent == QUICHE_ERR_DONE)
      break;

    if(sent < 0)
      return CURLE_SEND_ERROR;

    sent = send(sockfd, out, sent, 0);
    if(sent < 0)
      return CURLE_SEND_ERROR;
  } while(1);

  return CURLE_OK;
}

static int cb_each_header(uint8_t *name, size_t name_len,
                          uint8_t *value, size_t value_len,
                          void *argp)
{
  (void)argp;
  fprintf(stderr, "got HTTP header: %.*s=%.*s\n",
          (int) name_len, name, (int) value_len, value);
  return 0;
}

static ssize_t h3_stream_recv(struct connectdata *conn,
                              int sockindex,
                              char *buf,
                              size_t buffersize,
                              CURLcode *curlcode)
{
  bool fin;
  ssize_t recvd;
  struct quicsocket *qs = &conn->quic;
  curl_socket_t sockfd = conn->sock[sockindex];
  quiche_h3_event *ev;
  int rc;

  if(process_ingress(conn, sockfd)) {
    *curlcode = CURLE_RECV_ERROR;
    return -1;
  }

  recvd = quiche_conn_stream_recv(qs->conn, 0, (uint8_t *) buf, buffersize,
                                  &fin);
  if(recvd == QUICHE_ERR_DONE) {
    *curlcode = CURLE_AGAIN;
    return -1;
  }

  infof(conn->data, "%zd bytes of H3 to deal with\n", recvd);

  while(1) {
    int64_t s = quiche_h3_conn_poll(qs->h3c, qs->conn, &ev);
    if(s < 0)
      /* nothing more to do */
      break;

    switch(quiche_h3_event_type(ev)) {
    case QUICHE_H3_EVENT_HEADERS:
      rc = quiche_h3_event_for_each_header(ev, cb_each_header, NULL);
      if(rc) {
        fprintf(stderr, "failed to process headers");
        /* what do we do about this? */
      }
      break;
    case QUICHE_H3_EVENT_DATA:
      recvd = quiche_h3_recv_body(qs->h3c, qs->conn, s, (unsigned char *)buf,
                                  buffersize);
      if(recvd <= 0) {
        break;
      }
      break;

    case QUICHE_H3_EVENT_FINISHED:
      if(quiche_conn_close(qs->conn, true, 0, NULL, 0) < 0) {
        fprintf(stderr, "failed to close connection\n");
      }
      break;
    }

    quiche_h3_event_free(ev);
  }

  *curlcode = CURLE_OK;
  return recvd;
}

static ssize_t h3_stream_send(struct connectdata *conn,
                              int sockindex,
                              const void *mem,
                              size_t len,
                              CURLcode *curlcode)
{
  ssize_t sent;
  struct quicsocket *qs = &conn->quic;
  curl_socket_t sockfd = conn->sock[sockindex];

  if(!qs->h3c) {
    CURLcode result = http_request(conn, mem, len);
    if(result) {
      *curlcode = CURLE_SEND_ERROR;
      return -1;
    }
    return len;
  }
  else {
    sent = quiche_conn_stream_send(qs->conn, 0, mem, len, true);
    if(sent < 0) {
      *curlcode = CURLE_SEND_ERROR;
      return -1;
    }
  }

  if(flush_egress(conn, sockfd)) {
    *curlcode = CURLE_SEND_ERROR;
    return -1;
  }

  *curlcode = CURLE_OK;
  return sent;
}

/*
 * Store quiche version info in this buffer, Prefix with a space.  Return total
 * length written.
 */
int Curl_quic_ver(char *p, size_t len)
{
  return msnprintf(p, len, " quiche");
}

/* Index where :authority header field will appear in request header
   field list. */
#define AUTHORITY_DST_IDX 3

static CURLcode http_request(struct connectdata *conn, const void *mem,
                             size_t len)
{
  /*
   */
  struct HTTP *stream = conn->data->req.protop;
  size_t nheader;
  size_t i;
  size_t authority_idx;
  char *hdbuf = (char *)mem;
  char *end, *line_end;
  int64_t stream3_id;
  quiche_h3_header *nva = NULL;
  struct quicsocket *qs = &conn->quic;

  qs->config = quiche_h3_config_new(0, 1024, 0, 0);
  /* TODO: handle failure */

  /* Create a new HTTP/3 connection on the QUIC connection. */
  qs->h3c = quiche_h3_conn_new_with_transport(qs->conn, qs->config);
  /* TODO: handle failure */

  /* Calculate number of headers contained in [mem, mem + len). Assumes a
     correctly generated HTTP header field block. */
  nheader = 0;
  for(i = 1; i < len; ++i) {
    if(hdbuf[i] == '\n' && hdbuf[i - 1] == '\r') {
      ++nheader;
      ++i;
    }
  }
  if(nheader < 2)
    goto fail;

  /* We counted additional 2 \r\n in the first and last line. We need 3
     new headers: :method, :path and :scheme. Therefore we need one
     more space. */
  nheader += 1;
  nva = malloc(sizeof(quiche_h3_header) * nheader);
  if(!nva)
    return CURLE_OUT_OF_MEMORY;

  /* Extract :method, :path from request line
     We do line endings with CRLF so checking for CR is enough */
  line_end = memchr(hdbuf, '\r', len);
  if(!line_end)
    goto fail;

  /* Method does not contain spaces */
  end = memchr(hdbuf, ' ', line_end - hdbuf);
  if(!end || end == hdbuf)
    goto fail;
  nva[0].name = (unsigned char *)":method";
  nva[0].name_len = strlen((char *)nva[0].name);
  nva[0].value = (unsigned char *)hdbuf;
  nva[0].value_len = (size_t)(end - hdbuf);

  hdbuf = end + 1;

  /* Path may contain spaces so scan backwards */
  end = NULL;
  for(i = (size_t)(line_end - hdbuf); i; --i) {
    if(hdbuf[i - 1] == ' ') {
      end = &hdbuf[i - 1];
      break;
    }
  }
  if(!end || end == hdbuf)
    goto fail;
  nva[1].name = (unsigned char *)":path";
  nva[1].name_len = strlen((char *)nva[1].name);
  nva[1].value = (unsigned char *)hdbuf;
  nva[1].value_len = (size_t)(end - hdbuf);

  nva[2].name = (unsigned char *)":scheme";
  nva[2].name_len = strlen((char *)nva[2].name);
  if(conn->handler->flags & PROTOPT_SSL)
    nva[2].value = (unsigned char *)"https";
  else
    nva[2].value = (unsigned char *)"http";
  nva[2].value_len = strlen((char *)nva[2].value);


  authority_idx = 0;
  i = 3;
  while(i < nheader) {
    size_t hlen;

    hdbuf = line_end + 2;

    /* check for next CR, but only within the piece of data left in the given
       buffer */
    line_end = memchr(hdbuf, '\r', len - (hdbuf - (char *)mem));
    if(!line_end || (line_end == hdbuf))
      goto fail;

    /* header continuation lines are not supported */
    if(*hdbuf == ' ' || *hdbuf == '\t')
      goto fail;

    for(end = hdbuf; end < line_end && *end != ':'; ++end)
      ;
    if(end == hdbuf || end == line_end)
      goto fail;
    hlen = end - hdbuf;

    if(hlen == 4 && strncasecompare("host", hdbuf, 4)) {
      authority_idx = i;
      nva[i].name = (unsigned char *)":authority";
      nva[i].name_len = strlen((char *)nva[i].name);
    }
    else {
      nva[i].name = (unsigned char *)hdbuf;
      nva[i].name_len = (size_t)(end - hdbuf);
    }
    hdbuf = end + 1;
    while(*hdbuf == ' ' || *hdbuf == '\t')
      ++hdbuf;
    end = line_end;

#if 0 /* This should probably go in more or less like this */
    switch(inspect_header((const char *)nva[i].name, nva[i].namelen, hdbuf,
                          end - hdbuf)) {
    case HEADERINST_IGNORE:
      /* skip header fields prohibited by HTTP/2 specification. */
      --nheader;
      continue;
    case HEADERINST_TE_TRAILERS:
      nva[i].value = (uint8_t*)"trailers";
      nva[i].value_len = sizeof("trailers") - 1;
      break;
    default:
      nva[i].value = (unsigned char *)hdbuf;
      nva[i].value_len = (size_t)(end - hdbuf);
    }
#endif
    nva[i].value = (unsigned char *)hdbuf;
    nva[i].value_len = (size_t)(end - hdbuf);

    ++i;
  }

  /* :authority must come before non-pseudo header fields */
  if(authority_idx != 0 && authority_idx != AUTHORITY_DST_IDX) {
    quiche_h3_header authority = nva[authority_idx];
    for(i = authority_idx; i > AUTHORITY_DST_IDX; --i) {
      nva[i] = nva[i - 1];
    }
    nva[i] = authority;
  }

  /* Warn stream may be rejected if cumulative length of headers is too
     large. */
#define MAX_ACC 60000  /* <64KB to account for some overhead */
  {
    size_t acc = 0;

    for(i = 0; i < nheader; ++i) {
      acc += nva[i].name_len + nva[i].value_len;

      H3BUGF(infof(conn->data, "h3 [%.*s: %.*s]\n",
                   nva[i].name_len, nva[i].name,
                   nva[i].value_len, nva[i].value));
    }

    if(acc > MAX_ACC) {
      infof(conn->data, "http_request: Warning: The cumulative length of all "
            "headers exceeds %zu bytes and that could cause the "
            "stream to be rejected.\n", MAX_ACC);
    }
  }

  switch(conn->data->set.httpreq) {
  case HTTPREQ_POST:
  case HTTPREQ_POST_FORM:
  case HTTPREQ_POST_MIME:
  case HTTPREQ_PUT:
    if(conn->data->state.infilesize != -1)
      stream->upload_left = conn->data->state.infilesize;
    else
      /* data sending without specifying the data amount up front */
      stream->upload_left = -1; /* unknown, but not zero */

    /* fix the body submission */
    break;
  default:
    stream3_id = quiche_h3_send_request(qs->h3c, qs->conn, nva, nheader,
                                        TRUE);
    break;
  }

  Curl_safefree(nva);

  if(stream3_id < 0) {
    H3BUGF(infof(conn->data, "http3_send() send error\n"));
    return CURLE_SEND_ERROR;
  }

  infof(conn->data, "Using HTTP/3 Stream ID: %x (easy handle %p)\n",
        stream3_id, (void *)conn->data);
  stream->stream3_id = stream3_id;

  return CURLE_OK;

fail:
  free(nva);
  return CURLE_SEND_ERROR;
}


#endif
