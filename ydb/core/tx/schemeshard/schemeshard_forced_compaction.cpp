#include "schemeshard_forced_compaction.h"

#include "schemeshard_impl.h"

namespace NKikimr::NSchemeShard {


void TSchemeShard::AddForcedCompaction(
    const TForcedCompactionInfo::TPtr& forcedCompactionInfo,
    const TVector<TShardIdx>& shardsToCompact)
{
    ForcedCompactions[forcedCompactionInfo->Id] = forcedCompactionInfo;
    ForcedCompactionsByTable[forcedCompactionInfo->TablePathId] = forcedCompactionInfo;
    ForcedCompactionTablesQueue.Enqueue(forcedCompactionInfo->TablePathId);
    auto& queue = ForcedCompactionShardsByTable[forcedCompactionInfo->TablePathId];
    for (const auto& shardId : shardsToCompact) {
        queue.Enqueue(shardId);
    }
}

void TSchemeShard::PersistForcedCompactionState(
    NIceDb::TNiceDb& db,
    const TForcedCompactionInfo& forcedCompactionInfo,
    const TVector<TShardIdx>& shardsToCompact)
{
    db.Table<Schema::ForcedCompactions>().Key(forcedCompactionInfo.Id).Update(
        NIceDb::TUpdate<Schema::ForcedCompactions::TableOwnerId>(forcedCompactionInfo.TablePathId.OwnerId),
        NIceDb::TUpdate<Schema::ForcedCompactions::TableLocalId>(forcedCompactionInfo.TablePathId.LocalPathId),
        NIceDb::TUpdate<Schema::ForcedCompactions::Cascade>(forcedCompactionInfo.Cascade),
        NIceDb::TUpdate<Schema::ForcedCompactions::MaxShardsInFlight>(forcedCompactionInfo.MaxShardsInFlight),
        NIceDb::TUpdate<Schema::ForcedCompactions::StartTime>(forcedCompactionInfo.StartTime.Seconds()),
        NIceDb::TUpdate<Schema::ForcedCompactions::EndTime>(forcedCompactionInfo.EndTime.Seconds()),
        NIceDb::TUpdate<Schema::ForcedCompactions::TotalShardCount>(forcedCompactionInfo.TotalShardCount),
        NIceDb::TUpdate<Schema::ForcedCompactions::DoneShardCount>(forcedCompactionInfo.DoneShardCount)
    );

    if (forcedCompactionInfo.UserSID) {
        db.Table<Schema::ForcedCompactions>().Key(forcedCompactionInfo.Id).Update(
            NIceDb::TUpdate<Schema::ForcedCompactions::UserSID>(*forcedCompactionInfo.UserSID)
        );
    }

    for (const auto& shardId : shardsToCompact) {
        db.Table<Schema::WaitingForcedCompactionShards>().Key(shardId.GetOwnerId(), shardId.GetLocalId()).Update(
            NIceDb::TUpdate<Schema::WaitingForcedCompactionShards::ForcedCompactionId>(forcedCompactionInfo.Id)
        );
    }
}

void TSchemeShard::FromForcedCompactionInfo(NKikimrForcedCompaction::TForcedCompaction& compaction, const TForcedCompactionInfo& info) {
    compaction.SetId(info.Id);

    if (info.StartTime != TInstant::Zero()) {
        *compaction.MutableStartTime() = SecondsToProtoTimeStamp(info.StartTime.Seconds());
    }
    if (info.EndTime != TInstant::Zero()) {
        *compaction.MutableEndTime() = SecondsToProtoTimeStamp(info.EndTime.Seconds());
    }

    if (info.UserSID) {
        compaction.SetUserSID(*info.UserSID);
    }

    TPath table = TPath::Init(info.TablePathId, this);
    compaction.MutableSettings()->set_source_path(table.PathString());
    compaction.MutableSettings()->set_cascade(info.Cascade);
    compaction.MutableSettings()->set_max_shards_in_flight(info.MaxShardsInFlight);

    float progress = info.TotalShardCount > 0 ? (100.f * info.DoneShardCount / info.TotalShardCount) : 0;

    compaction.SetProgress(progress);
}

void TSchemeShard::ProcessForcedCompactionQueues() {
    auto attempts = ForcedCompactionTablesQueue.Size();
    for (; attempts > 0; attempts--) {
        const auto& tablePathId = ForcedCompactionTablesQueue.Front();
        auto& compaction = ForcedCompactionsByTable.at(tablePathId);
        auto& shards = ForcedCompactionShardsByTable.at(tablePathId);
        if (!shards.Empty() && compaction->MaxShardsInFlight > compaction->ShardsInFlight.size()) {
            const auto& shardIdx = shards.Front();
            EnqueueForcedCompaction(shards.Front());
            compaction->ShardsInFlight.insert(shardIdx);
            shards.PopFront();
        }
        if (!shards.Empty()) {
            ForcedCompactionTablesQueue.PopFrontToBack();
        }
    }
}

void TSchemeShard::Handle(TEvForcedCompaction::TEvCreateRequest::TPtr& ev, const TActorContext& ctx) {
    Execute(CreateTxCreateForcedCompaction(ev), ctx);
}

NOperationQueue::EStartStatus TSchemeShard::StartForcedCompaction(const TShardIdx& shardIdx) {
    auto ctx = ActorContext();

    auto it = ShardInfos.find(shardIdx);
    if (it == ShardInfos.end()) {
        LOG_WARN_S(ctx, NKikimrServices::FLAT_TX_SCHEMESHARD, "[ForcedCompaction] [Start] Failed to resolve shard info "
            "for forced compaction# " << shardIdx
            << " at schemeshard# " << TabletID());

        return NOperationQueue::EStartStatus::EOperationRemove;
    }

    const auto& datashardId = it->second.TabletID;
    const auto& pathId = it->second.PathId;

    LOG_INFO_S(ctx, NKikimrServices::FLAT_TX_SCHEMESHARD, "[ForcedCompaction] [Start] Compacting "
        "for pathId# " << pathId << ", datashard# " << datashardId
        << ", next wakeup in# " << ForcedCompactionQueue->GetWakeupDelta()
        << ", rate# " << ForcedCompactionQueue->GetRate()
        << ", in queue# " << ForcedCompactionQueue->Size() << " shards"
        << ", running# " << ForcedCompactionQueue->RunningSize() << " shards"
        << " at schemeshard " << TabletID());

    std::unique_ptr<TEvDataShard::TEvCompactTable> request(
        new TEvDataShard::TEvCompactTable(pathId.OwnerId, pathId.LocalPathId));
    request->Record.SetCompactBorrowed(true);

    PipeClientCache->Send(
        ctx,
        ui64(datashardId),
        request.release(),
        static_cast<ui64>(ECompactionType::Forced));

    return NOperationQueue::EStartStatus::EOperationRunning;
}

void TSchemeShard::HandleForcedCompactionResult(TEvDataShard::TEvCompactTableResult::TPtr &ev, const TActorContext &ctx) {
    const auto& record = ev->Get()->Record;

    const TTabletId tabletId(record.GetTabletId());
    const TShardIdx shardIdx = GetShardIdx(tabletId);

    auto pathId = TPathId(
        record.GetPathId().GetOwnerId(),
        record.GetPathId().GetLocalId());

    if (ForcedCompactionQueue) {
        auto duration = ForcedCompactionQueue->OnDone(shardIdx);
        LOG_INFO_S(ctx, NKikimrServices::FLAT_TX_SCHEMESHARD, "[ForcedCompaction] [Finished] Compaction completed "
            "for pathId# " << pathId << ", datashard# " << tabletId
            << ", shardIdx# " << shardIdx
            << " in# " << duration.MilliSeconds() << " ms, with status# " << (int)record.GetStatus()
            << " at schemeshard " << TabletID());
    }
}

void TSchemeShard::EnqueueForcedCompaction(const TShardIdx& shardIdx) {
    if (!ForcedCompactionQueue)
        return;

    auto ctx = ActorContext();

    if (ForcedCompactionQueue->Enqueue(shardIdx)) {
        LOG_TRACE_S(ctx, NKikimrServices::FLAT_TX_SCHEMESHARD,
            "[ForcedCompaction] [Enqueue] Enqueued shard# " << shardIdx << " at schemeshard " << TabletID());
    }
}

void TSchemeShard::OnForcedCompactionTimeout(const TShardIdx& shardIdx) {
    auto ctx = ActorContext();

    auto it = ShardInfos.find(shardIdx);
    if (it == ShardInfos.end()) {
        LOG_WARN_S(ctx, NKikimrServices::FLAT_TX_SCHEMESHARD, "[ForcedCompaction] [Timeout] Failed to resolve shard info "
            "for timeout forced compaction# " << shardIdx
            << " at schemeshard# " << TabletID());
        return;
    }

    const auto& datashardId = it->second.TabletID;
    const auto& pathId = it->second.PathId;

    LOG_INFO_S(ctx, NKikimrServices::FLAT_TX_SCHEMESHARD, "[ForcedCompaction] [Timeout] Compaction timeouted "
        "for pathId# " << pathId << ", datashard# " << datashardId
        << ", next wakeup in# " << ForcedCompactionQueue->GetWakeupDelta()
        << ", in queue# " << ForcedCompactionQueue->Size() << " shards"
        << ", running# " << ForcedCompactionQueue->RunningSize() << " shards"
        << " at schemeshard " << TabletID());
}

} // namespace NKikimr::NSchemeShard
