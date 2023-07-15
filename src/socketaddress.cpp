#include <string.h>
#include "socketaddress.h"

void SocketAddress::from_ipv4(const struct addrinfo *res, unsigned port)
{
    addrlen = res->ai_addrlen;
    memcpy(&address, res->ai_addr, addrlen);
    reinterpret_cast<struct sockaddr_in *>(&address)->sin_port = htons(port);
}

void SocketAddress::from_ipv6(const struct addrinfo *res, unsigned port)
{
    addrlen = res->ai_addrlen;
    memcpy(&address, res->ai_addr, addrlen);
    reinterpret_cast<struct sockaddr_in6 *>(&address)->sin6_port = htons(port);
}

std::vector<SocketAddress>
SocketAddress::parse_addrinfo(const addrinfo *res, unsigned port)
{
    std::vector<SocketAddress> addresses;
    SocketAddress address;
    for (const addrinfo *r = res; r; r = r->ai_next)
        switch (r->ai_family) {
            case AF_INET:
                address.from_ipv4(r, port);
                addresses.push_back(address);
                break;
            case AF_INET6:
                address.from_ipv6(r, port);
                addresses.push_back(address);
                break;
            default:;
        }
    return addresses;
}
