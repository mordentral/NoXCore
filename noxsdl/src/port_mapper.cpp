// PortMapper (port_mapper.hpp): UPnP discovery and the port mapping, run on a thread of its own;
// the mapping removed again when it's destroyed.
#include "port_mapper.hpp"

#include <miniupnpc.h>
#include <upnpcommands.h>
#include <upnperrors.h>

#include <condition_variable>
#include <memory>
#include <string>
#include <vector>

namespace noxsdl {

namespace {

// The retired mappers' threads (retire), each with its "done" (under retireMutex), and how many of
// them are still removing.
struct Retiring {
    std::thread thread;
    std::shared_ptr<bool> done = std::make_shared<bool>(false);
};
std::mutex retireMutex;
std::condition_variable retireDone;
std::vector<Retiring> retiring;
int retirePending = 0;

} // namespace

void PortMapper::retire(std::unique_ptr<PortMapper> mapper)
{
    if (!mapper) {
        return;
    }
    std::vector<std::thread> finished; // (those done since: joined here, not kept till quit)
    {
        std::lock_guard lock(retireMutex);
        ++retirePending;
        mapper->retired_ = true; // (one still waiting to map gives up: it would wait on itself)
        retireDone.notify_all();
        for (auto it = retiring.begin(); it != retiring.end();) {
            if (*it->done) {
                finished.push_back(std::move(it->thread));
                it = retiring.erase(it);
            } else {
                ++it;
            }
        }
        Retiring r;
        r.thread = std::thread([m = std::move(mapper), done = r.done]() mutable {
            m.reset(); // (the destructor: discovery joined, the mapping removed)
            {
                std::lock_guard guard(retireMutex);
                --retirePending;
                *done = true;
            }
            retireDone.notify_all();
        });
        retiring.push_back(std::move(r));
    }
    for (auto& t : finished) {
        t.join(); // (at its end already)
    }
}

void PortMapper::joinRetired()
{
    std::vector<Retiring> threads;
    {
        std::lock_guard lock(retireMutex);
        threads.swap(retiring);
    }
    for (auto& r : threads) {
        r.thread.join();
    }
}

PortMapper::PortMapper(std::uint16_t port) : port_(port), thread_([this] { run(); }) {}

PortMapper::~PortMapper()
{
    if (thread_.joinable()) {
        thread_.join();
    }
    if (state_ == State::Mapped) {
        const std::string ext = std::to_string(port_);
        UPNP_DeletePortMapping(controlUrl_.c_str(), serviceType_.c_str(), ext.c_str(), "UDP", nullptr);
    }
}

std::string PortMapper::detail() const
{
    std::lock_guard lock(mutex_);
    return detail_;
}

void PortMapper::run()
{
    const auto fail = [this](std::string why) {
        std::lock_guard lock(mutex_);
        detail_ = std::move(why);
        state_ = State::Failed;
    };
    {
        // A retired mapper's removal first (the same port, most likely): after ours it would
        // take our mapping away.
        std::unique_lock lock(retireMutex);
        retireDone.wait(lock, [this] { return retirePending == 0 || retired_; });
        if (retired_) {
            state_ = State::Failed; // (dropped before it mapped: nothing to remove)
            return;
        }
    }
    int error = 0;
    // Up to 2 seconds for routers to answer.
    UPNPDev* devices = upnpDiscover(2000, nullptr, nullptr, UPNP_LOCAL_PORT_ANY, 0, 2, &error);
    if (!devices) {
        fail("no UPnP router found");
        return;
    }
    UPNPUrls urls{};
    IGDdatas data{};
    char lan[64] = {};
    char wan[64] = {};
    // The router (Internet gateway) our LAN address is on, and its outside address.
    const int igd = UPNP_GetValidIGD(devices, &urls, &data, lan, sizeof lan, wan, sizeof wan);
    freeUPNPDevlist(devices);
    if (igd != UPNP_CONNECTED_IGD) {
        if (igd != UPNP_NO_IGD) {
            FreeUPNPUrls(&urls);
        }
        // (UPNP_PRIVATEIP_IGD: the router's own outside address is private, behind another NAT:
        // a mapping there wouldn't reach us.)
        fail(igd == UPNP_NO_IGD ? "no UPnP router found"
             : igd == UPNP_PRIVATEIP_IGD ? std::string("the router is behind another NAT (its address ") + wan + ")"
                                          : "the UPnP router isn't connected");
        return;
    }
    const std::string port = std::to_string(port_);
    int r = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype, port.c_str(), port.c_str(), lan, "Nox (NoX Core)",
                                "UDP", nullptr, "0");
    if (r != UPNPCOMMAND_SUCCESS) { // (some routers refuse a permanent one: a day's lease then)
        r = UPNP_AddPortMapping(urls.controlURL, data.first.servicetype, port.c_str(), port.c_str(), lan, "Nox (NoX Core)",
                                "UDP", nullptr, "86400");
    }
    if (r != UPNPCOMMAND_SUCCESS) {
        const char* what = strupnperror(r);
        fail("the router refused the mapping (" + std::string(what ? what : std::to_string(r)) + ")");
        FreeUPNPUrls(&urls);
        return;
    }
    {
        std::lock_guard lock(mutex_);
        controlUrl_ = urls.controlURL;
        serviceType_ = data.first.servicetype;
        detail_ = "UDP " + port + " mapped to " + lan + ", outside " + wan;
    }
    FreeUPNPUrls(&urls);
    state_ = State::Mapped;
}

} // namespace noxsdl
