// A scriptable Commissioner for the enrollment tests: fixed results per
// operation, optional delays (to prove no lock is held during a call), and
// a record of every call.
#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "pogohome_api/commissioner.hpp"
#include "pogohome_api/otbr_helper_protocol.hpp"

namespace test_support {

class FakeCommissioner : public pogohome_api::Commissioner {
public:
    using Status = pogohome_api::CommissionerStatus;

    Status ensure_result = Status::Ok;
    Status add_result = Status::Ok;
    Status remove_result = Status::Ok;
    bool started_result = true;
    std::chrono::milliseconds ensure_delay{0};
    std::chrono::milliseconds add_delay{0};
    std::chrono::milliseconds remove_delay{0};
    // The OTBR's Mesh-Local EID as the helper would report it. A made-up
    // address in a made-up Mesh-Local Prefix.
    Status mleid_result = Status::Ok;
    std::string mleid_address = "fd11:2233:4455:0:8c1f:2e3d:4c5b:6a79";
    std::chrono::milliseconds mleid_delay{0};

    std::atomic<int> ensure_calls{0};
    std::atomic<int> add_calls{0};
    std::atomic<int> remove_calls{0};
    std::atomic<int> mleid_calls{0};
    // True while a QueryMeshLocalEid() call is inside its delay.
    std::atomic<bool> mleid_in_progress{false};
    std::mutex mutex;
    // Every call, in order: "mleid", "ensure", "add", "remove".
    std::vector<std::string> order;
    std::vector<std::string> added;
    std::vector<std::string> pskds_seen;
    std::vector<int> add_timeouts;
    std::vector<std::string> removed;

    Status QueryState(std::string &state) override
    {
        state = "active";
        return Status::Ok;
    }

    Status QueryMeshLocalEid(std::string &address) override
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            order.push_back("mleid");
        }
        mleid_calls++;
        mleid_in_progress = true;
        std::this_thread::sleep_for(mleid_delay);
        mleid_in_progress = false;
        std::lock_guard<std::mutex> lock(mutex);
        address = mleid_address;
        return mleid_result;
    }

    Status EnsureActive(bool &started) override
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            order.push_back("ensure");
        }
        ensure_calls++;
        std::this_thread::sleep_for(ensure_delay);
        started = started_result;
        return ensure_result;
    }

    Status AddJoiner(const std::string &eui64, std::string &pskd, int timeout_s) override
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            order.push_back("add");
            added.push_back(eui64);
            pskds_seen.push_back(pskd);
            add_timeouts.push_back(timeout_s);
        }
        pogohome_api::SecureWipe(pskd); // the interface's contract
        add_calls++;
        std::this_thread::sleep_for(add_delay);
        return add_result;
    }

    Status RemoveJoiner(const std::string &eui64) override
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            order.push_back("remove");
            removed.push_back(eui64);
        }
        remove_calls++;
        std::this_thread::sleep_for(remove_delay);
        return remove_result;
    }
};

} // namespace test_support
