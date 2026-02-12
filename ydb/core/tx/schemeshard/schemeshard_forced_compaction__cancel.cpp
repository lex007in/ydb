#include "schemeshard_impl.h"

namespace NKikimr::NSchemeShard {

using namespace NTabletFlatExecutor;

struct TSchemeShard::TForcedCompaction::TTxCancel: public TTransactionBase<TSchemeShard> {
    explicit TTxCancel(TSelf* self, TEvForcedCompaction::TEvCancelRequest::TPtr& ev)
        : TBase(self)
        , Request(ev)
    {}

    bool Execute(TTransactionContext &txc, const TActorContext &ctx) override {
        Y_UNUSED(txc);
        Y_UNUSED(ctx);
        return true;
    }

    void Complete(const TActorContext &ctx) override {
        Y_UNUSED(ctx);
    }

private:
    TEvForcedCompaction::TEvCancelRequest::TPtr Request;
};

ITransaction* TSchemeShard::CreateTxCancelForcedCompaction(TEvForcedCompaction::TEvCancelRequest::TPtr& ev) {
    return new TForcedCompaction::TTxCancel(this, ev);
}

} // namespace NKikimr::NSchemeShard
