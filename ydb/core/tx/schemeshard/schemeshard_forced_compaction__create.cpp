#include "schemeshard_forced_compaction.h"
#include "schemeshard_impl.h"

#define LOG_N(stream) LOG_NOTICE_S(ctx, NKikimrServices::FLAT_TX_SCHEMESHARD, "[" << Self->SelfTabletId() << "][ForcedCompaction] " << stream)

namespace NKikimr::NSchemeShard {

using namespace NTabletFlatExecutor;

struct TSchemeShard::TForcedCompaction::TTxCreate: public TRwTxBase {
    explicit TTxCreate(TSelf* self, TEvForcedCompaction::TEvCreateRequest::TPtr& ev)
        : TRwTxBase(self)
        , Request(ev)
    {}

    void DoExecute(TTransactionContext &txc, const TActorContext &ctx) override {
        const auto& request = Request->Get()->Record;
        const auto& settings = request.GetSettings();
        LOG_N("DoExecute " << request.ShortDebugString());

        auto response = MakeHolder<TEvForcedCompaction::TEvCreateResponse>(Request->Get()->Record.GetTxId());

        ui64 id = request.GetTxId();

        if (Self->ForcedCompactions.contains(id)) {
            return Reply(std::move(response), Ydb::StatusIds::ALREADY_EXISTS, TStringBuilder()
                << "Forced compaction with id '" << id << "' already exists");
        }

        const auto domainPath = TPath::Resolve(request.GetDatabaseName(), Self);
        {
            const auto checks = domainPath.Check();
            checks
                .IsAtLocalSchemeShard()
                .IsResolved()
                .NotDeleted()
                .NotUnderDeleting()
                .IsSubDomain()
                .NotUnderDomainUpgrade();

            if (!checks) {
                return Reply(std::move(response), Ydb::StatusIds::BAD_REQUEST, checks.GetError());
            }
        }

        const auto tablePath = TPath::Resolve(settings.source_path(), Self);
        {
            const auto checks = tablePath.Check();
            checks
                .IsAtLocalSchemeShard()
                .IsResolved()
                .NotDeleted()
                .NotUnderDeleting()
                .IsTable()
                .IsCommonSensePath()
                .IsTheSameDomain(domainPath);

            if (!checks) {
                return Reply(std::move(response), Ydb::StatusIds::BAD_REQUEST, checks.GetError());
            }
        }

        auto info = MakeIntrusive<TForcedCompactionInfo>();
        info->Id = id;
        info->TablePathId = tablePath.Base()->PathId;
        info->Cascade = settings.cascade();
        info->MaxShardsInFlight = settings.max_shards_in_flight();
        info->StartTime = TAppData::TimeProvider->Now();
        if (request.HasUserSID()) {
            info->UserSID = request.GetUserSID();
        }

        if (info->Cascade) {
            return Reply(
                std::move(response),
                Ydb::StatusIds::BAD_REQUEST,
                "'cascade = true' is not supported yet");
        }
        if (Self->ForcedCompactionsByTable.contains(info->TablePathId)) {
            return Reply(
                std::move(response),
                Ydb::StatusIds::PRECONDITION_FAILED,
                TStringBuilder() << "Forced compaction already exists for table " << tablePath.PathString());
        }

        auto tableInfo = Self->Tables.at(info->TablePathId);
        if (tableInfo->GetPartitions().empty()) {
            return Reply(
                std::move(response),
                Ydb::StatusIds::PRECONDITION_FAILED,
                "The table has no shards");
        }

        TVector<TShardIdx> shardsToCompact;
        shardsToCompact.reserve(tableInfo->GetPartitions().size());
        for (const auto& shardInfo : tableInfo->GetPartitions()) {
            shardsToCompact.push_back(shardInfo.ShardIdx);
        }

        NIceDb::TNiceDb db(txc.DB);
        Self->PersistForcedCompactionState(db, *info, shardsToCompact);

        Self->AddForcedCompaction(info, shardsToCompact);
        Self->FromForcedCompactionInfo(*response->Record.MutableForcedCompaction(), *info);

        Reply(std::move(response));

        SideEffects.ApplyOnExecute(Self, txc, ctx);
    }

    void DoComplete(const TActorContext &ctx) override {
        Self->ProcessForcedCompactionQueues();
        SideEffects.ApplyOnComplete(Self, ctx);
    }

    void Reply(
        THolder<TEvForcedCompaction::TEvCreateResponse> response,
        const Ydb::StatusIds::StatusCode status = Ydb::StatusIds::SUCCESS,
        const TString& errorMessage = TString())
    {
        auto& record = response->Record;
        record.SetStatus(status);
        if (errorMessage) {
            auto& issue = *record.MutableIssues()->Add();
            issue.set_severity(NYql::TSeverityIds::S_ERROR);
            issue.set_message(errorMessage);

        }

        SideEffects.Send(Request->Sender, std::move(response), 0, Request->Cookie);
    }

private:
    TSideEffects SideEffects;
    TEvForcedCompaction::TEvCreateRequest::TPtr Request;
};

ITransaction* TSchemeShard::CreateTxCreateForcedCompaction(TEvForcedCompaction::TEvCreateRequest::TPtr& ev) {
    return new TForcedCompaction::TTxCreate(this, ev);
}

} // namespace NKikimr::NSchemeShard
