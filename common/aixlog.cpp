/***
      __   __  _  _  __     __    ___
     / _\ (  )( \/ )(  )   /  \  / __)
    /    \ )(  )  ( / (_/\(  O )( (_ \
    \_/\_/(__)(_/\_)\____/ \__/  \___/
    version 1.5.3
    https://github.com/badaix/aixlog

    This file is part of aixlog
    Copyright (C) 2017-2025 Johannes Pohl

    This software may be modified and distributed under the terms
    of the MIT license.  See the LICENSE file for details.
***/

#include "aixlog.hpp"
#include <unordered_map>
#include <list>

namespace AixLog
{

class ShouldLogCache
{
public:
    struct CacheKey
    {
        int severity;
        std::string tag;

        bool operator==(const CacheKey& other) const
        {
            return severity == other.severity && tag == other.tag;
        }
    };

    struct CacheKeyHash
    {
        std::size_t operator()(const CacheKey& key) const
        {
            return std::hash<int>()(key.severity) ^ 
                   (std::hash<std::string>()(key.tag) << 1);
        }
    };

    bool getCached(Severity severity, const char* tag, bool& result)
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        CacheKey key{static_cast<int>(severity), tag ? std::string(tag) : std::string()};
        auto it = cache_.find(key);
        if (it != cache_.end())
        {
            result = it->second.second;
            access_order_.splice(access_order_.begin(), access_order_, it->second.first);
            cache_hits_++;
            return true;
        }
        cache_misses_++;
        return false;
    }

    void putCache(Severity severity, const char* tag, bool result)
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        CacheKey key{static_cast<int>(severity), tag ? std::string(tag) : std::string()};
        if (cache_.find(key) != cache_.end()) return;
        access_order_.emplace_front(key);
        cache_[key] = {access_order_.begin(), result};
        if (cache_.size() > max_cache_size_)
        {
            auto last = access_order_.back();
            cache_.erase(last);
            access_order_.pop_back();
        }
    }

    void clearCache()
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        cache_.clear();
        access_order_.clear();
        cache_hits_ = 0;
        cache_misses_ = 0;
    }

    void getStats(size_t& hits, size_t& misses, size_t& size)
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        hits = cache_hits_;
        misses = cache_misses_;
        size = cache_.size();
    }

    void setMaxSize(size_t size)
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        max_cache_size_ = size;
        while (cache_.size() > max_cache_size_)
        {
            auto last = access_order_.back();
            cache_.erase(last);
            access_order_.pop_back();
        }
    }

private:
    size_t max_cache_size_{1000};
    std::unordered_map<CacheKey, std::pair<std::list<CacheKey>::iterator, bool>, CacheKeyHash> cache_;
    std::list<CacheKey> access_order_;
    std::mutex cache_mutex_;
    size_t cache_hits_{0};
    size_t cache_misses_{0};
};

static ShouldLogCache& getShouldLogCache()
{
    static ShouldLogCache instance;
    return instance;
}

bool Log::should_log_cached(SEVERITY severity, const char* tag)
{
    auto& cache = getShouldLogCache();
    bool result;
    if (cache.getCached(static_cast<Severity>(severity), tag, result))
    {
        return result;
    }
    result = should_log_internal(static_cast<Severity>(severity), tag);
    cache.putCache(static_cast<Severity>(severity), tag, result);
    return result;
}

bool Log::should_log_cached(Severity severity, const char* tag)
{
    auto& cache = getShouldLogCache();
    bool result;
    if (cache.getCached(severity, tag, result))
    {
        return result;
    }
    result = should_log_internal(severity, tag);
    cache.putCache(severity, tag, result);
    return result;
}

bool Log::should_log_cached(Severity severity, const std::string& tag)
{
    return should_log_cached(severity, tag.c_str());
}

void Log::clearShouldLogCache()
{
    getShouldLogCache().clearCache();
}

void Log::getShouldLogCacheStats(size_t& hits, size_t& misses, size_t& size)
{
    getShouldLogCache().getStats(hits, misses, size);
}

void Log::setShouldLogCacheMaxSize(size_t size)
{
    getShouldLogCache().setMaxSize(size);
}

} // namespace AixLog

