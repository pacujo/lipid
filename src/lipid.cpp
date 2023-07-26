#include <chrono>
#include <cstdint>
#include <iostream>
#include <getopt.h>
#include <netinet/in.h>
#include <openssl/sha.h>
#include <async/async.h>
#include <async/tcp_client.h>
#include <async/tls_connection.h>
#include <async/queuestream.h>
#include <async/stringstream.h>
#include <async/jsonyield.h>
#include <async/jsonencoder.h>
#include <async/naiveencoder.h>
#include <encjson.h>
#include <fsdyn/base64.h>
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

static const std::suspend_always suspend;

using std::cout;
using std::cerr;
using std::endl;

using pacujo::cordial::Thunk;
using pacujo::cordial::throw_errno;
using pacujo::etc::Hold;
using pacujo::etc::Keep;
using pacujo::net::SocketAddress;

using namespace std::chrono_literals;

App::App(const path &home_dir, const Opts &opts) :
    home_dir_ { home_dir }, opts_ { opts }
{
}

void App::run(fstrace_t *trace)
{
    if (opts_.config_file)
        read_configuration(*opts_.config_file);
    else read_configuration(home_dir_ / ".config/lipid/config.json");
    Thunk postfork = [trace]() { fstrace_reopen(trace); };
    resolver_ = Hold<fsadns_t> {
        fsadns_make_resolver(get_async(), 4, thunk_to_action(&postfork)),
        fsadns_destroy_resolver
    };
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
    if (json_object_get_array(cfg.get(), "locals", &locals))
        add_locals(locals);
    json_thing_t *clients;
    if (json_object_get_object(cfg.get(), "clients", &clients))
        add_clients(clients);
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

void App::add_locals(json_thing_t *locals)
{
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
}

void App::add_clients(json_thing_t *clients)
{
    for (auto field = json_object_first(clients); field;
         field = json_field_next(field)) {
        auto name { json_field_name(field) };
        auto value { json_field_value(field) };
        if (json_thing_type(value) != JSON_OBJECT)
            throw BadConfigException();
        Client client;
        const char *salt;
        if (json_object_get_string(value, "salt", &salt))
            client.salt = salt;
        const char *sha256;
        if (json_object_get_string(value, "sha256", &sha256))
            client.sha256 = sha256;
        config_.clients[name] = client;
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
    auto wakeup { co_await intro<Task::introspect>() };
    co_await resolve_addresses(wakeup);
    vector<Task> holder;
    for (auto &local : config_.local)
        for (auto &res : local.resolutions)
            holder.push_back(serve(wakeup, res, local));
    for (auto &_ : holder)
        co_await suspend;       // TODO: call await_resume()
}

App::Task App::resolve_addresses(const Thunk *notify)
{
    auto wakeup { co_await intro<Task::introspect>(notify) };
    vector<Future<AddrInfo>> resolve_local;
    for (auto &local : config_.local)
        resolve_local.emplace_back(resolve_address(wakeup, local.address));
    for (auto &_ : resolve_local)
        co_await suspend;
    auto it { config_.local.begin() };
    for (auto &local_fut : resolve_local) {
        auto &local = *it++;
        auto local_res { local_fut.await_resume() };
        local.resolutions =
            SocketAddress::parse_addrinfo(local_res.get(), local.port);
    }
}

App::Future<AddrInfo> App::resolve_address(const Thunk *notify,
                                           const string &address)
{
    auto wakeup { co_await intro<Future<AddrInfo>::introspect>(notify) };
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
        fsadns_resolve(resolver_->get(), address.c_str(), nullptr, &hint,
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
        co_await suspend;
    }
}

App::Task App::serve(const Thunk *notify, const SocketAddress &address,
                     const Local &local)
{
    auto wakeup { co_await intro<Task::introspect>(notify) };
    Hold<tcp_server_t> tcp_server {
        tcp_listen(get_async(),
                   reinterpret_cast<const sockaddr *>(&address.address),
                   address.addrlen),
        tcp_close_server
    };
    if (!tcp_server)
        throw_errno();
    vector<int64_t> notifications;
    auto connected { false };
    Thunk wakeup_accept {
        [wakeup, &connected]() {
            connected = true;
            (*wakeup)();
        }
    };
    auto listener { accept(&wakeup_accept, tcp_server.get()) };
    listener.await_suspend();
    for (;;) {
        co_await suspend;
        if (connected) {
            auto tcp_conn { listener.await_resume() };
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
            connected = false;
            listener = accept(&wakeup_accept, tcp_server.get());
            listener.await_suspend();
        }
        for (auto key : notifications) {
            auto it { sessions_.find(key) };
            try {
                it->second.get_task()->await_resume();
                cerr << "session " << key << " ended" << endl;
            } catch (const exception &e) {
                cerr << "session " << key << " crashed: "
                     << e.what() << endl;
            }
            sessions_.erase(it);
        }
        notifications.clear();
    }
}

App::Future<Hold<tcp_conn_t>> App::accept(const Thunk *notify,
                                          tcp_server_t *tcp_server)
{
    auto wakeup
        { co_await intro<Future<Hold<tcp_conn_t>>::introspect>(notify) };
    tcp_register_server_callback(tcp_server, thunk_to_action(wakeup));
    Hold<tcp_server_t> dereg { tcp_server, tcp_unregister_server_callback };
    for (;;) {
        Hold<tcp_conn_t> tcp_conn
            { tcp_accept(tcp_server, nullptr, nullptr), tcp_close };
        if (tcp_conn)
            co_return tcp_conn;
        if (errno != EAGAIN)
            throw_errno();
        co_await suspend;
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

class ClientStack {
public:
    ClientStack(async_t *async, Hold<tcp_conn_t> tcp_conn, const Local &local) :
        tcp_conn_ { std::move(tcp_conn) },
        tls_conn_ {
            open_tls_server(async,
                            tcp_get_input_stream(tcp_conn_.get()),
                            local.certificate.c_str(),
                            local.private_key.c_str()),
            tls_close
        },
        responses_ { make_queuestream(async),  queuestream_terminate },
        requests_ {
            open_jsonyield(async,
                           tls_get_plain_input_stream(tls_conn_.get()),
                           100'000),
            jsonyield_close
        }
    {
        tls_suppress_ragged_eofs(tls_conn_.get());
        tls_set_plain_output_stream(tls_conn_.get(),
                                    ByteStream { responses_.get() });
        tcp_set_output_stream(tcp_conn_.get(),
                              tls_get_encrypted_output_stream(tls_conn_.get()));
    }

    jsonyield_t *get_requests() const { return requests_.get(); }
    queuestream_t *get_responses() const { return responses_.get(); }

private:
    Hold<tcp_conn_t> tcp_conn_;
    Hold<tls_conn_t> tls_conn_;
    Hold<queuestream_t> responses_;
    Hold<jsonyield_t> requests_;
};

class ServerStack {
public:
    ServerStack(async_t *async, Hold<tcp_conn_t> tcp_conn,
                const string &server_hostname) :
        tcp_conn_ { std::move(tcp_conn) },
        tls_conn_ {
            open_tls_client_2(async, 
                              tcp_get_input_stream(tcp_conn_.get()),
                              TLS_SYSTEM_CA_BUNDLE,
                              server_hostname.c_str()),
            tls_close
        },
        requests_ { make_queuestream(async),  queuestream_terminate },
        responses_ {
            tls_get_plain_input_stream(tls_conn_.get()),
            bytestream_1_close
        }
    {
        tls_suppress_ragged_eofs(tls_conn_.get());
        tls_set_plain_output_stream(tls_conn_.get(),
                                    ByteStream { requests_.get() });
        tcp_set_output_stream(tcp_conn_.get(),
                              tls_get_encrypted_output_stream(tls_conn_.get()));
    }

    queuestream_t *get_requests() const { return requests_.get(); }
    bytestream_1 get_responses() const { return responses_.get(); }

private:
    Hold<tcp_conn_t> tcp_conn_;
    Hold<tls_conn_t> tls_conn_;
    Hold<queuestream_t> requests_;
    Keep<bytestream_1> responses_;
};

App::Task App::run_session(const Thunk *notify, Hold<tcp_conn_t> tcp_conn,
                           const Local &local)
{
    auto wakeup { co_await intro<Task::introspect>(notify) };
    ClientStack client_stack { get_async(), std::move(tcp_conn), local };
    Multiplex<Flow<Hold<json_thing_t>>, Flow<string>> mx(wakeup);
    auto req_flow
        { get_requests(mx.wakeup_left(), client_stack.get_requests()) };
    auto login_req { co_await req_flow };
    if (!authorized(std::move(login_req))) {
        cerr << "client unauthorized" << endl;
        co_await delay(wakeup, 3s);
        co_return;
    }
    auto irc_conn { co_await connect_to_server(wakeup) };
    ServerStack server_stack
        { get_async(), std::move(irc_conn), config_.irc_server.address };
    auto resp_flow
        { get_response(mx.wakeup_right(), server_stack.get_responses()) };
    auto login_resp { make_response("login") };
    send(client_stack.get_responses(), login_resp.get());
    for (;;) {
        auto result { co_await mx.tie(&req_flow, &resp_flow) };
        if (got_left(result)) {
            auto &request { get_left(result) };
            if (!request) {
                cerr << "client bailed" << endl;
                break;
            }
            process_client_request(client_stack.get_responses(),
                                   request->get(),
                                   server_stack.get_requests());
        } else {
            assert(got_right(result));
            auto response { get_right(result) };
            if (!response) {
                cerr << "server bailed" << endl;
                break;
            }
            cout << "server: " << *response << endl;
        }
    }
}

bool App::authorized(optional<Hold<json_thing_t>> login_req)
{
    try {
        authorize(std::move(login_req));
    } catch (const UnauthorizedException &e) {
        return false;
    }
    return true;
}

void App::authorize(optional<Hold<json_thing_t>> login_req)
{
    if (!login_req)
        throw UnauthorizedException("no login request");
    if (json_thing_type(login_req->get()) != JSON_OBJECT)
        throw UnauthorizedException("bad login request");
    const char *type;
    if (!json_object_get_string(login_req->get(), "type", &type))
        throw UnauthorizedException("no login request type");
    if (string(type) != "login")
        throw UnauthorizedException("login request expected");
    const char *name;
    if (!json_object_get_string(login_req->get(), "name", &name))
        throw UnauthorizedException("no login request name");
    const char *secret;
    if (!json_object_get_string(login_req->get(), "secret", &secret))
        throw UnauthorizedException("no login request secret");
    auto it { config_.clients.find(name) };
    if (it == config_.clients.end())
        throw UnauthorizedException("unknown login client");
    const auto &client { it->second };
    const auto tester { client.salt + "\n" + secret + "\n" };
    uint8_t hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const uint8_t *>(tester.c_str()), tester.length(),
           hash);
    Hold<char> sha256 { base64_encode_simple(hash, sizeof hash), fsfree };
    if (client.sha256 != sha256.get())
        throw UnauthorizedException("authentication failure");
}

App::Future<Hold<tcp_conn_t>> App::connect_to_server(const Thunk *notify)
{
    auto wakeup
        { co_await intro<Future<Hold<tcp_conn_t>>::introspect>(notify) };
    Hold<tcp_client_t> setup {
        open_tcp_client_2(get_async(),
                          config_.irc_server.address.c_str(),
                          config_.irc_server.port,
                          resolver_->get()),
        tcp_client_close
    };
    tcp_client_register_callback(setup.get(), thunk_to_action(wakeup));
    Hold<tcp_client_t> dereg { setup.get(), tcp_client_unregister_callback };
    for (;;) {
        Hold<tcp_conn_t> irc_conn
            { tcp_client_establish(setup.get()), tcp_close };
        if (irc_conn)
            co_return irc_conn;
        if (errno != EAGAIN)
            throw_errno();
        co_await suspend;
    }
}

App::Flow<Hold<json_thing_t>> App::get_requests(const Thunk *notify,
                                                jsonyield_t *requests)
{
    auto wakeup
        { co_await intro<Flow<Hold<json_thing_t>>::introspect>(notify) };
    for (;;) {
        auto request { co_await get_request(wakeup, requests) };
        if (!request)
            break;
        co_yield std::move(request);
    }
}

App::Future<Hold<json_thing_t>> App::get_request(const Thunk *notify,
                                                 jsonyield_t *requests)
{
    auto wakeup
        { co_await intro<Future<Hold<json_thing_t>>::introspect>(notify) };
    jsonyield_register_callback(requests, thunk_to_action(wakeup));
    Hold<jsonyield_t> dereg { requests, jsonyield_unregister_callback };
    for (;;) {
        Hold<json_thing_t> request
            { jsonyield_receive(requests), json_destroy_thing };
        if (request || errno == 0)
            co_return request;
        if (errno != EAGAIN)
            throw_errno();
        co_await suspend;
    }
}

void App::process_client_request(queuestream_t *responses,
                                 json_thing_t *request,
                                 queuestream_t *requests)
{
    if (json_thing_type(request) != JSON_OBJECT) {
        reject_request(responses, "request not an object");
        return;
    }
    const char *type;
    if (!json_object_get_string(request, "type", &type)) {
        reject_request(responses, "no request type");
        return;
    }
    string msg_type { type };
    if (msg_type == "nick")
        process_nick_request(responses, request, requests);
    else reject_request(responses, "unknown request type");
}

void App::process_nick_request(queuestream_t *responses, json_thing_t *request,
                               queuestream_t *requests)
{
    const char *nick;
    if (!json_object_get_string(request, "nick", &nick)) {
        reject_request(responses, "nick missing");
        return;
    }        
    const char *user;
    if (!json_object_get_string(request, "user", &user)) {
        reject_request(responses, "user missing");
        return;
    }
    emit(requests, "NICK ");
    emit(requests, nick);
    emit(requests, " \r\n");
    emit(requests, "USER ");
    emit(requests, nick);
    emit(requests, " 0 * :");
    emit(requests, user);
    emit(requests, "\r\n");
    auto response { make_response("nick") };
    send(responses, response.get());
}

FSTRACE_DECL(IRC_EMIT, "TEXT=%s");

void App::emit(queuestream_t *requests, const string &text)
{
    FSTRACE(IRC_EMIT, text.c_str());
    stringstream_t *sstr = copy_stringstream(get_async(), text.c_str());
    queuestream_enqueue(requests, stringstream_as_bytestream_1(sstr));
}

void App::reject_request(queuestream_t *responses, const string &reason)
{
    auto response { make_response("bad-request") };
    json_add_to_object(response.get(), "reason",
                       json_make_string(reason.c_str()));
    send(responses, response.get());
}

Hold<json_thing_t> App::make_response(const string &type)
{
    Hold<json_thing_t> response { json_make_object(), json_destroy_thing };
    json_add_to_object(response.get(), "type", json_make_string(type.c_str()));
    return response;
}

void App::send(queuestream_t *qstr, json_thing_t *msg)
{
    ByteStream encoder { json_encode(get_async(), msg) };
    ByteStream framer { naive_encode(get_async(), encoder, 0, 0) };
    queuestream_enqueue(qstr, framer);
}

App::Flow<string> App::get_response(const Thunk *notify, bytestream_1 responses)
{
    auto wakeup { co_await intro<Flow<string>::introspect>(notify) };
    string response;
    for (;;) {
        char buffer[1100];
        auto count { co_await read(wakeup, responses, buffer, sizeof buffer) };
        if (count > 0) {
            response.append(buffer, count);
            for (;;) {
                auto loc { response.find("\r\n") };
                if (loc == string::npos)
                    break;
                co_yield response.substr(0, loc);
                response = response.substr(loc + 2);
            }
            if (response.length() > 1500)
                throw OverlongMessageException();
        } else {
            if (response.length() > 0)
                throw ConnectionBrokenException();
            co_return;
        }
    }
}

App::Future<size_t> App::read(const Thunk *notify, bytestream_1 stream,
                              char *buffer, size_t length)
{
    auto wakeup { co_await intro<Future<size_t>::introspect>(notify) };
    bytestream_1_register_callback(stream, thunk_to_action(wakeup));
    Keep<bytestream_1> dereg { stream, bytestream_1_unregister_callback };
    for (;;) {
        auto count { bytestream_1_read(stream, buffer, sizeof buffer) };
        if (count >= 0)
            co_return count;
        if (errno != EAGAIN)
            throw_errno();
        co_await suspend;
    }
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
