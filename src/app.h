#pragma once

#include <exception>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <async/fsadns.h>
#include <async/tcp_connection.h>

#include "coasync.h"
#include "socketaddress.h"
#include "hold.h"

class UsageException : public std::exception {
public:
    const char *what() const noexcept override { return "bad usage"; }
};

class NoHomeException : public std::exception {
public:
    const char *what() const noexcept override { return "HOME not defined"; }
};

class BadHomeException : public std::exception {
public:
    const char *what() const noexcept override { return "bad HOME"; }
};

class BadConfigException : public std::exception {
public:
    const char *what() const noexcept override {
        return "bad configuration file";
    }
};

class AddressResolutionException : public std::exception {
public:
    AddressResolutionException(int errcode) : errcode_ { errcode } {}
    const char *what() const noexcept override {
        return gai_strerror(errcode_);
    }

private:
    int errcode_;
};

class UnauthorizedException : public std::exception {
public:
    UnauthorizedException(std::string reason) : reason_ { reason } {}
    const char *what() const noexcept override {
        return reason_.c_str();
    }

private:
    std::string reason_;
};

class ConnectionBrokenException : public std::exception {
public:
    const char *what() const noexcept override { return "connection broken"; }
};

class OverlongMessageException : public std::exception {
public:
    const char *what() const noexcept override { return "overlong message"; }
};

struct UserSettings {
    std::string nick;
    std::string name;
    std::map<std::string, std::string> autojoins;
};

struct LocalConfig {
    std::string address { "0.0.0.0" };
    std::vector<pacujo::net::SocketAddress> resolutions;
    int port { 11345 };
    std::optional<std::string> tls_server_name;
    std::string certificate;
    std::string private_key;
};

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
    template<typename T> using Hold = pacujo::etc::Hold<T>;

public:
    ClientStack(async_t *async, Hold<tcp_conn_t> tcp_conn,
                const LocalConfig &local_config) :
        tcp_conn_ { std::move(tcp_conn) },
        tls_conn_ {
            open_tls_server(async,
                            tcp_get_input_stream(tcp_conn_.get()),
                            local_config.certificate.c_str(),
                            local_config.private_key.c_str()),
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
    template<typename T> using Hold = pacujo::etc::Hold<T>;
    template<typename T> using Keep = pacujo::etc::Keep<T>;

public:
    ServerStack(async_t *async, Hold<tcp_conn_t> tcp_conn,
                const std::string &server_hostname) :
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

struct ClientConfig {
    std::string salt;
    // base64(sha256(salt + "\n" + secret + "\n"))
    std::string sha256;
};

struct Config {
    std::map<std::string, UserSettings> settings;
    std::vector<LocalConfig> local;
    std::map<std::string, ClientConfig> clients;
    struct {
        std::string address { "irc.oftc.net" };
        int port { 6697 };
        bool use_tls { true };
    } irc_server;
    std::string cache_directory { ".cache/lipid/main" };
};

struct Opts {
    bool help {};
    std::optional<std::filesystem::path> config_file;
    std::optional<std::string> trace_include;
    std::optional<std::string> trace_exclude;
};

class AddrInfo {
    template<typename T> using Hold = pacujo::etc::Hold<T>;

public:
    AddrInfo(addrinfo *info = nullptr) : info_ { info, fsadns_freeaddrinfo } {}
    AddrInfo(AddrInfo &&other) = default;
    AddrInfo &operator=(AddrInfo &&other) = default;
    const addrinfo *get() const { return info_.get(); }
    operator bool() const { return bool(info_); }
private:
    Hold<addrinfo> info_;
};

class App : public pacujo::coasync::Framework {
    template<typename T> using optional = std::optional<T>;
    template<typename T> using Hold = pacujo::etc::Hold<T>;
    using Thunk = pacujo::cordial::Thunk;

public:
    App(const std::filesystem::path &home_dir, const Opts &opts);
    void run(fstrace_t *trace);

private:
    class ClientSession {
    public:
        ClientSession(const Thunk &wakeup) : wakeup_ { wakeup } {}
        ClientSession(ClientSession &&other) = default;
        ClientSession &operator=(ClientSession &&other) = default;
        void set_task(Task &&task) { task_ = std::move(task); }
        optional<Task> &get_task() { return task_; }
        const Thunk *get_wakeup() const { return &wakeup_; }
    private:
        Thunk wakeup_;
        optional<Task> task_;
    };

    class ServerSession {
    public:
        ServerSession(const Thunk &wakeup) : wakeup_ { wakeup } {}
    private:
        Thunk wakeup_;
        optional<Task> task_;
    };

    std::filesystem::path home_dir_;
    const Opts &opts_;
    Config config_;
    optional<Hold<fsadns_t>> resolver_;
    int64_t next_session_ { 0 };
    std::map<int64_t, std::shared_ptr<ClientSession>> client_sessions_;
    std::map<std::string, std::shared_ptr<ServerSession>> server_sessions_;

    void read_configuration(std::filesystem::path config_file);
    void add_locals(json_thing_t *locals);
    void add_clients(json_thing_t *clients);
    Task run_server();
    Task resolve_addresses(const Thunk *notify);
    Future<AddrInfo> resolve_address(const Thunk *notify,
                                     const std::string &address);
    Task serve(const Thunk *notify,
               const pacujo::net::SocketAddress &address,
               const LocalConfig &local_config);
    Future<Hold<tcp_conn_t>> accept(const Thunk *notify,
                                    tcp_server_t *tcp_server);
    Task run_session(const Thunk *notify, Hold<tcp_conn_t> tcp_conn,
                     const LocalConfig &local_config);
    optional<std::string> authorized(optional<Hold<json_thing_t>> login_req);
    std::string authorize(optional<Hold<json_thing_t>> login_req);
    Future<Hold<tcp_conn_t>> connect_to_server(const Thunk *notify);
    Flow<Hold<json_thing_t>> get_requests(const Thunk *notify,
                                          jsonyield_t *requests);
    Future<Hold<json_thing_t>> get_request(const Thunk *notify,
                                           jsonyield_t *requests);
    void process_client_request(queuestream_t *responses,
                                json_thing_t *request,
                                queuestream_t *requests);
    void process_nick_request(queuestream_t *responses, json_thing_t *request,
                              queuestream_t *requests);
    void emit(queuestream_t *requests, const std::string &text);
    void reject_request(queuestream_t *responses, const std::string &reason);
    Hold<json_thing_t> make_response(const std::string &type);
    void send(queuestream_t *q, json_thing_t *msg);
    Flow<std::string> get_response(const Thunk *notify, bytestream_1 responses);
    Future<size_t> read(const Thunk *notify, bytestream_1 stream,
                        char *buffer, size_t length);
};
