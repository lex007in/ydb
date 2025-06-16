#pragma once

#include "shared_cache_switchable.h"
#include "shared_sausagecache.h"

namespace NKikimr::NSharedCache {

    template <typename TPage, typename TPageTraits>
    class TTieredCache : public ICacheCache<TPage> {
        using TCounterPtr = ::NMonitoring::TDynamicCounters::TCounterPtr;
        using TReplacementPolicy = NKikimrSharedCache::TReplacementPolicy;

        struct TCacheHolder {
            TSwitchableCache<TPage, TPageTraits> Cache;
            TCounterPtr ActivePages;
            TCounterPtr ActiveBytes;
        };

    public:
        template <typename TCacheBuilder>
        TTieredCache(ui64 limit, TCacheBuilder createCache, TReplacementPolicy policy, ui32 numberOfTiers, TSharedPageCacheCounters& cacheCounters)
            : CacheTiers(::Reserve(numberOfTiers))
        {
            Y_ENSURE(numberOfTiers > 0);
            for (const auto& tier : xrange(numberOfTiers)) {
                CacheTiers.emplace_back(
                    TSwitchableCache<TPage, TPageTraits>(1, createCache(), cacheCounters.ReplacementPolicySize(policy)),
                    cacheCounters.ActivePagesTier(tier),
                    cacheCounters.ActiveBytesTier(tier)
                );
            }
            UpdateLimit(limit);
        }
        
        TIntrusiveList<TPage> MoveTouch(TPage *page, ui32 targetTier) {
            ui32 sourceTier = TPageTraits::GetTier(page);
            Y_ENSURE(sourceTier < CacheTiers.size());

            if (sourceTier == targetTier) {
                ui32 cacheId = TPageTraits::GetCacheId(page);
                if (cacheId == 0) {
                    CacheTiers[sourceTier].ActivePages->Inc();
                    CacheTiers[sourceTier].ActiveBytes->Add(TPageTraits::GetSize(page));
                }
                return ProcessEvictedList(CacheTiers[sourceTier].Cache.Touch(page), sourceTier);
            }

            Y_ENSURE(targetTier < CacheTiers.size());

            UpdateLimits(page, sourceTier, targetTier);

            ui32 cacheId = TPageTraits::GetCacheId(page);
            if (cacheId > 0) {
                CacheTiers[sourceTier].ActivePages->Dec();
                CacheTiers[sourceTier].ActiveBytes->Sub(TPageTraits::GetSize(page));
                CacheTiers[sourceTier].Cache.Erase(page);
            }
            TPageTraits::SetTier(page, targetTier);
            CacheTiers[targetTier].ActivePages->Inc();
            CacheTiers[targetTier].ActiveBytes->Add(TPageTraits::GetSize(page));
            return ProcessEvictedList(CacheTiers[targetTier].Cache.Touch(page), targetTier);
        }

        template <typename TCacheBuilder>
        TIntrusiveList<TPage> Switch(TCacheBuilder createCache, TCounterPtr sizeCounter) Y_WARN_UNUSED_RESULT {
            TIntrusiveList<TPage> evictedList;

            for (ui32 tier = 0; tier < CacheTiers.size(); ++tier) {
                evictedList.Append(ProcessEvictedList(CacheTiers[tier].Cache.Switch(createCache(), sizeCounter), tier));
            }

            return evictedList;
        }

        TIntrusiveList<TPage> EvictNext() override {
            for (ui32 tier = 0; tier < CacheTiers.size(); ++tier) {
                if (auto evicted = CacheTiers[tier].Cache.EvictNext(); !evicted.Empty()) {
                    return ProcessEvictedList(std::move(evicted), tier);
                }
            }
            return {};
        }

        TIntrusiveList<TPage> Touch(TPage *page) override {
            ui32 tier = TPageTraits::GetTier(page);
            Y_ENSURE(tier < CacheTiers.size());
            ui32 cacheId = TPageTraits::GetCacheId(page);
            if (cacheId == 0) {
                CacheTiers[tier].ActivePages->Inc();
                CacheTiers[tier].ActiveBytes->Add(TPageTraits::GetSize(page));
            }
            return ProcessEvictedList(CacheTiers[tier].Cache.Touch(page), tier);
        }

        void Erase(TPage *page) override {
            ui32 tier = TPageTraits::GetTier(page);
            Y_ENSURE(tier < CacheTiers.size());
            ui32 cacheId = TPageTraits::GetCacheId(page);
            if (cacheId > 0) {
                CacheTiers[tier].ActivePages->Dec();
                CacheTiers[tier].ActiveBytes->Sub(TPageTraits::GetSize(page));
            }
            CacheTiers[tier].Cache.Erase(page);
        }

        void UpdateLimit(ui64 limit) override {
            for (ui32 tier = CacheTiers.size() - 1; tier > 0; --tier) {
                ui64 currentTierLimit = Min(CacheTiers[tier].Cache.GetSize(), limit);
                CacheTiers[tier].Cache.UpdateLimit(currentTierLimit);
                limit -= currentTierLimit;
            }
            CacheTiers[0].Cache.UpdateLimit(limit);
        }

        ui64 GetLimit() const override {
            ui64 result = 0;
            for (const auto& cacheTier : CacheTiers) {
                result += cacheTier.Cache.GetLimit();
            }
            return result;
        }

        ui64 GetSize() const override {
            ui64 result = 0;
            for (const auto& cacheTier : CacheTiers) {
                result += cacheTier.Cache.GetSize();
            }
            return result;
        }

        virtual TString Dump() const override {
            TStringBuilder result;
            size_t count = 0;

            for (const auto& cacheTier : CacheTiers) {
                if (count) result << "; ";
                result << cacheTier.Cache.Dump();
                count++;
            }
        
            return result;
        }

    private:

        void UpdateLimits(TPage *page, ui32 sourceTier, ui32 targetTier) {
            auto sourceCacheLimit = CacheTiers[sourceTier].Cache.GetLimit();
            auto limitDelta = Min(sourceCacheLimit, TPageTraits::GetSize(page));
            CacheTiers[sourceTier].Cache.UpdateLimit(sourceCacheLimit - limitDelta);
            CacheTiers[targetTier].Cache.UpdateLimit(CacheTiers[targetTier].Cache.GetLimit() + limitDelta);
        }

        TIntrusiveList<TPage> ProcessEvictedList(TIntrusiveList<TPage>&& evictedList, ui32 tier) {
            ui64 evictedPages = 0;
            ui64 evictedSize = 0;

            for (auto& page_ : evictedList) {
                TPage* page = &page_;
                TPageTraits::SetCacheId(page, 0);
                ++evictedPages;
                evictedSize += TPageTraits::GetSize(page);
            }

            CacheTiers[tier].ActivePages->Sub(evictedPages);
            CacheTiers[tier].ActiveBytes->Sub(evictedSize);

            return evictedList;
        }

    private:
        TVector<TCacheHolder> CacheTiers;
    };

} // namespace NKikimr::NSharedCache
