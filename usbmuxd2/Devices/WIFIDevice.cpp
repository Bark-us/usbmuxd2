//
//  WIFIDevice.cpp
//  usbmuxd2
//
//  Created by tihmstar on 21.06.19.
//  Copyright © 2019 tihmstar. All rights reserved.
//

#include <Devices/WIFIDevice.hpp>
#include <SockConn.hpp>
#include <assert.h>
#include <log.h>
#include <libgeneral/macros.h>
#include <SockConn.hpp>
#include <string.h>
#include <Muxer.hpp>
#include <plist/plist.h>
#include <sysconf/sysconf.hpp>

const long LOCKDOWND_TIMEOUT = 30;

WIFIDevice::WIFIDevice(std::string uuid, std::string ipaddr, std::string serviceName, Muxer *mux)
: Device(mux,Device::MUXCONN_WIFI), _ipaddr(ipaddr), _serviceName(serviceName), _hbclient(NULL), _hbrsp(NULL),
	_idev(NULL)
{
    printf("USBMUXD: new device, uuid=%s, ipaddr=%s, serviceName=%s\n", uuid.c_str(), ipaddr.c_str(), serviceName.c_str());
	strncpy(_serial, uuid.c_str(), sizeof(_serial));
}

WIFIDevice::~WIFIDevice() {
    printf("USBMUXD: destroy wifi device\n");
    stopLoop();
    _muxer->delete_device(this);
    safeFreeCustom(_hbclient, heartbeat_client_free);
    safeFreeCustom(_hbrsp, plist_free);
    safeFreeCustom(_idev, idevice_free);
}

void WIFIDevice::loopEvent(){
    if (_hbclient) {
        plist_t hbeat = NULL;
        cleanup([&]{
            safeFreeCustom(hbeat, plist_free);
        });
        heartbeat_error_t hret = HEARTBEAT_E_SUCCESS;

        retassure((hret = heartbeat_receive_with_timeout(_hbclient,&hbeat,15000)) == HEARTBEAT_E_SUCCESS, "[WIFIDevice] failed to recv heartbeat with error=%d",hret);
        retassure((hret = heartbeat_send(_hbclient,_hbrsp)) == HEARTBEAT_E_SUCCESS,"[WIFIDevice] failed to send heartbeat");
    } else {
        retassure(checkLockdownRunning(), "[WIFIDevice] lost connection with lockdownd");
        waitForTimeout(LOCKDOWND_TIMEOUT);
    }
}

void WIFIDevice::beforeLoop(){

}

void WIFIDevice::afterLoop() noexcept{
	kill();
}

void WIFIDevice::startLoop(){
    heartbeat_error_t hret = HEARTBEAT_E_SUCCESS;
    _loopState = LOOP_STOPPED;

    retassure(idevice_new_with_options(&_idev,_serial, IDEVICE_LOOKUP_NETWORK) == IDEVICE_E_SUCCESS, "[WIFIDevice] Could not connect to ios device");
    hret = heartbeat_client_start_service(_idev, &_hbclient, "usbmuxd2");
    if (hret == HEARTBEAT_E_SUCCESS) {
        retassure(_hbrsp = plist_new_dict(), "[WIFIDevice] Could not create heartbeat response");
        plist_dict_set_item(_hbrsp, "Command", plist_new_string("Polo"));
    } else {
        warning("[WIFIDevice] Warning:  Could not start heartbeat.  Falling back to using lockdownd directly.");
    }

	_loopState = LOOP_UNINITIALISED;
    Manager::startLoop();
}


void WIFIDevice::start_connect(uint16_t dport, Client *cli){
	SockConn *conn = nullptr;
	cleanup([&]{
		if (conn){
			conn->kill();
		}
	});

    printf("USBMUXD: start connect, serial=%s, ipaddr=%s, dport=%d\n", _serial, _ipaddr.c_str(), dport);

	conn = new SockConn(_ipaddr,dport,cli);
	conn->connect();
	conn = nullptr; //let SockConn float and manage itself
}

bool WIFIDevice::checkLockdownRunning() {
    lockdownd_client_t client = NULL;
    lockdownd_error_t err = lockdownd_client_new_with_handshake(_idev, &client, "usbmuxd2");
    safeFreeCustom(client, lockdownd_client_free);
    return (err == LOCKDOWN_E_SUCCESS) || (err == LOCKDOWN_E_INVALID_HOST_ID);
}

void WIFIDevice::waitForTimeout(long timeout) {
    std::unique_lock<std::mutex> lck(_ld_mutex);
    long elapsed = 0;
    time_t begin, now;
    time(&begin);
    while ((_loopState == LOOP_RUNNING) && elapsed < timeout) {
        _ld_cv.wait_for(lck, std::chrono::seconds(timeout - elapsed));
        time(&now);
        elapsed = std::difftime(now, begin);
    }
}
