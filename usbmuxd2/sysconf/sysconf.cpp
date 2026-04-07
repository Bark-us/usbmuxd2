//
//  sysconf.cpp
//  usbmuxd2
//
//  Created by tihmstar on 18.08.19.
//  Copyright © 2019 tihmstar. All rights reserved.
//

#include "sysconf.hpp"
#include <log.h>
#include <libgeneral/macros.h>
#include <libgeneral/exception.hpp>
#include <sys/stat.h>
#include <errno.h>
#include <libgen.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <map>
#include <plist/plist.h>
#include <vector>
#include <ctime>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#ifdef HAVE_FILESYSTEM
#include <filesystem>
#else
#include <dirent.h>
#endif //HAVE_FILESYSTEM

#define CONFIG_DIR  "lockdown"
#define CONFIG_FILE "SystemConfiguration"

#define CONFIG_SYSTEM_BUID_KEY "SystemBUID"
#define CONFIG_HOST_ID_KEY "HostID"


#ifdef __APPLE__
#   define BASE_CONFIG_DIR "/var/db"
#else
#   define BASE_CONFIG_DIR "/var/lib"
#endif


#ifndef HAVE_FILESYSTEM
//really crappy implementation in case <filesystem> isn't available :o
class myfile{
    std::string _path;
public:
    myfile(std::string p): _path(p){}
    std::string path(){return _path;}
};

class diriter{
public:
    std::vector<myfile> _file;
    auto begin(){return _file.begin();}
    auto end(){return _file.end();}
};

namespace std {
    namespace filesystem{
        diriter directory_iterator(std::string);
    }
}

diriter std::filesystem::directory_iterator(std::string dirpath){
    DIR *dir = NULL;
    struct dirent *ent = NULL;
    diriter ret;

    assure(dir = opendir(dirpath.c_str()));

    while ((ent = readdir (dir)) != NULL) {
        if (ent->d_type != DT_REG)
            continue;
        ret._file.push_back({dirpath + "/" + ent->d_name});
    }

    if (dir) closedir(dir);
    return ret;
}
#endif



static std::map<std::string,std::string> gKnownMacAddrs;

static void sysconf_load_known_macaddrs();


constexpr const char *sysconf_get_config_dir(){
    return BASE_CONFIG_DIR "/" CONFIG_DIR;
}

static char *sysconf_generate_system_buid(){
    char *uuid = (char *) malloc(sizeof(char) * 37);
    const char *chars = "ABCDEF0123456789";
    srand((unsigned)time(NULL));
    int i = 0;

    for (i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            uuid[i] = '-';
            continue;
        } else {
            uuid[i] = chars[random() % 16];
        }
    }

    uuid[36] = '\0';
    return uuid;
}

static void mkdir_with_parents(const char *dir, int mode){
    char *parent = NULL;
    char* parentdir = NULL; //not allocated
    cleanup([&]{
        safeFree(parent);
    });
    assure(dir);
    if (mkdir(dir, mode) == 0 || errno == EEXIST) {
        return;
    }

    parent = strdup(dir);
    assure(parentdir = dirname(parent));
    mkdir_with_parents(parentdir, mode);
}



plist_t readPlist(const char *filePath){
    int fd = 0;
    struct stat finfo{};
    char *fbuf = NULL;
    plist_t pl = NULL;
    cleanup([&]{
        if (fd) {
            close(fd);
        }
        safeFree(fbuf);
    });

    retassure((fd = open(filePath, O_RDONLY))>0, "Failed to read plist at path '%s'",filePath);
    assure(!fstat(fd, &finfo));

    assure(fbuf = (char*)malloc(finfo.st_size));

    assure(read(fd, fbuf, finfo.st_size) == finfo.st_size);

    if (memcmp(fbuf, "bplist00", 8) == 0) {
        plist_from_bin(fbuf, (uint32_t)finfo.st_size, &pl);
    } else {
        plist_from_xml(fbuf, (uint32_t)finfo.st_size, &pl);
    }

    return pl;
}

void writePlistToFile(plist_t plist, const char *dst){
    char *buf = NULL;
    FILE * saveFile = NULL;
    cleanup([&]{
        safeFree(buf);
        safeFreeCustom(saveFile, fclose);
    });
    uint32_t bufLen = 0;
    plist_to_xml(plist, &buf, &bufLen);

    retassure(saveFile = fopen(dst, "w"), "Failed to write plist file to=%s",dst);
    assure(fwrite(buf, 1, bufLen, saveFile) == bufLen);
}

static void sysconf_create_config_dir(void){
    struct stat st{};
    constexpr const char *config_path = sysconf_get_config_dir();

    if (stat(config_path, &st) != 0) {
        mkdir_with_parents(config_path, 0755);
    }
}

char *get_device_record_path(const char *udid){
    constexpr const char *config_path = sysconf_get_config_dir();
    size_t filepathSize = 0;
    char *filepath = NULL;

    assure(udid);

    sysconf_create_config_dir();

    filepathSize = strlen(config_path) + strlen(udid) + sizeof("/.plist");
    assure(filepath = (char*)malloc(filepathSize));

    snprintf(filepath, filepathSize, "%s/%s.plist", config_path,udid);
    return filepath;
}

plist_t sysconf_get_device_record(const char *udid){
    char *filepath = NULL;
    cleanup([&]{
        safeFree(filepath);
    });
    filepath = get_device_record_path(udid);

    return readPlist(filepath);
}


void sysconf_set_device_record(const char *udid, const plist_t record){
    char *filepath = NULL;
    std::string xmlRecord;
    cleanup([&]{
        safeFree(filepath);
    });
    assure(udid);
    assure(record);
    filepath = get_device_record_path(udid);

    writePlistToFile(record, filepath);
    sysconf_load_known_macaddrs();
}

void sysconf_remove_device_record(const char *udid){
    char *filepath = NULL;
    cleanup([&]{
        safeFree(filepath);
    });
    filepath = get_device_record_path(udid);

    retassure(!remove(filepath), "could not remove %s: %s", filepath, strerror(errno));
    sysconf_load_known_macaddrs();
}


//allocated a Node
plist_t sysconf_get_value(const std::string &key){
    char *filepath = NULL;
    plist_t p_devrecord = NULL;
    cleanup([&]{
        safeFree(filepath);
        safeFreeCustom(p_devrecord, plist_free);
    });
    plist_t p_val = NULL;
    filepath = get_device_record_path(CONFIG_FILE);

    p_devrecord = readPlist(filepath);

    retassure(p_val = plist_dict_get_item(p_devrecord, key.c_str()), "Failed to get value for key '%s'",key.c_str());

    return plist_copy(p_val);
}

void sysconf_set_value(const std::string &key, plist_t val){
    char *filepath = NULL;
    plist_t p_sysconf = NULL;
    cleanup([&]{
        safeFree(filepath);
        safeFreeCustom(p_sysconf, plist_free);
    });
    filepath = get_device_record_path(CONFIG_FILE);

    try {
        p_sysconf = readPlist(filepath);
    } catch (tihmstar::exception &e) {
        warning("%s: Reading %s failed! Regenerating!",__func__,CONFIG_FILE);
        p_sysconf = plist_new_dict();
    }

    plist_dict_set_item(p_sysconf, key.c_str(), val);
    writePlistToFile(p_sysconf, filepath);
}

std::string sysconf_get_system_buid(){
    plist_t p_buid = NULL;
    cleanup([&]{
        safeFreeCustom(p_buid, plist_free);
    });
    const char *buid_str = NULL;
    uint64_t buid_str_len = 0;

    try {
        p_buid = sysconf_get_value(CONFIG_SYSTEM_BUID_KEY);
    } catch (tihmstar::exception &e) {
        warning("Failed to get SystemBuid! regenerating %s",CONFIG_FILE);
        std::string buid = sysconf_generate_system_buid();
        p_buid = plist_new_string(buid.c_str());
        sysconf_set_value(CONFIG_SYSTEM_BUID_KEY, p_buid);
    }

    retassure(buid_str = plist_get_string_ptr(p_buid, &buid_str_len), "Failed to get str ptr from build");

    return std::string(buid_str,buid_str_len);
}

static void sysconf_load_known_macaddrs(){
    constexpr const char *config_path = sysconf_get_config_dir();
    char *sysconfigpath = NULL;
    cleanup([&]{
        safeFree(sysconfigpath);
    });

    sysconf_create_config_dir();

    gKnownMacAddrs.clear();

    sysconfigpath = get_device_record_path(CONFIG_FILE);

    for(auto& p : std::filesystem::directory_iterator(config_path)){
        if (p.path() == sysconfigpath)
            continue; //ignore sysconfig file
        debug("reading file=%s\n",p.path().c_str());
        try{ //we ignore any error happening in here
            plist_t p_devrecord = NULL;
            cleanup([&]{
                safeFreeCustom(p_devrecord, plist_free);
            });
            plist_t p_macaddr = NULL;
            std::string macaddr;

            p_devrecord = readPlist(p.path().c_str());

            retassure(p_macaddr = plist_dict_get_item(p_devrecord, "WiFiMACAddress"), "Failed to read macaddr from pairing record");

            {
                const char *str = NULL;
                uint64_t str_len = 0;
                retassure(str = plist_get_string_ptr(p_macaddr, &str_len), "Faile to get str ptr from MacAddress");
                macaddr = std::string(str,str_len);
            }

            std::string path = p.path();

            size_t lastSlashPos = path.find_last_of("/")+1;
            size_t dotPos = path.find(".");

            std::string uuid = path.substr(lastSlashPos,dotPos-lastSlashPos);
            debug("adding macaddr=%s for uuid=%s",macaddr.c_str(),uuid.c_str());

            gKnownMacAddrs[macaddr] = uuid;

        } catch (tihmstar::exception &e){
            debug("failed to read record with error=%d (%s)",e.code(),e.what());
        }
    }
}

std::string sysconf_udid_for_macaddr(std::string macaddr){
    if (!gKnownMacAddrs.size()){
        sysconf_load_known_macaddrs();
    }
    try{
        return gKnownMacAddrs.at(macaddr);
    }catch (...){
        reterror("macaddr=%s is not paired",macaddr.c_str());
    }
}

void sysconf_add_macaddr_mapping(std::string macaddr, std::string udid){
    if (!gKnownMacAddrs.size()){
        sysconf_load_known_macaddrs();
    }
    gKnownMacAddrs[macaddr] = udid;
    debug("cached private macaddr=%s for uuid=%s",macaddr.c_str(),udid.c_str());
}

std::vector<std::string> sysconf_get_known_udids(){
    if (!gKnownMacAddrs.size()){
        sysconf_load_known_macaddrs();
    }
    std::vector<std::string> udids;
    for (const auto &pair : gKnownMacAddrs){
        udids.push_back(pair.second);
    }
    return udids;
}

// Helper: send a lockdownd plist request and receive the response
static plist_t lockdownd_send_recv(int sfd, plist_t req){
    char *xml = NULL;
    uint32_t xml_len = 0;
    plist_to_xml(req, &xml, &xml_len);
    if (!xml) return NULL;

    uint32_t net_len = htonl(xml_len);
    bool ok = (send(sfd, &net_len, 4, 0) == 4 && send(sfd, xml, xml_len, 0) == (ssize_t)xml_len);
    free(xml);
    if (!ok) return NULL;

    uint32_t resp_len = 0;
    if (recv(sfd, &resp_len, 4, MSG_WAITALL) != 4) return NULL;
    resp_len = ntohl(resp_len);
    if (resp_len == 0 || resp_len > 65536) return NULL;

    std::vector<char> buf(resp_len);
    ssize_t total = 0;
    while (total < (ssize_t)resp_len) {
        ssize_t n = recv(sfd, buf.data() + total, resp_len - total, 0);
        if (n <= 0) break;
        total += n;
    }
    if (total != (ssize_t)resp_len) return NULL;

    plist_t resp = NULL;
    plist_from_xml(buf.data(), resp_len, &resp);
    return resp;
}

// Connect to a device's lockdownd and try StartSession with each known pairing
// record to identify which device this is. Returns the matching UDID or empty string.
std::string sysconf_probe_lockdownd_udid(const char *ipaddr, uint32_t ifindex){
    std::string udid;
    std::vector<std::string> udids = sysconf_get_known_udids();
    std::string buid = sysconf_get_system_buid();

    for (const auto &candidate : udids) {
        int sfd = -1;
        struct addrinfo hints = {}, *res = NULL;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        if (getaddrinfo(ipaddr, "62078", &hints, &res) != 0 || !res) return udid;

        for (struct addrinfo *rp = res; rp; rp = rp->ai_next) {
            sfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
            if (sfd < 0) continue;

            if (rp->ai_family == AF_INET6 && ifindex != 0) {
                struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)rp->ai_addr;
                if (IN6_IS_ADDR_LINKLOCAL(&sin6->sin6_addr) && sin6->sin6_scope_id == 0) {
                    sin6->sin6_scope_id = ifindex;
                }
            }

            struct timeval tv = {5, 0};
            setsockopt(sfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(sfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

            if (connect(sfd, rp->ai_addr, rp->ai_addrlen) == 0) break;
            close(sfd);
            sfd = -1;
        }
        freeaddrinfo(res);

        if (sfd < 0) continue;

        // lockdownd requires QueryType as the initial handshake
        plist_t p_query = plist_new_dict();
        plist_dict_set_item(p_query, "Label", plist_new_string("usbmuxd2-probe"));
        plist_dict_set_item(p_query, "Request", plist_new_string("QueryType"));
        plist_t p_qresp = lockdownd_send_recv(sfd, p_query);
        plist_free(p_query);
        if (!p_qresp) { close(sfd); continue; }
        plist_free(p_qresp);

        // Read HostID from this candidate's pairing record
        plist_t p_record = NULL;
        try {
            p_record = sysconf_get_device_record(candidate.c_str());
        } catch (...) {
            close(sfd);
            continue;
        }
        if (!p_record) { close(sfd); continue; }

        plist_t p_hostid = plist_dict_get_item(p_record, "HostID");
        const char *hostid_str = NULL;
        uint64_t hostid_len = 0;
        if (p_hostid) hostid_str = plist_get_string_ptr(p_hostid, &hostid_len);

        // Try StartSession with this pairing record's HostID
        plist_t p_start = plist_new_dict();
        plist_dict_set_item(p_start, "Label", plist_new_string("usbmuxd2-probe"));
        plist_dict_set_item(p_start, "Request", plist_new_string("StartSession"));
        if (hostid_str) plist_dict_set_item(p_start, "HostID", plist_new_string(hostid_str));
        plist_dict_set_item(p_start, "SystemBUID", plist_new_string(buid.c_str()));

        plist_t p_sresp = lockdownd_send_recv(sfd, p_start);
        plist_free(p_start);
        plist_free(p_record);
        close(sfd);

        if (p_sresp) {
            plist_t p_err = plist_dict_get_item(p_sresp, "Error");
            if (!p_err) {
                // StartSession succeeded — this is the device
                udid = candidate;
                info("StartSession succeeded for UDID %s", candidate.c_str());
                plist_free(p_sresp);
                break;
            }
            plist_free(p_sresp);
        }
    }

    return udid;
}

void sysconf_fix_permissions(int uid, int gid){
    constexpr const char *config_path = sysconf_get_config_dir();

    sysconf_create_config_dir();

    assure(!chown(config_path, uid, gid));

    for(auto& p: std::filesystem::directory_iterator(config_path)){
        assure(!chown(p.path().c_str(), uid, gid));
   }
}


#pragma mark config


bool sysconf_try_getconfig_bool(std::string key, bool defaultValue){
    plist_t p_boolVal = NULL;
    cleanup([&]{
        safeFreeCustom(p_boolVal, plist_free);
    });
    try {
        p_boolVal = sysconf_get_value(key);
        assure(plist_get_node_type(p_boolVal) == PLIST_BOOLEAN);
        return plist_bool_val_is_true(p_boolVal);
    } catch (tihmstar::exception &e) {
        warning("Failed to get %s! setting it to default val",key.c_str());
        p_boolVal = plist_new_bool(defaultValue);
        sysconf_set_value(key, p_boolVal);
        return defaultValue;
    }
}


Config::Config() :
//commandline
enableExit(false),
daemonize(false),
useLogfile(false),
debugLevel(0)
{
    //empty
}

void Config::load(){
    //config
    doPreflight = sysconf_try_getconfig_bool("doPreflight",true);
    enableWifiDeviceManager = sysconf_try_getconfig_bool("enableWifiDeviceManager",true);
    enableUSBDeviceManager = sysconf_try_getconfig_bool("enableUSBDeviceManager",true);
    debug("Loaded config");
}
