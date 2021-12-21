/*
 * Copyright (C) 2021 Stefano Moioli <smxdev4@gmail.com>
 * This software is provided 'as-is', without any express or implied warranty. In no event will the authors be held liable for any damages arising from the use of this software.
 * Permission is granted to anyone to use this software for any purpose, including commercial applications, and to alter it and redistribute it freely, subject to the following restrictions:
 *  1. The origin of this software must not be misrepresented; you must not claim that you wrote the original software. If you use this software in a product, an acknowledgment in the product documentation would be appreciated but is not required.
 *  2. Altered source versions must be plainly marked as such, and must not be misrepresented as being the original software.
 *  3. This notice may not be removed or altered from any source distribution.
 */
#include <stdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "config.h"

#if defined(EZ_TARGET_POSIX)
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#elif defined(EZ_TARGET_WINDOWS)
#include <winsock2.h>
#include <ws2tcpip.h>
#endif


#include "dlfcn_compat.h"


#include "log.h"

#include "ezinject_common.h"
#include "ezinject_util.h"
#include "ezinject_injcode.h"

#undef MAX
#undef MIN
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#define MIN(a,b) ((a) < (b) ? (a) : (b))

LOG_SETUP(V_DBG);

#define WEBAPI_DEBUG

enum api_msg_type {
	OP_INFO = 'I',
	OP_DLOPEN = 'D',
	OP_DLCLOSE = 'F',
	OP_DLSYM = 'S',
	OP_PEEK = 'R',
	OP_POKE = 'W',
	OP_CALL = 'C',
	OP_QUIT = 'Q'
};

enum api_msg_ccall {
	C_CDECL = 'C',
	C_STDCALL = 'S',
	C_THISCALL = 'T'
};

struct api_msg {
	int type;
	unsigned char data[1];
};

/** IMPORTANT: always prefer this to unaligned writes, or it will break on ARM v4
 * https://developer.arm.com/documentation/dui0473/j/using-the-assembler/address-alignment
 **/
#define PUTINT(dst, src) do { \
	uintptr_t __t = (uintptr_t)(src); \
	memcpy(dst, &__t, sizeof(src)); \
} while(0)

/**
 * NOTE: we must ensure the TCP packet is aligned to NET_IP_ALIGN bytes (2 on old Linux)
 * or it will not be dequeued from the skb (https://lwn.net/Articles/89597/)
 */
#define MAX_BODYSZ 20
struct ez_pkt {
	uint32_t magic;
	uint32_t hdr_length;
	uint32_t body_length;
	uint8_t body[MAX_BODYSZ];
};

static void _build_pkt(struct ez_pkt *pkt, char *str){
	int length = (str == NULL) ? 0 : strlen(str);
	if(length > MAX_BODYSZ){
		// if we overflowed
		// we prefer being a no-op than sending an incorrect reply
		length = 0;
	} else if(length > 0) {
		strncpy(pkt->body, str, MAX_BODYSZ);
	}

	pkt->magic = htonl(0x4F4B3030); //OK00
	pkt->hdr_length = ntohl(sizeof(*pkt));
	pkt->body_length = ntohl(length);
}

intptr_t safe_send(int fd, void *buf, size_t length, int flags){
	uint8_t *pb = (uint8_t *)buf;

	ssize_t acc = 0;
	while(acc < length){
		ssize_t sent = send(fd, &pb[acc], length - acc, 0);
		if(sent < 0){
			PERROR("send");
			return -1;
		}
		acc += sent;
	}
	return (intptr_t)acc;
}

intptr_t safe_recv(int fd, void *buf, size_t length, int flags){
	uint8_t *pb = (uint8_t *)buf;
	
	ssize_t acc = 0;
	while(acc < length){
		ssize_t received = recv(fd, &pb[acc], length - acc, 0);
		if(received < 0){
			PERROR("recv");
			return -1;
		}
		acc += received;
	}
	return (intptr_t)acc;
}

intptr_t send_str(int fd, char *str){
	struct ez_pkt pkt;
	memset(&pkt, 0x00, sizeof(pkt));

	_build_pkt(&pkt, str);
	uint8_t *pb = (uint8_t *)&pkt;
#ifdef WEBAPI_DEBUG
	for(int i=0; i<sizeof(pkt); i++){
		printf("%02hhx ", pb[i]);
	}
	puts("");
#endif

	if(send(fd, &pkt, sizeof(pkt), 0) != sizeof(pkt)){
		return -1;
	}
	return 0;
}

intptr_t send_ptrstr(int fd, void *ptr){
	uint8_t buf[MAX_BODYSZ];
	memset(buf, 0x00, sizeof(buf));
	snprintf(buf, sizeof(buf), "%p", ptr);
	return send_str(fd, buf);
}

intptr_t send_datahdr(int fd, unsigned int size){
	struct ez_pkt pkt;
	memset(&pkt, 0x00, sizeof(pkt));

	_build_pkt(&pkt, NULL);
	pkt.body_length = ntohl(size);
	if(send(fd, &pkt, sizeof(pkt), 0) != sizeof(pkt)){
		return -1;
	}
	return 0;
}

int handle_client(int client){
	/** disable NAGLE algorithm (packet aggregation) **/
	int flag = 1;
	setsockopt(client, IPPROTO_TCP, TCP_NODELAY, (char *) &flag, sizeof(int));

	int last_txrx = 0;

	#define SAFE_SEND(fd, buf, length, flags) do { \
		last_txrx = safe_send(fd, buf, length, flags); \
		if(last_txrx < 0) { \
			PERROR("send"); \
			serve = 0; break; \
		} \
	} while(0)

	#define SAFE_RECV(fd, buf, length, flags) do { \
		last_txrx = safe_recv(fd, buf, length, flags); \
		if(last_txrx < 0) { \
			PERROR("recv"); \
			serve = 0; break; \
			break; \
		} \ 
	} while(0)


	int serve = 1;
	while(serve){
		unsigned int length = 0;
		SAFE_RECV(client, &length, sizeof(length), 0);
		length = ntohl(length);
		int malloc_sz = WORDALIGN(MAX(length, 64));
		DBG("incoming msg, length: %u", length);
		uint8_t *mem = calloc(malloc_sz, 1);
		uint8_t *data = mem;
		SAFE_RECV(client, data, length, 0);

		switch(*(data++)){
			case OP_INFO:{
				DBG("OP_INFO");
				#if defined(EZ_TARGET_POSIX)
				send_str(client, "posix");
				#elif defined(EZ_TARGET_WINDOWS)
				send_str(client, "win32");
				#else
				#error "Unsupported build type"
				#endif
				break;
			}
			case OP_DLOPEN:{
				DBG("OP_DLOPEN");
				const char *path = (const char *)data;
				void *handle = NULL;
				if(length == 2 && path[0] == '0'){
					DBG("dlopen(NULL)");
				#if defined(EZ_TARGET_POSIX)
					handle = dlopen(NULL, RTLD_NOW);
				#elif defined(EZ_TARGET_WINDOWS)
					handle = GetModuleHandle(NULL);
				#endif
				} else {
					DBG("dlopen(%s)", path);
					handle = LIB_OPEN(path);
				}
				if(send_ptrstr(client, handle) != 0){
					ERR("send_ptrstr failed");
					serve = 0;
				}
				break;
			}
			case OP_DLSYM:{
				DBG("OP_DLSYM");
				void *handle = (void *)strtoull(data, NULL, 16);
				data = strchr(data, ' ') + 1;
				DBG("dlsym(%p, %s)", handle, data);
				void *sym = LIB_GETSYM(handle, data);
				if(send_ptrstr(client, sym) != 0){
					ERR("send_ptrstr failed");
					serve = 0;
				}
				break;
			}
			case OP_DLCLOSE:{
				DBG("OP_DLCLOSE");
				void *handle = (void *)strtoull(data, NULL, 16);
				LIB_CLOSE(handle);
				if(send_str(client, NULL) != 0){
					ERR("send_str failed");
					serve = 0;
				}
				break;
			}
			case OP_PEEK:{
				DBG("OP_PEEK");
				uint8_t *start_addr = (uint8_t *)strtoull(data, NULL, 16);
				data = strchr(data, ' ') + 1;
				unsigned int length = strtoul(data, NULL, 16);
				DBG("length: %u", length);
				
				int blocksize = 4096;
				int nblocks = length / blocksize;
				int blockoff = length % blocksize;

				// align to multiple of NETALIGN (2)
				int rem = (length % sizeof(uintptr_t));
				uintptr_t padding = 0;

				int length_aligned = ntohl(length + rem);

				if(send_datahdr(client, 0) != 0){
					ERR("send_datahdr failed");
					serve = 0;
					break;
				}
				SAFE_SEND(client, &length_aligned, sizeof(length_aligned), 0);

				DBG("writing %d blocks", nblocks);
				for(int i=0; i<nblocks; i++){
					SAFE_SEND(client, start_addr, blocksize, 0);
					start_addr += blocksize;
				}
				DBG("writing %d bytes", blockoff);
				if(blockoff > 0){
					SAFE_SEND(client, start_addr, blockoff, 0);
				}

				if(rem > 0){
					DBG("writing rem: %d", rem);
					SAFE_SEND(client, &padding, rem, 0);
				}
				break;
			}
			case OP_POKE:{
				DBG("OP_POKE");
				uint8_t *start_addr = (uint8_t *)strtoull(data, NULL, 16);
				data = strchr(data, ' ') + 1;
				unsigned int length = strtoul(data, NULL, 16);
				DBG("size: %u", length);

				int blocksize = 4096;
				int nblocks = length / blocksize;
				int blockoff = length % blocksize;

				DBG("reading %d blocks", nblocks);
				for(int i=0; i<nblocks; i++){
					SAFE_RECV(client, start_addr, blocksize, 0);
					start_addr += blocksize;
				}
				DBG("reading %d bytes", blockoff);
				if(blockoff > 0){
					SAFE_RECV(client, start_addr, blockoff, 0);
				}
				
				if(send_str(client, NULL) != 0){
					ERR("send_str failed");
					serve = 0;
				}

				break;
			}
			case OP_CALL:{
				DBG("OP_CALL");
				uint8_t call_type = *(data++);
				int nargs = strtoul(data, NULL, 16);
				data = strchr(data, ' ') + 1;

				void *addr = (void *)strtoull(data, NULL, 16);
				data = strchr(data, ' ') + 1;

				if(nargs > 14) break;

				void *a[15] = {NULL};
				for(int i=0; i<nargs; i++){
					a[i] = (void *)strtoull(data, NULL, 16);
					DBGPTR(a[i]);
					data = strchr(data, ' ') + 1;
				}

				void *result = NULL;

				switch(call_type){
					case C_CDECL:
						switch(nargs){					
							#include "calls.c"
						}
						break;
					case C_STDCALL:
						break;
					case C_THISCALL:
						break;
				}

				if(send_ptrstr(client, result) != 0){
					ERR("send_ptrstr failed");
					serve = 0;
				}
				break;
			}
			case OP_QUIT:{
				if(send_str(client, NULL) != 0){
					ERR("send_str failed");
				}
				serve = 0;
				break;
			}
				
		}

		free(mem);
	}
	close(client);
	return 0;
}

void *start_server(void *arg){
	unsigned short port = (unsigned short)arg;
	int rc = 0;
	do {
		int sock = socket(AF_INET, SOCK_STREAM, 0);
		if(sock < 0){
			perror("socket");
			rc = -1;
			break;
		}

		struct sockaddr_in sa = {
			.sin_family = AF_INET,
			.sin_addr.s_addr = htonl(INADDR_ANY),
			.sin_port = htons(port)
		};
		int enable = 1;
		if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0){
    		perror("setsockopt(SO_REUSEADDR)");
		}
		if(bind(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0){
			perror("bind");
			rc = -1;
			break;
		}

		if(listen(sock, 5) < 0){
			perror("listen");
			rc = -1;
			break;
		}

		int run = 1;
		while(run){
			struct sockaddr_in sac;
			socklen_t saclen = sizeof(sac);
			int client = accept(sock, (struct sockaddr *)&sac, &saclen);
			if(client < 0){
				perror("accept");
			}
			int result = handle_client(client);
		}
	} while(0);

	return (void *)rc;
}

int lib_preinit(struct injcode_user *user){
	user->persist = 1;
	return 0;
}

int lib_main(int argc, char *argv[]){
	lputs("Hello World from main");
	for(int i=0; i<argc; i++){
		lprintf("argv[%d] = %s\n", i, argv[i]);
	}

	if(argc < 2){
		lprintf("usage: %s [port]\n", argv[0]);
		return 1;
	}

	int port = strtoul(argv[1], NULL, 10);
	pthread_t tid;
	pthread_create(&tid, NULL, start_server, port);

	return 0;
}
