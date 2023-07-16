#include <cstdint>
#include <iostream>
#include <getopt.h>
#include <netinet/in.h>
#include <async/async.h>
#include <async/tcp_connection.h>
#include <async/tls_connection.h>
#include <async/queuestream.h>
#include <async/jsonyield.h>
#include <async/jsonencoder.h>
#include <async/naiveencoder.h>
#include <encjson.h>
#include <fstrace.h>

#include "app.h"
#include "hold.h"

using std::exception;
using std::filesystem::path;
using std::make_pair;
using std::map;
using std::optional;
using std::string;
using std::vector;

using std::cout;
using std::cerr;
using std::endl;

using pacujo::cordial::Thunk;
using pacujo::cordial::throw_errno;
using pacujo::etc::Hold;
using pacujo::net::SocketAddress;

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
    Hold<json_thing_t> cfg
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
            const char *certificate;
            if (json_object_get_string(value, "certificate", &certificate))
                local.certificate = certificate;
            const char *private_key;
            if (json_object_get_string(value, "private-key", &private_key))
                local.private_key = private_key;
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
    vector<Task> holder;
    for (auto &local : config_.local)
        for (auto &res : local.resolutions)
            holder.push_back(serve(res, local));
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
        local.resolutions =
            SocketAddress::parse_addrinfo(local_res.get(), local.port);
    }
    auto remote_res { resolve_irc_server.await_resume() };
    auto remote_addresses {
        SocketAddress::parse_addrinfo(remote_res.get(), config_.irc_server.port)
    };
}

App::Future<AddrInfo>
App::resolve_address(const Thunk *notify, const string &address)
{
    auto [promise, wakeup]
        { co_await intro<Future<AddrInfo>::introspect>(notify) };
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
            co_return res;
        }
        if (status != EAI_SYSTEM)
            throw AddressResolutionException(status);
        if (errno != EAGAIN)
            throw_errno();
        co_await std::suspend_always {};
    }
}

App::Task App::serve(const SocketAddress &address, const Local &local) {
    auto [promise, wakeup] { co_await intro<Task::introspect>() };
    Hold<tcp_server_t> tcp_server {
        tcp_listen(get_async(),
                   reinterpret_cast<const sockaddr *>(&address.address),
                   address.addrlen),
        tcp_close_server
    };
    if (!tcp_server)
        throw_errno();
    /* A negative notification indicates a possibility for
     * tcp_accept(). A non-negative notification is the key of a
     * session that has finished. */
    vector<int64_t> notifications;
    Thunk wakeup_accept {
        [wakeup, &notifications]() {
            notifications.push_back(-1);
            (*wakeup)();
        }
    };
    tcp_register_server_callback(tcp_server.get(),
                                 thunk_to_action(&wakeup_accept));
    Hold<tcp_server_t> dereg
        { tcp_server.get(), tcp_unregister_server_callback };
    for (;;) {
        for (auto key : notifications)
            if (key >= 0)
                sessions_.erase(sessions_.find(key));
        notifications.clear();
        for (;;) {
            Hold<tcp_conn_t> tcp_conn
                { tcp_accept(tcp_server.get(), nullptr, nullptr), tcp_close };
            if (!tcp_conn)
                break;
            auto key { next_session_++ };
            Thunk wakeup_end {
                [key, wakeup, &notifications]() {
                    notifications.push_back(key);
                    (*wakeup)();
                }
            };
            sessions_.emplace(make_pair<int64_t, Session>(std::move(key),
                                                          { wakeup_end }));
            auto &[_, session] = *sessions_.find(key);
            session.set_task(run_session(session.get_wakeup(),
                                         std::move(tcp_conn),
                                         local));
        }
        if (errno != EAGAIN)
            throw_errno();
        co_await std::suspend_always {};
    }
}

class ByteStream {
public:
    ByteStream(queuestream_t *qstr) :
        bs_ { queuestream_as_bytestream_1(qstr) } {}
    ByteStream(jsonencoder_t *encoder) :
        bs_ { jsonencoder_as_bytestream_1(encoder) } {}
    ByteStream(naiveencoder_t *encoder) :
        bs_ { naiveencoder_as_bytestream_1(encoder) } {}

    operator bytestream_1() const { return bs_; }
    
private:
    bytestream_1 bs_;
};

App::Task
App::run_session(const Thunk *notify, Hold<tcp_conn_t> tcp_conn,
                 const Local &local)
{
    auto [promise, wakeup] { co_await intro<Task::introspect>(notify) };
    Hold<tls_conn_t> tls_conn
        {
            open_tls_server(get_async(),
                            tcp_get_input_stream(tcp_conn.get()),
                            local.certificate.c_str(),
                            local.private_key.c_str()),
            tls_close
        };
    auto responses { make_queuestream(get_async()) };
    tls_set_plain_output_stream(tls_conn.get(), ByteStream { responses });
    tcp_set_output_stream(tcp_conn.get(),
                          tls_get_encrypted_output_stream(tls_conn.get()));
    Hold<jsonyield_t> requests
        {
            open_jsonyield(get_async(),
                           tls_get_plain_input_stream(tls_conn.get()),
                           100'000),
            jsonyield_close
        };
    jsonyield_register_callback(requests.get(), thunk_to_action(wakeup));
    Hold<jsonyield_t> dereg { requests.get(), jsonyield_unregister_callback };
    for (;;) {
        for (;;) {
            Hold<json_thing_t> request
                { jsonyield_receive(requests.get()), json_destroy_thing };
            if (!request)
                break;
            // echo back
            send(responses, request.get());
        }
        if (errno != EAGAIN)
            break;
        co_await std::suspend_always {};
    }
    if (errno != 0)
        throw_errno();
    queuestream_terminate(responses);
}

void App::send(queuestream_t *qstr, json_thing_t *msg)
{
    ByteStream encoder { json_encode(get_async(), msg) };
    ByteStream framer { naive_encode(get_async(), encoder, 0, 0) };
    queuestream_enqueue(qstr, framer);
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
