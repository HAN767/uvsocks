/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
   vim: set autoindent expandtab shiftwidth=2 softtabstop=2 tabstop=2: */
/*
 * uvsocks.c
 *
 * Copyright (c) 2017 EMSTONE, All rights reserved.
 */

#include "uvsocks.h"
#include "aqueue.h"
#include <uv.h>
#include <glib.h>
#include <stdio.h>
#include <memory.h>
#include <string.h>
#include <limits.h>

/* using edc_aqueue_try_pop, UVSOCKS_LOOP is 1, but 0 */
#define UVSOCKS_LOOP 0

#define UVSOCKS_BUF_MAX (1024 * 1024)

#ifndef UV_BUF_LEN
#ifdef _WIN32
#define UV_BUF_LEN(x) ((ULONG)(x))
#else
#define UV_BUF_LEN(x) ((size_t)(x))
#endif
#endif

typedef enum _UvSocksVersion
{
  UVSOCKS_VER_5       = 0x05,
} UvSocksVersion;

typedef enum _UvSocksAuthMethod
{
  UVSOCKS_AUTH_NONE   = 0x00,
  UVSOCKS_AUTH_GSSAPI = 0x01,
  UVSOCKS_AUTH_PASSWD = 0x02,
} UvSocksAuthMethod;

typedef enum _UvSocksAuthResult
{
  UVSOCKS_AUTH_ALLOW = 0x00,
  UVSOCKS_AUTH_DENY  = 0x01,
} UvSocksAuthResult;

typedef enum _UvSocksAddrType
{
  UVSOCKS_ADDR_TYPE_IPV4 = 0x01,
  UVSOCKS_ADDR_TYPE_HOST = 0x03,
  UVSOCKS_ADDR_TYPE_IPV6 = 0x04,
} UvSocksAddrType;

typedef enum _UvSocksCmd
{
  UVSOCKS_CMD_CONNECT       = 0x01,
  UVSOCKS_CMD_BIND          = 0x02,
  UVSOCKS_CMD_UDP_ASSOCIATE = 0x03,
} UvSocksCmd;

typedef enum _UvSocksStage
{
  UVSOCKS_STAGE_NONE                = 0x00,
  UVSOCKS_STAGE_CONNECTED           = 0x01,
  UVSOCKS_STAGE_AUTHENTICATE        = 0x02,
  UVSOCKS_STAGE_AUTHENTICATED       = 0x03,
  UVSOCKS_STAGE_ESTABLISH           = 0x04,
  UVSOCKS_STAGE_ESTABLISHED         = 0x05,
  UVSOCKS_STAGE_TUNNEL              = 0x06,
  UVSOCKS_STAGE_BIND                = 0x07,
  UVSOCKS_STAGE_TUNNELED            = 0x08,
} UvSocksStage;

typedef struct _UvSocksContext UvSocksContext;

typedef struct _UvSocksPoll UvSocksPoll;
struct _UvSocksPoll
{
  UvSocksContext     *context;
  uv_poll_t           handle;
  uv_os_sock_t        sock;
  size_t              read;
  char               *buf;
};

typedef struct _UvSocksForward UvSocksForward;
struct _UvSocksForward
{
  UvSocks           *uvsocks;
  UvSocksForward    *prev;
  UvSocksForward    *next;

  char              *listen_host;
  int                listen_port;
  char              *listen_path;
  char              *remote_host;
  int                remote_port;
  char              *remote_path;

  UvSocksCmd         command;
  UvSocksPoll       *server;
  UvSocksForwardFunc callback_func;
  void              *callback_data;
};

struct _UvSocksContext
{
  UvSocks          *uvsocks;
  UvSocksForward   *forward;
  UvSocksContext   *prev;
  UvSocksContext   *next;

  UvSocksStage      stage;
  uv_mutex_t        mutex;
  UvSocksPoll      *remote;
  UvSocksPoll      *local;
};

typedef struct _UvSocks UvSocks;
struct _UvSocks
{
  uv_loop_t              loop;
  AQueue                *queue;
  uv_async_t             async;
  uv_thread_t            thread;

  char                  *host;
  int                    port;
  char                  *user;
  char                  *password;

  UvSocksForward        *forwards;
  UvSocksForward        *reverse_forwards;
  UvSocksContext        *contexts;

  UvSocksTunnelFunc      callback_func;
  void                  *callback_data;
};

typedef struct _UvSocksPacketReq UvSocksPacketReq;
struct _UvSocksPacketReq
{
  uv_write_t req;
  uv_buf_t   buf;
};

typedef void (*UvSocksFunc) (UvSocks *uvsocks,
                             void    *data);

typedef struct _UvSocksMessage UvSocksMessage;
struct _UvSocksMessage
{
  UvSocksFunc   func;
  void         *data;
  void        (*destroy_data) (void *data);
};

typedef void (*UvSocksDnsResolveFunc) (UvSocksContext   *context,
                                       struct addrinfo  *resolved);
typedef struct _UvSocksDnsResolve UvSocksDnsResolve;
struct _UvSocksDnsResolve
{
  UvSocksDnsResolveFunc func;
  void                 *data;
};

static void
uvsocks_add_forward0 (UvSocks        *uvsocks,
                      UvSocksForward *forward)
{
  forward->uvsocks = uvsocks;

  if (!uvsocks->forwards)
    uvsocks->forwards = forward;
  else
    {
      forward->next = uvsocks->forwards;
      uvsocks->forwards->prev = forward;
      uvsocks->forwards = forward;
    }
}

static void
uvsocks_remove_forward (UvSocks        *uvsocks,
                        UvSocksForward *forward)
{
  if (forward->next)
    forward->next->prev = forward->prev;
  if (forward->prev)
    forward->prev->next = forward->next;
  if (forward == uvsocks->forwards)
    uvsocks->forwards = forward->next;

  g_free (forward->listen_host);
  g_free (forward->listen_path);
  g_free (forward->remote_host);
  g_free (forward->remote_path);

  g_free (forward);
}

static void
uvsocks_free_forward (UvSocks *uvsocks)
{
  if (!uvsocks->forwards)
    return;

  while (uvsocks->forwards)
    uvsocks_remove_forward (uvsocks, uvsocks->forwards);
}

static void
uvsocks_add_reverse_forward0 (UvSocks        *uvsocks,
                              UvSocksForward *forward)
{
  forward->uvsocks = uvsocks;

  if (!uvsocks->reverse_forwards)
    uvsocks->reverse_forwards = forward;
  else
    {
      forward->next = uvsocks->reverse_forwards;
      uvsocks->reverse_forwards->prev = forward;
      uvsocks->reverse_forwards = forward;
    }
}

static void
uvsocks_remove_reverse_forward (UvSocks        *uvsocks,
                                UvSocksForward *forward)
{
  if (forward->next)
    forward->next->prev = forward->prev;
  if (forward->prev)
    forward->prev->next = forward->next;
  if (forward == uvsocks->reverse_forwards)
    uvsocks->reverse_forwards = forward->next;

  g_free (forward->listen_host);
  g_free (forward->listen_path);
  g_free (forward->remote_host);
  g_free (forward->remote_path);

  g_free (forward);
}

static void
uvsocks_free_reverse_forward (UvSocks *uvsocks)
{
  if (!uvsocks->reverse_forwards)
    return;

  while (uvsocks->reverse_forwards)
    uvsocks_remove_reverse_forward (uvsocks, uvsocks->reverse_forwards);
}

static void
uvsocks_receive_async (uv_async_t *handle)
{
  UvSocks *uvsocks = handle->data;

  while (1)
    {
      UvSocksMessage *msg;

      msg = aqueue_pop (uvsocks->queue);
      if (!msg)
        break;

      msg->func (uvsocks, msg->data);

      if (msg->destroy_data)
        msg->destroy_data (msg->data);
      free (msg);
    }
}

static void
uvsocks_send_async (UvSocks      *uvsocks,
                    UvSocksFunc   func,
                    void         *data,
                    void        (*destroy_data) (void *data))
{
  UvSocksMessage *msg;

  msg = malloc (sizeof (*msg));
  if (!msg)
    return;

  msg->func = func;
  msg->data = data;
  msg->destroy_data = destroy_data;
  aqueue_push (uvsocks->queue, msg);
  uv_async_send (&uvsocks->async);
}

void
uvset_thread_name (const char *name)
{
  char thread_name[17];

  snprintf (thread_name, sizeof (thread_name), "%s-%s", PACKAGE, name);
#ifdef linux
  prctl (PR_SET_NAME, (unsigned long) thread_name, 0, 0, 0);
#endif
}

static void
uvsocks_thread_main (void *arg)
{
  UvSocks *uvsocks = arg;

  uvset_thread_name ("uvsocks");

  uv_run (&uvsocks->loop, UV_RUN_DEFAULT);
}

UvSocks *
uvsocks_new (void)
{
  UvSocks *uvsocks;

  uvsocks = g_new0 (UvSocks, 1);
  if (!uvsocks)
    return NULL;

  int iret = uv_loop_init (&uvsocks->loop);
  uvsocks->queue = aqueue_new (128);
  uv_async_init (&uvsocks->loop, &uvsocks->async, uvsocks_receive_async);
  uvsocks->async.data = uvsocks;
  uv_thread_create (&uvsocks->thread, uvsocks_thread_main, uvsocks);

  return uvsocks;
}

static void
uvsocks_quit (UvSocks  *uvsocks,
              void     *data)
{
  uv_stop (&uvsocks->loop);
}

static void
uvsocks_add_context (UvSocks        *uvsocks,
                     UvSocksContext *context)
{
  context->uvsocks = uvsocks;

  if (!uvsocks->contexts)
    uvsocks->contexts = context;
  else
    {
      context->next = uvsocks->contexts;
      uvsocks->contexts->prev = context;
      uvsocks->contexts = context;
    }
}

static void
uvsocks_close_socket (uv_os_sock_t sock)
{
#ifdef _WIN32
  if (sock != INVALID_SOCKET)
    closesocket(sock);
#else
  if (sock >= 0)
    close(sock);
#endif
}

static void
uvsocks_free_poll (uv_handle_t *handle)
{
  UvSocksPoll *poll = handle->data;

  free (poll->buf);
  free (poll);
}

static void
uvsocks_destroy_poll (UvSocksPoll *poll)
{
  uv_poll_stop (&poll->handle);
  uvsocks_close_socket (poll->sock);
  uv_close ((uv_handle_t*) &poll->handle, uvsocks_free_poll);
}

static void
uvsocks_remove_context (UvSocks        *uvsocks,
                        UvSocksContext *context)
{
  if (context->next)
    context->next->prev = context->prev;
  if (context->prev)
    context->prev->next = context->next;
  if (context == uvsocks->contexts)
    uvsocks->contexts = context->next;

  if (uv_is_active ((uv_handle_t *) &context->remote->handle))
    {
      fprintf (stderr,
              "uvsocks:: uvsocks_destroy_poll remote\n");
      uvsocks_destroy_poll (context->remote);
    }

  if (uv_is_active ((uv_handle_t *) &context->local->handle))
    {
      fprintf (stderr,
              "uvsocks:: uvsocks_destroy_poll local\n");
      uvsocks_destroy_poll (context->local);
    }

  uv_mutex_destroy (&context->mutex);
  g_free (context);
}

static void
uvsocks_remote_set_stage (UvSocksContext *context,
                          UvSocksStage    stage)
{
  context->stage = stage;
}

static UvSocksContext *
uvsocks_create_context (UvSocksForward *forward)
{
  UvSocksContext *context;
  UvSocksPoll *local;
  UvSocksPoll *remote;

  context = g_new0 (UvSocksContext, 1);
  if (!context)
    return NULL;
  local =  malloc (sizeof (*local));
  if (!local)
    {
      g_free (context);
      return NULL;
    }
  local->buf = malloc (UVSOCKS_BUF_MAX);
  local->read = 0;
  local->context = context;
#ifdef _WIN32
  local->sock = INVALID_SOCKET;
#else
  local->sock = -1;
#endif
  local->handle.data = local;

  remote =  malloc (sizeof (*remote));
  if (!remote)
    {
      free (local);
      g_free (context);
      return NULL;
    }
  remote->buf = malloc (UVSOCKS_BUF_MAX);
  remote->read = 0;
  remote->context = context;
#ifdef _WIN32
  remote->sock = INVALID_SOCKET;
#else
  remote->sock = -1;
#endif
  remote->handle.data = remote;

  context->forward = forward;
  context->local = local;
  context->remote = remote;
  uv_mutex_init (&context->mutex);
  uvsocks_remote_set_stage (context, UVSOCKS_STAGE_NONE);

  return context;
}

static void
uvsocks_free_context (UvSocks *uvsocks)
{
  if (!uvsocks->contexts)
    return;

  while (uvsocks->contexts)
    uvsocks_remove_context (uvsocks, uvsocks->contexts);
}

void
uvsocks_free (UvSocks *uvsocks)
{
  uvsocks_send_async (uvsocks, uvsocks_quit, NULL, NULL);
  uv_thread_join (&uvsocks->thread);
  uv_close ((uv_handle_t *) &uvsocks->async, NULL);
  uv_loop_close (&uvsocks->loop);

  uvsocks_free_context (uvsocks);
  uvsocks_free_forward (uvsocks);
  uvsocks_free_reverse_forward (uvsocks);

  g_free (uvsocks->host);
  g_free (uvsocks->user);
  g_free (uvsocks->password);
  g_free (uvsocks);
}

void
uvsocks_add_forward (UvSocks           *uvsocks,
                     char              *listen_host,
                     int                listen_port,
                     char              *listen_path,
                     char              *remote_host,
                     int                remote_port,
                     char              *remote_path,
                     UvSocksForwardFunc callback_func,
                     void              *callback_data)
{
  UvSocksForward *forward;

  forward = g_new0 (UvSocksForward, 1);
  if (!forward)
    return;

  forward->command = UVSOCKS_CMD_CONNECT;
  forward->listen_host = g_strdup (listen_host);
  forward->listen_port = listen_port;
  forward->listen_path = g_strdup (listen_path);
  forward->remote_host = g_strdup (remote_host);
  forward->remote_port = remote_port;
  forward->remote_path = g_strdup (remote_path);

  forward->callback_func = callback_func;
  forward->callback_data = callback_data;

  fprintf (stderr,
          "Add forwarding -> "
          "listen host:%s:%d path:%s connect host:%s:%d path:%s\n",
           forward->listen_host,
           forward->listen_port,
           forward->listen_path,
           forward->remote_host,
           forward->remote_port,
           forward->remote_path);
  uvsocks_add_forward0 (uvsocks, forward);
}

void
uvsocks_add_reverse_forward (UvSocks           *uvsocks,
                             char              *listen_host,
                             int                listen_port,
                             char              *listen_path,
                             char              *remote_host,
                             int                remote_port,
                             char              *remote_path,
                             UvSocksForwardFunc callback_func,
                             void              *callback_data)
{
  UvSocksForward *forward;

  forward = g_new0 (UvSocksForward, 1);
  if (!forward)
    return;

  forward->command = UVSOCKS_CMD_BIND;
  forward->listen_host = g_strdup (listen_host);
  forward->listen_port = listen_port;
  forward->listen_path = g_strdup (listen_path);
  forward->remote_host = g_strdup (remote_host);
  forward->remote_port = remote_port;
  forward->remote_path = g_strdup (remote_path);

  forward->callback_func = callback_func;
  forward->callback_data = callback_data;

  fprintf (stderr,
          "Add reverse forwarding -> "
          "listen host:%s:%d path:%s connect host:%s:%d path:%s\n",
           forward->listen_host,
           forward->listen_port,
           forward->listen_path,
           forward->remote_host,
           forward->remote_port,
           forward->remote_path);
  uvsocks_add_reverse_forward0 (uvsocks, forward);
}

static int
uvsocks_set_nonblocking (uv_os_sock_t sock)
{
  int r;
#ifdef _WIN32
  unsigned long on = 1;
  r = ioctlsocket (sock, FIONBIO, &on);
  if (r)
    return 1;
#else
  int flags = fcntl (sock, F_GETFL, 0);
  if (flags < 0)
    return 1;
  r = fcntl (sock, F_SETFL, flags | O_NONBLOCK);
  if (r < 0)
    return 1;
#endif
  return 0;
}

static int
uvsocks_create_socket (uv_os_sock_t *sock)
{
  uv_os_sock_t s;

  s = socket (AF_INET, SOCK_STREAM, IPPROTO_IP);
#ifdef _WIN32
  if (s == INVALID_SOCKET)
    return 1;
#else
  if (s < 0)
    return 1;
#endif
  *sock = s;
  return 0;
}

static int
uvsocks_got_eagain (void)
{
#ifdef _WIN32
  return WSAGetLastError () == WSAEWOULDBLOCK;
#else
  return errno == EAGAIN
      || errno == EINPROGRESS
#ifdef EWOULDBLOCK
      || errno == EWOULDBLOCK;
#endif
      ;
#endif
}

static void
uvsocks_dns_resolved (uv_getaddrinfo_t  *resolver,
                      int                status,
                      struct addrinfo   *resolved)
{
  UvSocksDnsResolve *d = resolver->data;
  UvSocksContext *context = d->data;

  if (status < 0)
    {
      fprintf (stderr,
              "socks: failed to resolve dns name: %s\n",
               uv_strerror ((int) status));
      if (context->uvsocks->callback_func)
        context->uvsocks->callback_func (context->uvsocks,
                                         UVSOCKS_ERROR_DNS_RESOLVE,
                                         context->uvsocks->callback_data);

      uvsocks_remove_context (context->uvsocks, context);
      goto done;
    }

  if (d->func)
    d->func (context, resolved);

done:
  uv_freeaddrinfo (resolved);
  free (resolver);
  free (d);
}

static void
uvsocks_dns_resolve (UvSocks              *uvsocks,
                     char                 *host,
                     char                 *port,
                     UvSocksDnsResolveFunc func,
                     void                 *data)
{
  UvSocksContext *context = data;
  UvSocksDnsResolve *d;
  uv_getaddrinfo_t *resolver;
  struct addrinfo hints;
  int status;

  hints.ai_family = PF_INET; 
  hints.ai_socktype = SOCK_STREAM; 
  hints.ai_protocol = IPPROTO_TCP; 
  hints.ai_flags = 0;

  resolver = malloc (sizeof (*resolver));
  if (!resolver)
    return;
  d = malloc (sizeof (*d));
  if (!d)
    {
      free (resolver);
      g_free (port);
      return;
    }

  d->data = data;
  d->func = func;
  resolver->data = d;

#if UVSOCKS_LOOP
  status = uv_getaddrinfo (&context->uvsocks->loop,
#else
  status = uv_getaddrinfo (uv_default_loop (),
#endif
                           resolver,
                           uvsocks_dns_resolved,
                           host,
                           port,
                           &hints);
  if (status)
    {
      fprintf (stderr,
              "socks: failed getaddrinfo: %s\n",
               uv_err_name (status));
      if (uvsocks->callback_func)
        uvsocks->callback_func (uvsocks,
                                UVSOCKS_ERROR_DNS_ADDRINFO,
                                uvsocks->callback_data);

      uvsocks_remove_context (uvsocks, context);
      free (resolver);
    }
  g_free (port);
}

static int
uvsocks_write_packet (uv_os_sock_t sock,
                      char        *packet,
                      size_t       len)
{
  return send (sock, packet, (int) len, 0);
}

static int
uvsocks_read_packet (uv_os_sock_t sock,
                     char        *buffer,
                     size_t       read)
{
  return recv (sock, buffer, (int) read, 0);
}

static int
uvsocks_local_read_start (UvSocksContext *context);

static void
uvsocks_reverse_forward (UvSocks *uvsocks,
                         void    *data);

static void
uvsocks_remote_read (uv_poll_t *handle,
                     int        status,
                     int        events)
{
  UvSocksPoll *poll = handle->data;
  UvSocksContext *context = poll->context;
  UvSocks *uvsocks = poll->context->uvsocks;

  if (status < 0)
    {
      fprintf (stderr,
              "uvsocks:: failed poll remote read -> %s@%s:%d status:%d events:%d\n",
               uvsocks->user,
               uvsocks->host,
               uvsocks->port,
               status,
               events);
      if (uvsocks->callback_func)
        uvsocks->callback_func (uvsocks,
                                UVSOCKS_ERROR_POLL_REMOTE_READ,
                                uvsocks->callback_data);

      uvsocks_remove_context (uvsocks, poll->context);
      return;
    }

  if (events & UV_DISCONNECT)
    {
      fprintf (stderr,
              "uvsocks:: remote disconnected from %s@%s:%d status:%d events:%d\n",
               uvsocks->user,
               uvsocks->host,
               uvsocks->port,
               status,
               events);

      uvsocks_remove_context (uvsocks, poll->context);
      if (context->forward->command == UVSOCKS_CMD_BIND)
        uvsocks_send_async (uvsocks, uvsocks_reverse_forward, context->forward, NULL);
      return;
    }

  if (events & UV_WRITABLE)
    {
      switch (context->stage)
        {
          case UVSOCKS_STAGE_NONE:
          break;
          case UVSOCKS_STAGE_CONNECTED:
            {
              char packet[1024];
              size_t packet_size;

              uvsocks_remote_set_stage (context, UVSOCKS_STAGE_AUTHENTICATE);
              packet_size = 0;
              packet[packet_size++] = UVSOCKS_VER_5;
              packet[packet_size++] = 0x01;
              packet[packet_size++] = UVSOCKS_AUTH_PASSWD;

              uvsocks_write_packet (poll->sock, packet, 3);
            }
            break;
          case UVSOCKS_STAGE_AUTHENTICATE:
          break;
          case UVSOCKS_STAGE_AUTHENTICATED:
            {
              char packet[1024];
              size_t packet_size;
              size_t length;

              uvsocks_remote_set_stage (context, UVSOCKS_STAGE_ESTABLISH);
              packet_size = 0;
              packet[packet_size++] = 0x01;
              length = strlen (uvsocks->user);
              packet[packet_size++] = (char) length;
              memcpy (&packet[packet_size], uvsocks->user, length);
              packet_size += length;

              length = strlen (uvsocks->password);
              packet[packet_size++] = (char) length;
              memcpy (&packet[packet_size], uvsocks->password, length);
              packet_size += length;

              uvsocks_write_packet (poll->sock, packet, packet_size);
            }
            break;
          case UVSOCKS_STAGE_ESTABLISH:
          break;
          case UVSOCKS_STAGE_ESTABLISHED:
            {
              char packet[1024];
              size_t packet_size;
              unsigned short port;
              struct sockaddr_in addr;

              uvsocks_remote_set_stage (context, UVSOCKS_STAGE_TUNNEL);
              packet_size = 0;
              packet[packet_size++] = UVSOCKS_VER_5;
              packet[packet_size++] = context->forward->command;
              packet[packet_size++] = 0x00;
              packet[packet_size++] = UVSOCKS_ADDR_TYPE_IPV4;
              uv_ip4_addr (context->forward->remote_host,
                           context->forward->remote_port,
                          &addr);
              //addr = (const struct sockaddr_in *)resolved->ai_addr;
              memcpy (&packet[packet_size], &addr.sin_addr.S_un.S_addr, 4);
              packet_size += 4;
              port = htons (context->forward->remote_port);
              memcpy (&packet[packet_size], &port, 2);
              packet_size += 2;
              uvsocks_write_packet (poll->sock, packet, packet_size);
            }
            break;
          case UVSOCKS_STAGE_BIND:
            break;
          case UVSOCKS_STAGE_TUNNELED:
            {
              UvSocksPoll *local = poll->context->local;
              int sent;

              if (local->read > 0)
                {
                  uv_mutex_lock (&poll->context->mutex);
                  sent = uvsocks_write_packet (poll->sock,
                                               local->buf,
                                               local->read);
                  local->read -= sent;
                  uv_mutex_unlock (&poll->context->mutex);
                }
            }
            break;
        }
    }

  if ((events & UV_READABLE))
    {
      switch (context->stage)
        {
          case UVSOCKS_STAGE_NONE:
          break;
          case UVSOCKS_STAGE_CONNECTED:
          break;
          case UVSOCKS_STAGE_AUTHENTICATE:
            {
              poll->read += uvsocks_read_packet (poll->sock,
                                                &poll->buf[poll->read],
                                                 2);
              if (poll->read < 2)
                break;

              if (poll->buf[0] != UVSOCKS_VER_5 ||
                  poll->buf[1] != UVSOCKS_AUTH_PASSWD)
                break;

              poll->read -= 2;
              uvsocks_remote_set_stage (context, UVSOCKS_STAGE_AUTHENTICATED);
            }
            break;
          break;
          case UVSOCKS_STAGE_AUTHENTICATED:
            break;
          case UVSOCKS_STAGE_ESTABLISH:
            {
              poll->read += uvsocks_read_packet (poll->sock,
                                                &poll->buf[poll->read],
                                                 2);
              if (poll->read < 2)
                break;
              if (poll->buf[0] != 0x01 ||
                  poll->buf[1] != UVSOCKS_AUTH_ALLOW)
                break;

              poll->read -= 2;
              uvsocks_remote_set_stage (context, UVSOCKS_STAGE_ESTABLISHED);
            }
            break;
          case UVSOCKS_STAGE_ESTABLISHED:
            break;
          case UVSOCKS_STAGE_TUNNEL:
          case UVSOCKS_STAGE_BIND:
            {
              poll->read += uvsocks_read_packet (poll->sock,
                                                &poll->buf[poll->read],
                                                 10);
              if (poll->read < 10)
                break;
              if (poll->buf[0] != UVSOCKS_VER_5 ||
                  poll->buf[1] != 0)
                break;

              poll->read -= 10;

              if (context->stage == UVSOCKS_STAGE_TUNNEL &&
                  context->forward->command == UVSOCKS_CMD_BIND)
                {
                  int port;

                  memcpy (&port, &poll->buf[8], 2);
                  port = htons(port);

                  if (context->forward->callback_func)
                    context->forward->callback_func (uvsocks,
                                                     context->forward->remote_host,
                                                     context->forward->remote_port,
                                                     uvsocks->host,
                                                     port,
                                                     context->forward->callback_data);
                  uvsocks_remote_set_stage (context, UVSOCKS_STAGE_BIND);
                  break;
                }

              if (context->stage == UVSOCKS_STAGE_BIND &&
                  context->forward->command == UVSOCKS_CMD_BIND)
                {
                  struct sockaddr_in addr;
                  int r;

                  uv_ip4_addr (context->forward->remote_host,
                               context->forward->remote_port,
                              &addr);
                  if (uvsocks_create_socket (&context->local->sock))
                    break;

                  r = connect (context->local->sock, (struct sockaddr*) &addr, sizeof (addr));
                  if (r || uvsocks_got_eagain ())
                    {
                      if (context->uvsocks->callback_func)
                        context->uvsocks->callback_func (context->uvsocks,
                                                         UVSOCKS_ERROR_CONNECT,
                                                         context->uvsocks->callback_data);
                      break;
                    }
                }

              uvsocks_remote_set_stage (context, UVSOCKS_STAGE_TUNNELED);
              if (uvsocks_local_read_start (context))
                {
                  fprintf (stderr,
                          "uvsocks:: failed uvsocks_local_read_start\n");
                  if (uvsocks->callback_func)
                    uvsocks->callback_func (uvsocks,
                                            UVSOCKS_ERROR_POLL_LOCAL_READ_START,
                                            uvsocks->callback_data);

                  uvsocks_remove_context (uvsocks, poll->context);
                  return;
                }
            }
            break;
          case UVSOCKS_STAGE_TUNNELED:
            {
              int read;

              uv_mutex_lock (&poll->context->mutex);
              read = uvsocks_read_packet (poll->sock,
                                         &poll->buf[poll->read],
                                          UVSOCKS_BUF_MAX - poll->read);
              poll->read += read;
              uv_mutex_unlock (&poll->context->mutex);
            }
            break;
        }
    }
}

static void
uvsocks_local_read (uv_poll_t*  handle,
                    int         status,
                    int         events)
{
  UvSocksPoll *poll = handle->data;
  UvSocks *uvsocks = poll->context->uvsocks;

  if (status < 0 )
    {
      fprintf (stderr,
              "uvsocks:: failed poll local read -> %s@%s:%d status:%d events:%d\n",
               uvsocks->user,
               uvsocks->host,
               uvsocks->port,
               status,
               events);
      if (uvsocks->callback_func)
        uvsocks->callback_func (uvsocks,
                                UVSOCKS_ERROR_POLL_LOCAL_READ,
                                uvsocks->callback_data);
      uvsocks_remove_context (uvsocks, poll->context);
      return;
    }

  if (events & UV_DISCONNECT)
    {
      fprintf (stderr,
              "uvsocks:: local disconnected from %s@%s:%d status:%d events:%d\n",
               uvsocks->user,
               uvsocks->host,
               uvsocks->port,
               status,
               events);
      uvsocks_remove_context (uvsocks, poll->context);
      if (poll->context->forward->command == UVSOCKS_CMD_BIND)
        uvsocks_send_async (uvsocks, uvsocks_reverse_forward, poll->context->forward, NULL);
      return;
    }

  if (events & UV_WRITABLE)
    {
      UvSocksPoll *remote = poll->context->remote;
      int sent;

      if (remote->read > 0)
        {
          uv_mutex_lock (&poll->context->mutex);
          sent = uvsocks_write_packet (poll->sock,
                                       remote->buf,
                                       remote->read);
          remote->read -= sent;
          uv_mutex_unlock (&poll->context->mutex);
        }
    }

  if (events & UV_READABLE)
    {
      int read;

      uv_mutex_lock (&poll->context->mutex);
      read = uvsocks_read_packet (poll->sock,
                                 &poll->buf[poll->read],
                                  UVSOCKS_BUF_MAX - poll->read);
      poll->read += read;
      uv_mutex_unlock (&poll->context->mutex);
    }
}

static int
uvsocks_local_read_start (UvSocksContext *context)
{
  int r;

  r = uv_poll_init_socket (uv_default_loop (),
                          &context->local->handle,
                           context->local->sock);
  if (r)
    {
      free (context->local);
      return 1;
    }
  r = uv_poll_start (&context->local->handle,
                      UV_READABLE | UV_WRITABLE | UV_DISCONNECT,
                      uvsocks_local_read);
  if (r)
    return 1;

  return 0;
}

static int
uvsocks_remote_read_start (UvSocksContext *context)
{
  int r;

  if (uvsocks_set_nonblocking (context->remote->sock))
    return 1;

  r = uv_poll_init_socket (uv_default_loop (),
                          &context->remote->handle,
                           context->remote->sock);
  if (r)
    {
      free (context->remote);
      return 1;
    }
  r = uv_poll_start (&context->remote->handle,
                      UV_READABLE | UV_WRITABLE | UV_DISCONNECT,
                      uvsocks_remote_read);
  if (r)
    return 1;

  return 0;
}

static void
uvsocks_connect_remote_real (UvSocksContext   *context,
                             struct addrinfo  *resolved)
{
  struct sockaddr_in addr;
  int r;

  addr = *(const struct sockaddr_in *)resolved->ai_addr;

  if (uvsocks_create_socket (&context->remote->sock))
    return;

  r = connect (context->remote->sock, (struct sockaddr*) &addr, sizeof (addr));
  if (r || uvsocks_got_eagain ())
    {
      fprintf (stderr,
              "uvsocks:: failed to connect remote\n");
      if (context->uvsocks->callback_func)
       context->uvsocks->callback_func (context->uvsocks,
                                        UVSOCKS_ERROR_CONNECT,
                                        context->uvsocks->callback_data);
      uvsocks_remove_context (context->uvsocks, context);
      return;
    }
  uvsocks_remote_set_stage (context, UVSOCKS_STAGE_CONNECTED);
  if (uvsocks_remote_read_start (context))
    {
      fprintf (stderr,
              "uvsocks:: failed uvsocks_remote_read_start\n");
      if (context->uvsocks->callback_func)
        context->uvsocks->callback_func (context->uvsocks,
                                         UVSOCKS_ERROR_POLL_REMOTE_READ_START,
                                         context->uvsocks->callback_data);

      uvsocks_remove_context (context->uvsocks, context);
      return;
    }
}

static int
uvsocks_connect_remote (UvSocksForward *forward,
                        UvSocksContext *context,
                        char           *host,
                        int             port)
{
  uvsocks_dns_resolve (forward->uvsocks,
                       host,
                       g_strdup_printf("%i", port),
                       uvsocks_connect_remote_real,
                       context);
  return 0;
}

static void
uvsocks_local_new_connection (uv_poll_t *handle,
                              int        status,
                              int        events)
{
  UvSocksForward *forward = handle->data;
  UvSocksPoll *server = forward->server;
  UvSocksContext *context;
  struct sockaddr_in addr;
  socklen_t addr_len;
  uv_os_sock_t sock;

  addr_len = sizeof (addr);
  sock = accept (server->sock, (struct sockaddr*) &addr, &addr_len);
#ifdef _WIN32
  if (sock == INVALID_SOCKET)
    return;
#else
  if (s < 0)
    return;
#endif

  if (uvsocks_set_nonblocking (sock))
    return;

  context = uvsocks_create_context (forward);
  if (!context)
    return;

  context->local->sock = sock;
  uvsocks_add_context (forward->uvsocks, context);

  uvsocks_connect_remote (forward,
                          context,
                          forward->uvsocks->host,
                          forward->uvsocks->port);
}

static UvSocksPoll *
uvsocks_start_local_server (UvSocks    *uvsocks,
                            const char *host,
                            int        *port)
{
  UvSocksPoll *server;
  struct sockaddr_in addr;
  int r;

  if (*port < 0 || *port > 65535)
    return NULL;
  uv_ip4_addr (host, *port, &addr);

  server = calloc (1, sizeof (*server));
  if (!server)
    return NULL;

  if (uvsocks_create_socket (&server->sock))
    return NULL;

  if (uvsocks_set_nonblocking (server->sock))
    return NULL;

#ifndef _WIN32
  {
    /* Allow reuse of the port. */
    int yes = 1;
    r = setsockopt (server->sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof (yes);
    if (r)
      return NULL;
  }
#endif

  r = bind (server->sock, (const struct sockaddr*) &addr, sizeof (addr));
  if (r < 0)
    return NULL;

  r = uv_poll_init_socket (uv_default_loop (),
                          &server->handle,
                           server->sock);
  server->handle.data = server;

  r = listen (server->sock, SOMAXCONN);
  if (r)
    goto fail;

  r = uv_poll_start (&server->handle,
                      UV_READABLE | UV_WRITABLE | UV_DISCONNECT,
                      uvsocks_local_new_connection);
  if (r)
    goto fail;

  return server;

fail:
  free (server);
  return NULL;
}

static void
uvsocks_forward (UvSocks *uvsocks,
                 void    *data)
{
  UvSocksForward *forward = data;
  UvSocksPoll *s;
  int port;

  port = forward->listen_port;
  s = uvsocks_start_local_server (uvsocks,
                                  forward->listen_host,
                                  &port);
  if (!s)
    {
      fprintf (stderr,
              "failed to forward -> "
              "local:%s:%d path:%s -> server:%s:%d  -> remote:%s:%d path:%s\n",
               forward->listen_host,
               forward->listen_port,
               forward->listen_path,
               uvsocks->host,
               uvsocks->port,
               forward->remote_host,
               forward->remote_port,
               forward->remote_path);

      if (uvsocks->callback_func)
       uvsocks->callback_func (uvsocks,
                               UVSOCKS_ERROR_LOCAL_SERVER,
                               uvsocks->callback_data);
      return;
    }

  fprintf (stderr,
          "forward -> "
          "local:%s:%d path:%s -> server:%s:%d  -> remote:%s:%d path:%s\n",
           forward->listen_host,
           forward->listen_port,
           forward->listen_path,
           uvsocks->host,
           uvsocks->port,
           forward->remote_host,
           forward->remote_port,
           forward->remote_path);

  forward->server = s;
  forward->server->handle.data = forward;
  forward->listen_port = port;

  if (forward->callback_func)
    forward->callback_func (uvsocks,
                            forward->remote_host,
                            forward->remote_port,
                            forward->listen_host,
                            forward->listen_port,
                            forward->callback_data);
}

static void
uvsocks_reverse_forward (UvSocks *uvsocks,
                         void    *data)
{
  UvSocksForward *forward = data;
  UvSocksContext *context;

  context = uvsocks_create_context (forward);
  if (!context)
    return;

  uvsocks_add_context (forward->uvsocks, context);

  uvsocks_connect_remote (forward,
                          context,
                          forward->uvsocks->host,
                          forward->uvsocks->port);
}

static void
uvsocks_tunnel_real (UvSocks  *uvsocks,
                     void     *data)
{
  UvSocksStatus status;
  UvSocksForward *l;

  status = UVSOCKS_OK;

  for (l = uvsocks->reverse_forwards; l != NULL; l = l->next)
    uvsocks_send_async (uvsocks, uvsocks_reverse_forward, l, NULL);

  for (l = uvsocks->forwards; l != NULL; l = l->next)
    uvsocks_send_async (uvsocks, uvsocks_forward, l, NULL);
}

int
uvsocks_tunnel (UvSocks           *uvsocks,
                char              *host,
                int                port,
                char              *user,
                char              *password,
                UvSocksTunnelFunc  callback_func,
                void              *callback_data)
{
  fprintf (stderr,
          "tunnel -> "
          "host:%s:%d user:%s\n",
           host,
           port,
           user);

  uvsocks->host = g_strdup (host);
  uvsocks->port = port;
  uvsocks->user = g_strdup (user);
  uvsocks->password = g_strdup (password);

  uvsocks->callback_func = callback_func;
  uvsocks->callback_data = callback_data;

  uvsocks_send_async (uvsocks, uvsocks_tunnel_real, NULL, NULL);
  return 0;
}