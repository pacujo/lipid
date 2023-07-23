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
    const char *what() const noexcept override {
        return "connection broken";
    }
};

struct UserSettings {
    std::string nick;
    std::string name;
    std::map<std::string, std::string> autojoins;
};

struct Local {
    std::string address { "0.0.0.0" };
    std::vector<pacujo::net::SocketAddress> resolutions;
    int port { 11345 };
    std::optional<std::string> tls_server_name;
    std::string certificate;
    std::string private_key;
};

struct Client {
    std::string salt;
    // base64(sha256(salt + "\n" + secret + "\n"))
    std::string sha256;
};

struct Config {
    std::map<std::string, UserSettings> settings;
    std::vector<Local> local;
    std::map<std::string, Client> clients;
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
public:
    AddrInfo(addrinfo *info = nullptr) : info_ { info, fsadns_freeaddrinfo } {}
    AddrInfo(AddrInfo &&other) = default;
    AddrInfo &operator=(AddrInfo &&other) = default;
    const addrinfo *get() const { return info_.get(); }
    operator bool() const { return bool(info_); }
private:
    pacujo::etc::Hold<addrinfo> info_;
};

class App : public pacujo::coasync::Framework {
public:
    App(const std::filesystem::path &home_dir, const Opts &opts);
    void run(fstrace_t *trace);

private:
    class Session {
    public:
        Session(const pacujo::cordial::Thunk &wakeup) : wakeup_ { wakeup } {}
        Session(Session &&other) = default;
        Session &operator=(Session &&other) = default;
        void set_task(Task &&task) { task_ = std::move(task); }
        std::optional<Task> &get_task() { return task_; }
        const pacujo::cordial::Thunk *get_wakeup() const { return &wakeup_; }
    private:
        pacujo::cordial::Thunk wakeup_;
        std::optional<Task> task_ {};
    };

    std::filesystem::path home_dir_;
    const Opts &opts_;
    Config config_;
    std::optional<pacujo::etc::Hold<fsadns_t>> resolver_;
    int64_t next_session_ { 0 };
    std::map<int64_t, Session> sessions_;

    void read_configuration(std::filesystem::path config_file);
    void add_locals(json_thing_t *locals);
    void add_clients(json_thing_t *clients);
    Task run_server();
    Task resolve_addresses(const pacujo::cordial::Thunk *notify);
    Future<AddrInfo> resolve_address(const pacujo::cordial::Thunk *notify,
                                     const std::string &address);
    Task serve(const pacujo::cordial::Thunk *notify,
               const pacujo::net::SocketAddress &address, const Local &local);
    Task run_session(const pacujo::cordial::Thunk *notify,
                     pacujo::etc::Hold<tcp_conn_t> tcp_conn,
                     const Local &local);
    bool authorized(std::optional<pacujo::etc::Hold<json_thing_t>> login_req);
    void authorize(std::optional<pacujo::etc::Hold<json_thing_t>> login_req);
    Future<pacujo::etc::Hold<tcp_conn_t>>
    connect_to_server(const pacujo::cordial::Thunk *notify);
    Flow<pacujo::etc::Hold<json_thing_t>>
    get_request(const pacujo::cordial::Thunk *notify,
                jsonyield_t *requests);
    void send(queuestream_t *q, json_thing_t *msg);
    Flow<std::string> get_response(const pacujo::cordial::Thunk *notify,
                                   bytestream_1 responses);
};
