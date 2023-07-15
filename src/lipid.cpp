#include <filesystem>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <map>
#include <optional>
#include <vector>
#include <getopt.h>
#include <stdio.h>
#include <string.h>

#include <netinet/in.h>
#include <async/async.h>
#include <async/fsadns.h>
#include <async/tcp_connection.h>
#include <encjson.h>
#include <fstrace.h>

#include "coasync.h"

using std::exception;
using std::filesystem::path;
using std::map;
using std::optional;
using std::pair;
using std::string;
using std::unique_ptr;
using std::vector;
using std::cout;
using std::cerr;
using std::endl;

using fsecure::encjson::JsonThingPtr;
using pacujo::cordial::Thunk;
using pacujo::cordial::throw_errno;

template<typename T>
class Hold {
public:
    Hold(T *ptr, std::function<void(T *)> f) : ptr_(ptr, f) {}
    Hold(Hold &&other) : ptr_(other.ptr_) {}
    T *get() const { return ptr_.get(); }
    operator bool() const { return bool(ptr_); }
private:
    std::unique_ptr<T, std::function<void(T *)>> ptr_;
};

struct SocketAddress {
    sockaddr_storage address;
    socklen_t addrlen;

    void from_ipv4(const addrinfo *res, unsigned port);
    void from_ipv6(const addrinfo *res, unsigned port);
};

static vector<SocketAddress> parse_addrinfo(const addrinfo *res, unsigned port);

class TestException : public exception {
public:
    const char *what() const noexcept override { return "TestException"; }
};

class UsageException : public exception {
public:
    const char *what() const noexcept override { return "bad usage"; }
};

class NoHomeException : public exception {
public:
    const char *what() const noexcept override { return "HOME not defined"; }
};

class BadHomeException : public exception {
public:
    const char *what() const noexcept override { return "bad HOME"; }
};

class BadConfigException : public exception {
public:
    const char *what() const noexcept override {
        return "bad configuration file";
    }
};

class AddressResolutionException : public exception {
public:
    AddressResolutionException(int errcode) : errcode_ { errcode } {}
    const char *what() const noexcept override {
        return gai_strerror(errcode_);
    }

private:
    int errcode_;
};

struct UserSettings {
    std::string nick;
    std::string name;
    std::map<std::string, std::string> autojoins;
};

struct Local {
    std::string address { "0.0.0.0" };
    std::vector<SocketAddress> resolutions;
    int port { 11345 };
    std::optional<std::string> tls_server_name;
};

struct Config {
    std::map<std::string, UserSettings> settings;
    std::vector<Local> local;
    struct {
        std::string address { "irc.oftc.net" };
        int port { 6697 };
        bool use_tls { true };
    } irc_server;
    std::string cache_directory { ".cache/lipid/main" };
};

struct Opts {
    bool help {};
    std::optional<path> config_file;
    std::optional<std::string> trace_include;
    std::optional<std::string> trace_exclude;
};

class AddrInfo {
public:
    AddrInfo() {}
    AddrInfo(addrinfo *info) : info_ { info } {}
    AddrInfo(AddrInfo &&other) : info_ { other.info_ } {
        other.info_ = nullptr;
    }
    AddrInfo &operator=(AddrInfo &&other) {
        info_ = other.info_;
        other.info_ = nullptr;
        return *this;
    }
    ~AddrInfo() {
        if (info_)
            fsadns_freeaddrinfo(info_);
    }
    const addrinfo *get() const { return info_; }
private:
    addrinfo *info_ {};
};

class Session {
public:
    Session(tcp_conn_t *conn) : conn_ { conn, tcp_close } {}
    Session(Session &&other) = delete;
    Session(const Session &other) = delete;
    Session &operator=(const Session &other) = delete;
    Session &operator=(Session &&other) = delete;
private:
    Hold<tcp_conn_t> conn_;
};

class App : public pacujo::coasync::Framework {
public:
    App(const path &home_dir, const Opts &opts);
    ~App();
    void run(fstrace_t *trace);

private:
    path home_dir_;
    const Opts &opts_;
    Config config_;
    fsadns_t *resolver_;
    vector<Session> sessions_;

    void read_configuration(path config_file);
    Task run_server();
    Task resolve_addresses(const Thunk *notify);
    Future<AddrInfo> resolve_address(const Thunk *notify,
                                     const std::string &address);
    Task serve(const SocketAddress &address);
};

App::App(const path &home_dir, const Opts &opts) :
    home_dir_ { home_dir }, opts_ { opts }
{
}

App::~App()
{
    if (resolver_ != nullptr)
        fsadns_destroy_resolver(resolver_);
}

void App::run(fstrace_t *trace)
{
    if (opts_.config_file)
        read_configuration(*opts_.config_file);
    else read_configuration(home_dir_ / ".config/lipid/config.json");
    Thunk postfork = [trace]() { fstrace_reopen(trace); };
    resolver_ =
        fsadns_make_resolver(get_async(), 4, thunk_to_action(&postfork));
    auto server { run_server() };
    if (async_loop(get_async()) < 0)
        throw_errno();
}

void App::read_configuration(path config_file)
{
    Hold<FILE> cfgf { fopen(config_file.c_str(), "r"), fclose };
    if (!cfgf)
        throw_errno(config_file);
    JsonThingPtr cfg
        { json_utf8_decode_file(cfgf.get(), 1000000), json_destroy_thing };
    if (!cfg || json_thing_type(cfg.get()) != JSON_OBJECT)
        throw BadConfigException();
    json_thing_t *locals;
    if (json_object_get_array(cfg.get(), "local", &locals))
        for (auto e = json_array_first(locals); e; e = json_element_next(e)) {
            auto value = json_element_value(e);
            if (json_thing_type(value) != JSON_OBJECT)
                throw BadConfigException();
            Local local;
            const char *address;
            if (json_object_get_string(value, "address", &address))
                local.address = address;
            long long port;
            if (json_object_get_integer(value, "port", &port))
                local.port = port;
            const char *name;
            if (json_object_get_string(value, "tls-server-name", &name))
                local.tls_server_name = name;
            config_.local.push_back(local);
        }
    json_thing_t *irc_server;
    if (json_object_get_object(cfg.get(), "irc-server", &irc_server)) {
        const char *address;
        if (json_object_get_string(irc_server, "address", &address))
            config_.irc_server.address = address;
        long long port;
        if (json_object_get_integer(irc_server, "port", &port))
            config_.irc_server.port = port;
        bool use_tls;
        if (json_object_get_boolean(irc_server, "use_tls", &use_tls))
            config_.irc_server.use_tls = use_tls;
    }
}

static path find_home_dir()
{
    auto home_dir { ::getenv("HOME") };
    if (!home_dir)
        throw NoHomeException();
    if (!string(home_dir).starts_with("/"))
        throw BadHomeException();
    return home_dir;
}

App::Task App::run_server()
{
    auto [promise, wakeup] { co_await intro<Task::introspect>() };
    co_await resolve_addresses(wakeup);
    cout << "Got them!" << endl;
    vector<Task> holder;
    for (auto &local : config_.local)
        for (auto &res : local.resolutions)
            holder.push_back(serve(res));
    for (auto &_ : holder)
        co_await std::suspend_always {};
}

App::Task App::resolve_addresses(const Thunk *notify)
{
    auto [promise, wakeup] { co_await intro<Task::introspect>(notify) };
    vector<Future<AddrInfo>> resolve_local;
    for (auto &local : config_.local)
        resolve_local.emplace_back(resolve_address(wakeup, local.address));
    auto resolve_irc_server
        { resolve_address(wakeup, config_.irc_server.address) };
    for (auto &_ : resolve_local)
        co_await std::suspend_always {};
    co_await std::suspend_always {};
    auto it { config_.local.begin() };
    for (auto &local_fut : resolve_local) {
        auto &local = *it++;
        auto local_res { local_fut.await_resume() };
        local.resolutions = parse_addrinfo(local_res.get(), local.port);
    }
    auto remote_res { resolve_irc_server.await_resume() };
    auto remote_addresses
        { parse_addrinfo(remote_res.get(), config_.irc_server.port) };
}

App::Future<AddrInfo>
App::resolve_address(const Thunk *notify, const string &address)
{
    auto [promise, wakeup]
        { co_await intro<Future<AddrInfo>::introspect>(notify) };
    cout << "resolve " << address << endl;
    auto resolved { false };
    auto canceler {
        [&resolved](fsadns_query_t *query) {
            if (!resolved)
                fsadns_cancel(query);
        }
    };
    const struct addrinfo hint {
        .ai_socktype = SOCK_STREAM,
        .ai_protocol = IPPROTO_TCP,
    };
    Hold<fsadns_query_t> query {
        fsadns_resolve(resolver_, address.c_str(), nullptr, &hint,
                       thunk_to_action(wakeup)),
        canceler
    };
    addrinfo *res;
    for (;;) {
        auto status { fsadns_check(query.get(), &res) };
        if (status == 0) {
            resolved = true;
            cout << "resolved " << address << endl;
            co_return res;
        }
        if (status != EAI_SYSTEM)
            throw AddressResolutionException(status);
        if (errno != EAGAIN)
            throw_errno();
        co_await std::suspend_always {};
    }
}

App::Task App::serve(const SocketAddress &address) {
    auto [promise, wakeup] { co_await intro<Task::introspect>() };
    Hold<tcp_server_t> tcp_server {
        tcp_listen(get_async(),
                   reinterpret_cast<const sockaddr *>(&address.address),
                   address.addrlen),
        tcp_close_server
    };
    if (!tcp_server)
        throw_errno();
    tcp_register_server_callback(tcp_server.get(), thunk_to_action(wakeup));
    Hold<tcp_server_t> dereg
        { tcp_server.get(), tcp_unregister_server_callback };
    cout << "listening" << endl;
    for (;;) {
        auto tcp_conn = tcp_accept(tcp_server.get(), nullptr, nullptr);
        if (tcp_conn != nullptr) {
            cout << "connected" << endl;
            continue;
        }
        if (errno != EAGAIN)
            throw_errno();
        co_await std::suspend_always {};
    }
}

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

static vector<SocketAddress> parse_addrinfo(const addrinfo *res, unsigned port)
{
    vector<SocketAddress> addresses;
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

static void parse_cmdline(int argc, char **argv, Opts &opts)
{
    static option long_options[] = {
        { "config", required_argument, 0, 'c' },
        { "help", no_argument, 0, 'h' },
        { "trace-include", required_argument, 0, 'I' },
        { "trace-exclude", required_argument, 0, 'E' },
        { 0 }
    };
    auto done { false };
    while (!done) {
        auto option_index { 0 };
        auto opt = getopt_long(argc, argv, "c:h", long_options, &option_index);
        switch (opt) {
            case -1:
                done = true;
                break;
            case 'c':
                opts.config_file = optarg;
                break;
            case 'h':
                opts.help = true;
                done = true;
                break;
            case 'I':
                opts.trace_include = optarg;
                break;
            case 'E':
                opts.trace_exclude = optarg;
                break;
            default:
                throw UsageException();
        }
    }
    if (optind != argc)
        throw UsageException();
}

static fstrace_t *init_tracing(const Opts &opts)
{
    auto trace = fstrace_direct(stderr);
    fstrace_declare_globals(trace);
    fstrace_select_regex(trace,
                         opts.trace_include ?
                         opts.trace_include->c_str() :
                         nullptr,
                         opts.trace_exclude ?
                         opts.trace_exclude->c_str() :
                         nullptr);
    return trace;
}

static void print_usage(std::ostream &os)
{
    os << "Usage: lipid [ options ]" << endl;
    os << "Options:" << endl;
    os << " -h | --help                print help" << endl;
    os << " -c | --config CONFIG-FILE  use config file" << endl;
    os << " --trace-include REGEX      select trace events" << endl;
    os << " --trace-exclude REGEX      deselect trace events" << endl;
}

int main(int argc, char **argv)
{
    auto home_dir { find_home_dir() };
    Opts opts;
    try {
        parse_cmdline(argc, argv, opts);
    } catch (UsageException) {
        print_usage(cerr);
        return EXIT_FAILURE;
    }
    if (opts.help) {
        print_usage(cout);
        return EXIT_SUCCESS;
    }
    auto trace = init_tracing(opts);
    try {
        App(home_dir, opts).run(trace);
    } catch (const exception &e) {
        cerr << "lipid: " << e.what() << endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
