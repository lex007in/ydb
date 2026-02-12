#include "schemeshard_impl.h"

namespace NKikimr::NSchemeShard {

using namespace NTabletFlatExecutor;

struct TSchemeShard::TForcedCompaction::TTxList: public TTransactionBase<TSchemeShard> {
    explicit TTxList(TSelf* self, TEvForcedCompaction::TEvListRequest::TPtr& ev)
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
    TEvForcedCompaction::TEvListRequest::TPtr Request;
};

ITransaction* TSchemeShard::CreateTxListForcedCompaction(TEvForcedCompaction::TEvListRequest::TPtr& ev) {
    return new TForcedCompaction::TTxList(this, ev);
}

} // namespace NKikimr::NSchemeShard
