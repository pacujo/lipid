#pragma once

#include <exception>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <async/fsadns.h>

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
    ~App();
    void run(fstrace_t *trace);

private:
    class Session {
    public:
        Session(const pacujo::cordial::Thunk &wakeup) : wakeup_ { wakeup } {}
        Session(Session &&other) = default;
        Session &operator=(Session &&other) = default;
        void set_task(Task &&task) { task_ = std::move(task); }
        const pacujo::cordial::Thunk *get_wakeup() const { return &wakeup_; }
    private:
        pacujo::cordial::Thunk wakeup_;
        std::optional<Task> task_ {};
    };

    std::filesystem::path home_dir_;
    const Opts &opts_;
    Config config_;
    fsadns_t *resolver_;
    int64_t next_session_ { 0 };
    std::map<int64_t, Session> sessions_;

    void read_configuration(std::filesystem::path config_file);
    Task run_server();
    Task resolve_addresses(const pacujo::cordial::Thunk *notify);
    Future<AddrInfo> resolve_address(const pacujo::cordial::Thunk *notify,
                                     const std::string &address);
    Task serve(const pacujo::net::SocketAddress &address, const Local &local);
    Task run_session(const pacujo::cordial::Thunk *notify,
                     pacujo::etc::Hold<tcp_conn_t> tcp_conn,
                     const Local &local);
    Flow<pacujo::etc::Hold<json_thing_t>>
    get_request(const pacujo::cordial::Thunk *notify,
                jsonyield_t *requests);
    void send(queuestream_t *q, json_thing_t *msg);
};
