//
//  WIFIDeviceManager-mDNS.cpp
//  usbmuxd2
//
//  Created by tihmstar on 26.09.19.
//  Copyright © 2019 tihmstar. All rights reserved.
//


#include <log.h>
#include <libgeneral/macros.h>

#include "WIFIDeviceManager-mDNS.hpp"
#include <Devices/WIFIDevice.hpp>
#include <sysconf/sysconf.hpp>
#include <Devices/WIFIDevice.hpp>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/select.h>


#ifdef HAVE_WIFI_MDNS

#pragma mark definitions

#define kDNSServiceInterfaceIndexAny 0
#define kDNSServiceInterfaceIndexLocalOnly ((uint32_t)-1)
#define kDNSServiceInterfaceIndexUnicast   ((uint32_t)-2)
#define kDNSServiceInterfaceIndexP2P       ((uint32_t)-3)
#define kDNSServiceInterfaceIndexBLE       ((uint32_t)-4)


#define kDNSServiceFlagsAdd     0x2
#define LONG_TIME 100000000



#pragma mark callbacks
void resolve_reply(DNSServiceRef sdRef, DNSServiceFlags flags, uint32_t interfaceIndex, DNSServiceErrorType errorCode, const char *fullname, const char *hosttarget, uint16_t port, uint16_t txtLen, const unsigned char *txtRecord, void *context)noexcept{

    int err = 0;
    WIFIDeviceManager *wifimgr = (WIFIDeviceManager*)context;
    WIFIDevice *dev = nullptr;
    struct addrinfo hints = {};
    struct addrinfo *res = NULL, *rp = NULL;
    std::string ipaddr;


    debug("Service '%s' at '%s' (interface %u):\n", fullname, hosttarget, interfaceIndex);
    std::string serviceName{fullname};
    std::string macAddr{serviceName.substr(0,serviceName.find("@"))};
    std::string uuid;

    try{
        uuid = sysconf_udid_for_macaddr(macAddr);
    }catch (tihmstar::exception &e){
        creterror("failed to find uuid for mac=%s with error=%d (%s)",macAddr.c_str(),e.code(),e.what());
    }

    if (!wifimgr->_mux->have_wifi_device(macAddr)) {
        // found new device

        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        if (getaddrinfo(hosttarget, NULL, &hints, &res) != 0 || !res) {
            creterror("failed to resolve hosttarget '%s'", hosttarget);
        }

        // Pick the best address: prefer IPv6 link-local (most reliable for iOS WiFi),
        // then any IPv6, then IPv4
        struct addrinfo *best = NULL;
        for (rp = res; rp != NULL; rp = rp->ai_next) {
            if (rp->ai_family == AF_INET6) {
                struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)rp->ai_addr;
                if (IN6_IS_ADDR_LINKLOCAL(&sin6->sin6_addr)) {
                    best = rp;
                    break; // link-local is preferred
                }
                if (!best) best = rp;
            } else if (rp->ai_family == AF_INET && !best) {
                best = rp;
            }
        }

        if (!best) best = res;

        if (best->ai_family == AF_INET6) {
            char addrstr[INET6_ADDRSTRLEN];
            struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)best->ai_addr;
            inet_ntop(AF_INET6, &sin6->sin6_addr, addrstr, sizeof(addrstr));
            ipaddr = addrstr;
        } else {
            char addrstr[INET_ADDRSTRLEN];
            struct sockaddr_in *sin4 = (struct sockaddr_in *)best->ai_addr;
            inet_ntop(AF_INET, &sin4->sin_addr, addrstr, sizeof(addrstr));
            ipaddr = addrstr;
        }

        freeaddrinfo(res); res = NULL;

        debug("Resolved '%s' to '%s' (interface %u)\n", hosttarget, ipaddr.c_str(), interfaceIndex);

        try{
            dev = new WIFIDevice(uuid, ipaddr.c_str(), serviceName, wifimgr->_mux, interfaceIndex);
        } catch (tihmstar::exception &e){
            creterror("failed to construct device with error=%d (%s)",e.code(),e.what());
        }
        wifimgr->_mux->add_device(dev); dev = NULL;
    }

error:
    if (res) freeaddrinfo(res);
    if (err) {
        error("resolve_reply failed with error=%d",err);
    }
}


void browse_reply(DNSServiceRef sdref, const DNSServiceFlags flags, uint32_t ifIndex, DNSServiceErrorType errorCode, const char *replyName, const char *replyType, const char *replyDomain, void *context) noexcept{
    int err = 0;
    DNSServiceErrorType res = 0;
    WIFIDeviceManager *wifimgr = (WIFIDeviceManager*)context;
    DNSServiceRef resolvClient = NULL;
    int resolvfd = -1;
    
    if (!(flags & kDNSServiceFlagsAdd)) {
        debug("ignoring event=%d. We only care about Add events at the moment",flags);
        return;
    }
    
    const char *op = (flags & kDNSServiceFlagsAdd) ? "Add" : "Rmv";
        printf("%s %8X %3d %-20s %-20s %s\n",
               op, flags, ifIndex, replyDomain, replyType, replyName);

    cassure(!(res = DNSServiceResolve(&resolvClient, 0, kDNSServiceInterfaceIndexAny, replyName, replyType, replyDomain, resolve_reply, wifimgr)));
    
    cassure((resolvfd = DNSServiceRefSockFD(resolvClient))>0);
    FD_SET(resolvfd, &wifimgr->_readfds);
    
    if (resolvfd>wifimgr->_nfds) wifimgr->_nfds = resolvfd;
    
    wifimgr->_resolveClients.push_back(resolvClient);
    
error:
    if (err) {
        error("browse_reply failed with error=%d",err);
    }
}


#pragma mark WIFIDevice

WIFIDeviceManager::WIFIDeviceManager(Muxer *mux): DeviceManager(mux), _client(NULL), _dns_sd_fd(-1), _readfds{}, _nfds(0), _tv{}{
    int err = 0;
    debug("WIFIDeviceManager mDNS-client");
    assure(!(err = DNSServiceBrowse(&_client, 0, kDNSServiceInterfaceIndexAny, "_apple-mobdev2._tcp", "", browse_reply, this)));

    assure((_dns_sd_fd = DNSServiceRefSockFD(_client))>0);

    FD_ZERO(&_readfds);
    FD_SET(_dns_sd_fd, &_readfds);
    _nfds = _dns_sd_fd;
    
    _tv.tv_sec  = LONG_TIME;
    _tv.tv_usec = 0;
}

WIFIDeviceManager::~WIFIDeviceManager(){
    stopLoop();

    DNSServiceRefDeallocate(_client);
}

void WIFIDeviceManager::loopEvent(){
    int res = 0;
    res = select(_nfds+1, &_readfds, (fd_set*)NULL, (fd_set*)NULL, &_tv);
    if (res > 0){
        std::vector<DNSServiceRef> removeClients;
        cleanup([&]{
            for (auto &rc : removeClients) {
                const auto target = std::remove(_resolveClients.begin(), _resolveClients.end(), rc);
                if (target != _resolveClients.end()){
                    _resolveClients.erase(target, _resolveClients.end());
                    DNSServiceRefDeallocate(*target);
                }
            }
        });
        DNSServiceErrorType err = 0;
        if (FD_ISSET(_dns_sd_fd, &_readfds))
            assure(!(err |= DNSServiceProcessResult(_client)));
        
        for (auto &rc : _resolveClients) {
            int rcfd = DNSServiceRefSockFD(rc);
            if (rcfd != -1 && FD_ISSET(rcfd, &_readfds)){
                assure(!(err |= DNSServiceProcessResult(rc)));
                FD_CLR(rcfd, &_readfds);
                removeClients.push_back(rc);
            }
        }
    }else if (res != 0){
        reterror("select() returned %d errno %d %s\n", res, errno, strerror(errno));
    }
}

#endif //HAVE_WIFI_MDNS
