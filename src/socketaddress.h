#pragma once

#include <vector>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

namespace pacujo::net {

struct SocketAddress {
    sockaddr_storage address;
    socklen_t addrlen;

    void from_ipv4(const addrinfo *res, unsigned port);
    void from_ipv6(const addrinfo *res, unsigned port);

    static std::vector<SocketAddress> parse_addrinfo(const addrinfo *res,
                                                     unsigned port);
};

};
