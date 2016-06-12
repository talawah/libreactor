#define _GNU_SOURCE

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <netdb.h>
#include <assert.h>
#include <err.h>
#include <regex.h>
#include <sys/socket.h>

#include <dynamic.h>
#include <clo.h>

#include "reactor_user.h"
#include "reactor_desc.h"
#include "reactor_core.h"
#include "reactor_stream.h"
#include "reactor_tcp.h"
#include "reactor_http.h"
#include "reactor_rest.h"

static inline void reactor_rest_dispatch(reactor_rest *rest, int type, void *state)
{
  if (rest->user.callback)
    reactor_user_dispatch(&rest->user, type, state);
}

static inline void reactor_rest_close_final(reactor_rest *rest)
{
  if (rest->state == REACTOR_REST_CLOSE_WAIT &&
      rest->http.state == REACTOR_HTTP_CLOSED &&
      rest->ref == 0)
    {
      vector_clear(&rest->maps);
      rest->state = REACTOR_REST_CLOSED;
      reactor_rest_dispatch(rest, REACTOR_REST_CLOSE, NULL);
    }
}

static void reactor_rest_map_release(void *data)
{
  reactor_rest_map *map;

  map = data;
  free(map->method);
  free(map->path);
  if (map->regex)
    {
      regfree(map->regex);
      free(map->regex);
    }
}

static void reactor_rest_cors(void *state, reactor_rest_request *request)
{
  //char *cors_origin, *cors_allow_headers;

  (void) state;
  (void) request;
  /*
  cors_origin = reactor_http_field_lookup(&request->request->fields, "Origin");
  if (!cors_origin)
    {
      reactor_rest_respond_empty(request, 404);
      return;
    }

  cors_allow_headers = reactor_http_field_lookup(&request->request->fields, "Access-Control-Request-Headers");
  reactor_rest_respond_fields(request, 204, NULL, NULL, 0, (reactor_http_field[]) {
      {.key = "Access-Control-Allow-Methods", .value = "GET, OPTIONS"},
      {.key = "Access-Control-Allow-Headers", .value = cors_allow_headers},
      {.key = "Access-Control-Max-Age", .value = "1728000"}}, 3);
  */
}


void reactor_rest_init(reactor_rest *rest, reactor_user_callback *callback, void *state)
{
  *rest = (reactor_rest) {.state = REACTOR_REST_CLOSED};
  reactor_user_init(&rest->user, callback, state);
  reactor_http_init(&rest->http, reactor_rest_event, rest);
  vector_init(&rest->maps, sizeof(reactor_rest_map));
  vector_release(&rest->maps, reactor_rest_map_release);
}

void reactor_rest_open(reactor_rest *rest, char *node, char *service, int flags)
{
  if (rest->state != REACTOR_REST_CLOSED)
    {
      reactor_rest_error(rest);
      return;
    }

  rest->flags = flags;
  if (flags & REACTOR_REST_ENABLE_CORS)
    reactor_rest_add_match(rest, "OPTIONS", NULL, reactor_rest_cors, NULL);

  rest->state = REACTOR_REST_OPEN;
  reactor_http_open(&rest->http, node, service, REACTOR_HTTP_SERVER);
}

void reactor_rest_error(reactor_rest *rest)
{
  rest->state = REACTOR_REST_INVALID;
  reactor_rest_dispatch(rest, REACTOR_REST_ERROR, NULL);
}

void reactor_rest_close(reactor_rest *rest)
{
  if (rest->state == REACTOR_REST_CLOSED)
    return;

  reactor_http_close(&rest->http);
  rest->state = REACTOR_REST_CLOSE_WAIT;
  reactor_rest_close_final(rest);
}

void reactor_rest_event(void *state, int type, void *data)
{
  reactor_rest *rest = state;
  reactor_http_session *session = data;
  reactor_http_message *message = &session->message;
  reactor_rest_request request = {.rest = rest, .session = session};
  reactor_rest_map *map;
  size_t i, nmatch = 32;
  regmatch_t match[nmatch];
  int e;

  switch (type)
    {
    case REACTOR_HTTP_MESSAGE:
      for (i = 0; i < vector_size(&rest->maps); i ++)
        {
          map = vector_at(&rest->maps, i);
          if (!map->method || strcmp(map->method, message->method) == 0)
            switch (map->type)
              {
              case REACTOR_REST_MAP_MATCH:
                if (!map->path || strcmp(map->path, message->path) == 0)
                  {
                    map->handler(map->state, &request);
                    return;
                  }
                break;
              case REACTOR_REST_MAP_REGEX:
                e = regexec(map->regex, message->path, 32, match, 0);
                if (e == 0)
                  {
                    request.match = match;
                    map->handler(map->state, &request);
                    return;
                  }
                break;
              }
        }
      reactor_rest_respond(&request, 404, 0, NULL, 0, NULL);
      break;
    case REACTOR_HTTP_ERROR:
      reactor_rest_error(rest);
      break;
    case REACTOR_HTTP_SHUTDOWN:
      reactor_rest_dispatch(rest, REACTOR_REST_SHUTDOWN, NULL);
      break;
    case REACTOR_HTTP_CLOSE:
      reactor_rest_close_final(rest);
      break;
    }
}

void reactor_rest_add_match(reactor_rest *rest, char *method, char *path, reactor_rest_handler *handler, void *state)
{
  reactor_rest_map map =
    {
      .type = REACTOR_REST_MAP_MATCH,
      .method = method ? strdup(method) : NULL,
      .path = path ? strdup(path) : NULL,
      .handler = handler,
      .state = state
    };

  vector_push_back(&rest->maps, &map);
  if ((method && !map.method) || (path && !map.path))
    reactor_rest_error(rest);
}

void reactor_rest_add_regex(reactor_rest *rest, char *method, char *regex, reactor_rest_handler *handler, void *state)
{
  int e;
  reactor_rest_map map =
    {
      .type = REACTOR_REST_MAP_REGEX,
      .method = method ? strdup(method) : NULL,
      .regex = malloc(sizeof(regex_t)),
      .handler = handler,
      .state = state
    };

  if (map.regex)
    {
      e = regcomp(map.regex, regex, REG_EXTENDED);
      if (e == -1)
        {
          regfree(map.regex);
          free(map.regex);
          map.regex = NULL;
        }
    }

  if (!map.regex)
    {
      free(map.method);
      reactor_rest_error(rest);
      return;
    }

  vector_push_back(&rest->maps, &map);
}

void reactor_rest_respond(reactor_rest_request *request, int status, size_t header_size, reactor_http_header *header,
                          size_t body_size, void *body)
{
  reactor_http_session_message(request->session, (reactor_http_message[]) {{
          .type = REACTOR_HTTP_MESSAGE_RESPONSE,
          .version = 1,
          .status = status,
          .reason = "OK",
          .header_size = header_size,
          .header = header,
          .body_size = body_size,
          .body = body
          }});
}

void reactor_rest_text(reactor_rest_request *request, char *text)
{
  reactor_http_session_message(request->session, (reactor_http_message[]) {reactor_http_message_text(text)});
}

/*
void reactor_rest_respond_empty(reactor_rest_request *request, unsigned status)
{
  reactor_rest_respond(request, status, NULL, NULL, 0);
}

void reactor_rest_respond_fields(reactor_rest_request *request, unsigned status,
                                 char *content_type, char *content, size_t content_size,
                                 reactor_http_field *fields, size_t nfields)
{
  reactor_http_field cors_fields[nfields + 3];
  char *cors_origin;

  cors_origin = NULL;
  if (request->server->flags & REACTOR_REST_ENABLE_CORS)
    cors_origin = reactor_http_field_lookup(&request->request->fields, "Origin");
  if (!cors_origin)
    reactor_http_server_session_respond_fields(request->session, status, content_type, content, content_size,
                                                 fields, nfields);
  else
    {
      memcpy(cors_fields, fields, nfields * (sizeof *fields));
      cors_fields[nfields] = (reactor_http_field) {.key = "Access-Control-Allow-Origin", .value = cors_origin};
      nfields ++;
      cors_fields[nfields] = (reactor_http_field) {.key = "Access-Control-Allow-Credentials", .value = "true"};
      nfields ++;
      cors_fields[nfields] = (reactor_http_field) {.key = "Vary", .value = "Origin"};
      nfields ++;
      reactor_http_server_session_respond_fields(request->session, status, content_type, content, content_size,
                                                 cors_fields, nfields);
    }
}



void reactor_rest_respond_found(reactor_rest_request *request, char *location)
{
  reactor_rest_respond_fields(request, 302, NULL, NULL, 0, (reactor_http_field[]){{.key = "Location", .value = location}}, 1);
}

void reactor_rest_respond_clo(reactor_rest_request *request, unsigned status, clo *clo)
{
  buffer b;
  int e;

  buffer_init(&b);
  e = 0;
  clo_encode(clo, &b, &e);
  if (e == 0)
    reactor_rest_respond(request, status, "application/json", buffer_data(&b), buffer_size(&b));
  else
    reactor_rest_respond_empty(request, 500);
  buffer_clear(&b);
}

*/
