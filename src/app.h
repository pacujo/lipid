#pragma once

#include <exception>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <async/fsadns.h>

#include "coasync.h"
#include "socketaddress.h"

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

class App : public pacujo::coasync::Framework {
public:
    App(const std::filesystem::path &home_dir, const Opts &opts);
    ~App();
    void run(fstrace_t *trace);

private:
    std::filesystem::path home_dir_;
    const Opts &opts_;
    Config config_;
    fsadns_t *resolver_;
    std::vector<Task> sessions_;

    void read_configuration(std::filesystem::path config_file);
    Task run_server();
    Task resolve_addresses(const pacujo::cordial::Thunk *notify);
    Future<AddrInfo> resolve_address(const pacujo::cordial::Thunk *notify,
                                     const std::string &address);
    Task serve(const pacujo::net::SocketAddress &address);
};
