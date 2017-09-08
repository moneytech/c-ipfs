/**
 * Methods for lightweight/specific HTTP for API communication.
 */
#define _GNU_SOURCE
#define __USE_GNU
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <arpa/inet.h>

#include <fcntl.h>

#include "libp2p/net/p2pnet.h"
#include "libp2p/utils/logger.h"
#include "ipfs/core/api.h"

pthread_mutex_t conns_lock;
int conns_count;

pthread_t listen_thread = 0;

struct s_list api_list;

/**
 * Write two strings on one write.
 * @param fd file descriptor to write.
 * @param str1 first string to write.
 * @param str2 second string to write.
 */
size_t write_dual(int fd, char *str1, char *str2)
{
	struct iovec iov[2];

	iov[0].iov_base = str1;
	iov[0].iov_len = strlen(str1);
	iov[1].iov_base = str2;
	iov[1].iov_len = strlen(str2);

	return writev(fd, iov, 2);
}

int find_chunk(char *buf, const size_t buf_size, size_t *pos, size_t *size)
{
	char *p = NULL;

	*size = strtol(buf, &p, 16);
	if (*size < 0 || !p || p < buf || p > (buf + 10)) {
		return 0;
	}
	*pos = (int)(p - buf);
	if (p[0] == '\r' && p[1] == '\n') {
		*pos += 2;
		return 1;
	}
	return 0;
}

int read_chunked(int fd, struct s_request *req, char *already, size_t already_size)
{
	char buf[MAX_READ], *p;
	size_t pos, nsize, buf_size = 0, r;

	if (already_size > 0) {
		if (already_size <= sizeof(buf)) {
			memcpy(buf, already, already_size);
			buf_size += already_size;
			already_size = 0;
		} else {
			memcpy(buf, already, sizeof(buf));
			already += sizeof(buf);
			buf_size += sizeof(buf);
			already_size -= sizeof(buf);
		}
	}

	while(buf_size) {
		if (!find_chunk(buf, buf_size, &pos, &nsize)) {
			libp2p_logger_error("api", "fail find_chunk.\n");
			libp2p_logger_error("api", "nsize = %d.\n", nsize);
			return 0;
		}
		if (nsize == 0) {
			break;
		}
		p = realloc(req->buf, req->size + nsize);
		if (!p) {
			libp2p_logger_error("api", "fail realloc.\n");
			return 0;
		}
		req->buf = p;
		req->size += nsize;

CPCHUNK:
		r = nsize;
		buf_size -= pos;
		if (r > buf_size) {
			r = buf_size;
		}
		memcpy(req->buf + req->body + req->body_size, buf + pos, r);
		req->body_size += r;
		nsize -= r;
		buf_size -= r;
		if (buf_size > 0) {
			memmove(buf, buf + pos + r, buf_size);
		}
		pos = 0;
		if (already_size > 0) {
			r = sizeof(buf) - buf_size;
			if (already_size <= r) {
				memcpy(buf, already, already_size);
				buf_size += already_size;
				already_size = 0;
			} else {
				memcpy(buf, already, r);
				already += r;
				buf_size += r;
				already_size -= r;
			}
		}

		if (socket_read_select4(fd, 5) > 0) {
			r = sizeof(buf) - buf_size;
			r = read(fd, buf+buf_size, r);
			buf_size += r;
			if (r == 0 && nsize == 0) {
				break;
			}

			if (r <= 0) {
				libp2p_logger_error("api", "read fail.\n");
				return 0;
			}
		}

		if (nsize > 0)
			goto CPCHUNK; // still have data to transfer on current chunk.

		if (memcmp (buf, "\r\n", 2)!=0) {
			libp2p_logger_error("api", "fail CRLF.\n");
			return 0;
		}
	}
	return 1;
}

int read_all(int fd, struct s_request *req, char *already, size_t alread_size)
{
	char buf[MAX_READ], *p;
	size_t size = 0;

	if (alread_size > 0) {
		p = realloc(req->buf, req->size + alread_size);
		if (!p) {
			return 0;
		}
		req->buf = p;
		req->size += alread_size;
		memcpy(req->buf + req->body + req->body_size, already, alread_size);
		req->body_size += alread_size;
	}
	for(;;) {
		if (socket_read_select4(fd, 5) <= 0) {
			break;
		}
		size = read(fd, buf, sizeof buf);
		if (size <= 0) {
			break;
		}
		p = realloc(req->buf, req->size + size);
		if (!p) {
			return 0;
		}
		req->buf = p;
		req->size += size;
		memcpy(req->buf + req->body + req->body_size, buf, size);
		req->body_size += size;
	}
	return 1;
}

/**
 * Find a token in a string array.
 * @param string array and token string.
 * @returns the pointer after where the token was found or NULL if it fails.
 */
char *str_tok(char *str, char *tok)
{
	char *p = strstr(str, tok);
	if (p) {
		p += strlen(tok);
		while(*p == ' ') p++;
	}
	return p;
}

/**
 * Find a token in a binary array.
 * @param array, size of array, token and size of token.
 * @returns the pointer after where the token was found or NULL if it fails.
 */
char *bin_tok(char *bin, size_t limit, char *tok, size_t tok_size)
{
	char *p = memmem(bin, limit, tok, tok_size);
	if (p) {
		p += tok_size;
	}
	return p;
}

/**
 * Check if header contain a especific value.
 * @param request structure, header name and value to check.
 * @returns the pointer where the value was found or NULL if it fails.
 */
char *header_value_cmp(struct s_request *req, char *header, char *value)
{
	char *p = str_tok(req->buf + req->header, header);
	if (p) {
		if (strstart(p, value)) {
			return p;
		}
	}
	return NULL;
}

/**
 * Lookup for boundary at buffer string.
 * @param body buffer string, boundary id, filename and content-type string.
 * @returns the pointer where the multipart start.
 */
char *boundary_find(char *str, char *boundary, char **filename, char **contenttype)
{
	char *p = str_tok(str, "--");
	while (p) {
		if (strstart(p, boundary)) {
			// skip to the beginning, ignoring the header for now, if there is.
			// TODO: return filename and content-type
			p = strstr(p, "\r\n\r\n");
			if (p) {
				return p + 4; // ignore 4 bytes CRLF 2x
			}
			break;
		}
		p = str_tok(str, "--");
	}
	return NULL;
}

/**
 * Return the size of boundary.
 * @param boundary buffer, boundary id.
 * @returns the size of boundary or 0 if fails.
 */
size_t boundary_size(char *str, char *boundary, size_t limit)
{
	char *p = bin_tok(str, limit, "\r\n--", 4);
	while (p) {
		if (strstart(p, boundary)) {
			if (cstrstart(p + strlen(boundary), "--\r\n")) {
				p -= 4;
				return (size_t)(p - str);
			}
		}
		p = bin_tok(p, limit, "\r\n--", 4);
	}
	return 0;
}

/**
 * Pthread to take care of each client connection.
 * @param ptr is the connection index in api_list, integer not pointer, cast required.
 * @returns nothing
 */
void *api_connection_thread (void *ptr)
{
	int timeout, s, r;
	const INT_TYPE i = (INT_TYPE) ptr;
	char resp[MAX_READ+1], buf[MAX_READ+1], *p, *body;
	char client[INET_ADDRSTRLEN];
	struct s_request req;
	int (*read_func)(int, struct s_request*, char*, size_t) = read_all;

	req.buf = NULL; // sanity.

	buf[MAX_READ] = '\0';

	s = api_list.conns[i]->socket;
	timeout = api_list.timeout;

	if (socket_read_select4(s, timeout) <= 0) {
		libp2p_logger_error("api", "Client connection timeout.\n");
		goto quit;
	}
	r = read(s, buf, sizeof buf);
	if (r <= 0) {
		libp2p_logger_error("api", "Read from client fail.\n");
		goto quit;
	}
	buf[r] = '\0';

	p = strstr(buf, "\r\n\r\n");

	if (p) {
		body = p + 4;

		req.size = p - buf + 1;
		req.buf = malloc(req.size);
		if (!req.buf) {
			// memory allocation fail.
			libp2p_logger_error("api", "malloc fail.\n");
			write_cstr (s, HTTP_500);
			goto quit;
		}
		memcpy(req.buf, buf, req.size - 1);
		req.buf[req.size-1] = '\0';

		req.method = 0;
		p = strchr(req.buf + req.method, ' ');
		if (!p) {
			libp2p_logger_error("api", "fail looking for space on method '%s'.\n", req.buf + req.method);
			write_cstr (s, HTTP_400);
			goto quit;
		}
		*p++ = '\0'; // End of method.
		req.path = p - req.buf;
		p = strchr(p, ' ');
		if (!p) {
			libp2p_logger_error("api", "fail looking for space on path '%s'.\n", req.buf + req.path);
			write_cstr (s, HTTP_400);
			goto quit;
		}
		*p++ = '\0'; // End of path.
		req.http_ver = p - req.buf;
		p = strchr(req.buf + req.http_ver, '\r');
		if (!p) {
			libp2p_logger_error("api", "fail looking for CR on http_ver '%s'.\n", req.buf + req.http_ver);
			write_cstr (s, HTTP_400);
			goto quit;
		}
		*p++ = '\0'; // End of http version.
		while (*p == '\r' || *p == '\n') p++;
		req.header = p - req.buf;
		req.body = req.size;
		req.body_size = 0;

		if (header_value_cmp(&req, "Transfer-Encoding:", "chunked")) {
			read_func = read_chunked;
		}

		if (!read_func(s, &req, body, r - (body - buf))) {
			libp2p_logger_error("api", "fail read_func.\n");
			write_cstr (s, HTTP_500);
			goto quit;
		}

		if (strcmp(req.buf + req.method, "GET")==0) {
			// just an error message, because it's not used yet.
			// TODO: implement gateway requests and GUI (javascript) for API.
			write_dual (s, req.buf + req.http_ver, strchr (HTTP_404, ' '));
		} else if (cstrstart(buf, "POST ")) {
			// TODO: Handle gzip/json POST requests.

			p = header_value_cmp(&req, "Content-Type:", "multipart/form-data;");
			if (p) {
				p = str_tok(p, "boundary=");
				if (p) {
					char *boundary, *l;
					int len;
					if (*p == '"') {
						p++;
						l = strchr(p, '"');
					} else {
						l = p;
						while (*l != '\r' && *l != '\0') l++;
					}
					len = l - p;
					boundary = malloc (len+1);
					if (boundary) {
						memcpy(boundary, p, len);
						boundary[len] = '\0';

						p = boundary_find(req.buf + req.body, boundary, NULL, NULL);
						if (p) {
							req.boundary_size = boundary_size(p, boundary, req.size - (p - buf));
							if (req.boundary_size > 0) {
								req.boundary = p - req.buf;
							}
						}

						free (boundary);
					}
				}
			}
			// TODO: Parse the path var and decide what to do with the received data.
			if (req.boundary > 0) {
				libp2p_logger_error("api", "boundary index = %d, size = %d\n", req.boundary, req.boundary_size);
			}

			libp2p_logger_error("api", "method = '%s'\n"
						   "path = '%s'\n"
						   "http_ver = '%s'\n"
						   "header {\n%s\n}\n"
						   "body_size = %d\n",
			req.buf+req.method, req.buf+req.path, req.buf+req.http_ver,
			req.buf+req.header, req.body_size);

			snprintf(resp, sizeof(resp), "%s 200 OK\r\n" \
			"Content-Type: application/json\r\n"
			"Server: c-ipfs/0.0.0-dev\r\n"
			"X-Chunked-Output: 1\r\n"
			"Connection: close\r\n"
			"Transfer-Encoding: chunked\r\n\r\n", req.buf + req.http_ver);
			write_str (s, resp);
			libp2p_logger_error("api", "resp = {\n%s\n}\n", resp);
		}
	} else {
		libp2p_logger_error("api", "fail looking for body.\n");
		write_cstr (s, HTTP_400);
	}

quit:
	if (req.buf)
		free(req.buf);
	if (inet_ntop(AF_INET, &(api_list.conns[i]->ipv4), client, INET_ADDRSTRLEN) == NULL)
		strcpy(client, "UNKNOW");
	libp2p_logger_error("api", "Closing client connection %s:%d (%d).\n", client, api_list.conns[i]->port, i+1);
	pthread_mutex_lock(&conns_lock);
	close(s);
	free (api_list.conns[i]);
	api_list.conns[i] = NULL;
	conns_count--;
	pthread_mutex_unlock(&conns_lock);

	return NULL;
}

/**
 * Close all connections stopping respectives pthreads and free allocated memory.
 */
void api_connections_cleanup (void)
{
	int i;

	pthread_mutex_lock(&conns_lock);
	if (conns_count > 0 && api_list.conns) {
		for (i = 0 ; i < api_list.max_conns ; i++) {
			if (api_list.conns[i]->pthread) {
				pthread_cancel (api_list.conns[i]->pthread);
				close (api_list.conns[i]->socket);
				free (api_list.conns[i]);
				api_list.conns[i] = NULL;
			}
		}
		conns_count = 0;
	}
	if (api_list.conns) {
		free (api_list.conns);
		api_list.conns = NULL;
	}
	pthread_mutex_unlock(&conns_lock);
}

/**
 * Pthread to keep in background dealing with client connections.
 * @param ptr is not used.
 * @returns nothing
 */
void *api_listen_thread (void *ptr)
{
	int s;
	INT_TYPE i;
	uint32_t ipv4;
	uint16_t port;
	char client[INET_ADDRSTRLEN];

	conns_count = 0;

	for (;;) {
		s = socket_accept4(api_list.socket, &ipv4, &port);
		if (s <= 0) {
			break;
		}
		if (conns_count >= api_list.max_conns) { // limit reached.
			libp2p_logger_error("api", "Limit of connections reached (%d).\n", api_list.max_conns);
			close (s);
			continue;
		}

		pthread_mutex_lock(&conns_lock);
		for (i = 0 ; i < api_list.max_conns && api_list.conns[i] ; i++);
		api_list.conns[i] = malloc (sizeof (struct s_conns));
		if (!api_list.conns[i]) {
			libp2p_logger_error("api", "Fail to allocate memory to accept connection.\n");
			close (s);
			continue;
		}
		if (inet_ntop(AF_INET, &ipv4, client, INET_ADDRSTRLEN) == NULL)
			strcpy(client, "UNKNOW");
		api_list.conns[i]->socket = s;
		api_list.conns[i]->ipv4   = ipv4;
		api_list.conns[i]->port   = port;
		if (pthread_create(&(api_list.conns[i]->pthread), NULL, api_connection_thread, (void*)i)) {
			libp2p_logger_error("api", "Create pthread fail.\n");
			free (api_list.conns[i]);
			api_list.conns[i] = NULL;
			conns_count--;
			close(s);
		} else {
			conns_count++;
		}
		libp2p_logger_error("api", "Accept connection %s:%d (%d/%d), pthread %d.\n", client, port, conns_count, api_list.max_conns, i+1);
		pthread_mutex_unlock(&conns_lock);
	}
	api_connections_cleanup ();
	return NULL;
}

/**
 * Start API interface daemon.
 * @param port.
 * @param max_conns.
 * @param timeout time out of client connection.
 * @returns 0 when failure or 1 if success.
 */
int api_start (uint16_t port, int max_conns, int timeout)
{
	int s;
	size_t alloc_size = sizeof(void*) * max_conns;

	api_list.ipv4 = hostname_to_ip("127.0.0.1"); // api is listening only on loopback.
	api_list.port = port;

	if (listen_thread != 0) {
		libp2p_logger_error("api", "API already running.\n");
		return 0;
	}

	if ((s = socket_listen(socket_tcp4(), &(api_list.ipv4), &(api_list.port))) <= 0) {
		libp2p_logger_error("api", "Failed to init API. port: %d\n", port);
		return 0;
	}

	api_list.socket = s;
	api_list.max_conns = max_conns;
	api_list.timeout = timeout;

	api_list.conns = malloc (alloc_size);
	if (!api_list.conns) {
		close (s);
		libp2p_logger_error("api", "Error allocating memory.\n");
		return 0;
	}
	memset(api_list.conns, 0, alloc_size);

	if (pthread_create(&listen_thread, NULL, api_listen_thread, NULL)) {
		close (s);
		free (api_list.conns);
		api_list.conns = NULL;
		listen_thread = 0;
		libp2p_logger_error("api", "Error creating thread for API.\n");
		return 0;
	}

	return 1;
}

/**
 * Stop API.
 * @returns 0 when failure or 1 if success.
 */
int api_stop (void)
{
	if (!listen_thread) return 0;
	pthread_cancel(listen_thread);

	api_connections_cleanup ();

	listen_thread = 0;

	return 1;
}
