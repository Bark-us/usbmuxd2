//
//  SockConn.cpp
//  usbmuxd2
//
//  Created by tihmstar on 18.08.19.
//  Copyright © 2019 tihmstar. All rights reserved.
//


#include "SockConn.hpp"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <log.h>
#include <libgeneral/macros.h>
#include <Client.hpp>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <netdb.h>
#include <system_error>


SockConn::SockConn(std::string ipaddr, uint16_t dPort, Client *cli, uint32_t ifIndex)
: _ipaddr(ipaddr), _cli(cli), _dPort(dPort), _ifIndex(ifIndex), _killInProcess(false), _didConnect(false), _cfd(-1), _dfd(-1), _pfds(NULL)
{

}

SockConn::~SockConn(){
	debug("Destroying SockConn (%p) dPort=%u",this,_dPort);
	auto fdlist = {_cfd,_dfd};
	_cfd = -1;
	_dfd = -1;

	for (int fd : fdlist){
		if (fd>0) {
			debug("~SockConn(%p) closing(%d)",this,fd);
        	close(fd);
  		}
	}
	safeFree(_pfds);
}

void SockConn::connect(){
	cleanup([&]{
        _didConnect = true; // make sure destructor knows this function was called and returned
    });
	int err = 0;
	struct addrinfo hints = {};
	struct addrinfo *res = NULL, *rp = NULL;
	char portstr[8];

	guard _cleanup_res([&]{
		if (res) freeaddrinfo(res);
	});

	snprintf(portstr, sizeof(portstr), "%u", _dPort);

	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	retassure(!(err = getaddrinfo(_ipaddr.c_str(), portstr, &hints, &res)), "failed to resolve '%s': %s", _ipaddr.c_str(), gai_strerror(err));

	for (rp = res; rp != NULL; rp = rp->ai_next) {
		if ((_dfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol)) < 0)
			continue;

		// For IPv6 link-local addresses, set the scope ID (interface index)
		if (rp->ai_family == AF_INET6 && _ifIndex != 0) {
			struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)rp->ai_addr;
			if (IN6_IS_ADDR_LINKLOCAL(&sin6->sin6_addr) && sin6->sin6_scope_id == 0) {
				sin6->sin6_scope_id = _ifIndex;
			}
		}

		if (::connect(_dfd, rp->ai_addr, rp->ai_addrlen) == 0)
			break;

		close(_dfd);
		_dfd = -1;
	}

	retassure(_dfd > 0, "failed to connect to device on port=%d errno=%d(%s)", _dPort, errno, strerror(errno));

    _cli->send_result(_cli->hdr->tag, RESULT_OK);
    _cfd = _cli->_fd;

    _cli->_fd = -1; //discard file descriptor, because the device (TCP) owns it now
    _cli = nullptr; // we don't need to keep a pointer in here anymore!
    debug("SockConn connected _cfd=%d _dfd=%d",_cfd,_dfd);

    _pfds = (struct pollfd*) malloc(sizeof(struct pollfd)*2);
    _pfds[0].fd = _cfd;
	_pfds[0].events = POLLIN;
    _pfds[1].fd = _dfd;
	_pfds[1].events = POLLIN;

    startLoop();
}


void SockConn::kill() noexcept{
    //sets _killInProcess to true and executes if statement if it was false before
    if (!_killInProcess.exchange(true)) {

        std::thread delthread([this](){
#ifdef DEBUG
            debug("killing SockConn (%p) C=%d D=%d",this,_cfd,_dfd);
#else
            info("killing SockConn C=%d D=%d",_cfd,_dfd);
#endif
            delete this;
        });
        delthread.detach();

    }
}


void SockConn::loopEvent(){
	int err = 0;
	ssize_t cnt = 0;
	char buf[0x4000];
	assure((err = poll(_pfds,2,-1)) != -1);

	for (int i = 0; i < 2; ++i){
		retassure((_pfds[i].revents & (~POLLIN)) == 0, "bad poll revents=0x%02x for fd=%d",_pfds[i].revents,i);
		if (_pfds[i].revents & POLLIN){
			retassure((cnt = read(_pfds[i].fd, buf, sizeof(buf)))>0, "read failed on fd=%d with cnt=%lld err=%s",_pfds[i],cnt,strerror(errno));
			retassure((cnt = write(_pfds[(i+1)&1].fd, buf, cnt))>0, "send failed on fd=%d with cnt=%lld err=%s",_pfds[i],cnt,strerror(errno));
		}
	}
}

void SockConn::afterLoop() noexcept{
	kill();
}
