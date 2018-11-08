#pragma once

#include <chrono>
#include <map>
#include <mutex>
#include <string>

#include <entwine/reader/cache.hpp>

#include <greyhound/auth.hpp>
#include <greyhound/configuration.hpp>
#include <greyhound/defs.hpp>
#include <greyhound/resource.hpp>

#include <entwine/third/arbiter/arbiter.hpp>
#include <entwine/types/files.hpp>

namespace greyhound
{

class Manager
{
public:
    Manager(const Configuration& config);

    template<typename Req>
    SharedResource get(std::string name, Req& req);

    entwine::Cache& cache() const { return m_cache; }
    std::shared_ptr<entwine::arbiter::Arbiter> arbiter() const { return m_arbiter; }
    const Paths& paths() const { return m_paths; }
    const Headers& headers() const { return m_headers; }
    std::size_t threads() const { return m_threads; }
    std::size_t timeoutSeconds() const { return m_timeoutSeconds; }

    const Configuration& config() const { return m_config; }
    void sweep();

private:
    std::vector<std::string> resolve(std::string name) const
    {
        if (m_aliases.count(name)) return m_aliases.at(name);
        else return std::vector<std::string>{ name };
    }

    mutable entwine::Cache m_cache;
    mutable std::shared_ptr<entwine::arbiter::Arbiter> m_arbiter;

    Paths m_paths;
    Headers m_headers;
    const std::size_t m_threads;

    const Configuration& m_config;
    std::map<std::string, std::vector<std::string>> m_aliases;

    std::map<std::string, TimedReader> m_readers;
    std::map<std::string, SharedResource> m_resources;
    std::unique_ptr<Auth> m_auth;

    mutable std::mutex m_mutex;

    TimePoint m_swept;
    std::size_t m_timeoutSeconds = 0;
};

template<typename Req>
SharedResource Manager::get(std::string name, Req& req)
{
    std::unique_lock<std::mutex> lock(m_mutex);

    auto it(m_resources.find(name));
    if (it == m_resources.end())
    {
        std::vector<TimedReader*> readers;

        for (const auto s : resolve(name))
        {
            auto rit(m_readers.find(s));
            if (rit == m_readers.end())
            {
                rit = m_readers.emplace(
                        std::piecewise_construct,
                        std::forward_as_tuple(s),
                        std::forward_as_tuple(*this, s)).first;
            }

            readers.push_back(&rit->second);
        }

        it = m_resources.emplace(
                name,
                std::make_shared<Resource>(*this, name, readers)).first;
    }

    SharedResource resource(it->second);

    lock.unlock();

    entwine::Pool pool(threads());

    for (TimedReader* reader : resource->readers())
    {
        const auto name(reader->name());
        if (m_auth)
        {
            const auto code(m_auth->check(name, req));
            if (!ok(code))
            {
                throw HttpError(code, "Authorization failure: " + name);
            }
        }

        pool.add([reader, name]()
        {
            if (!reader->get())
            {
                throw HttpError(
                        HttpStatusCode::client_error_not_found,
                        "Not found: " + name);
            }
        });
    }

    pool.join();

    return resource;
}

} // namespace greyhound

