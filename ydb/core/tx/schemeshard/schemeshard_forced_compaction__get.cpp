#include "schemeshard_impl.h"

namespace NKikimr::NSchemeShard {

using namespace NTabletFlatExecutor;

struct TSchemeShard::TForcedCompaction::TTxGet: public TTransactionBase<TSchemeShard> {
    explicit TTxGet(TSelf* self, TEvForcedCompaction::TEvGetRequest::TPtr& ev)
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
    TEvForcedCompaction::TEvGetRequest::TPtr Request;
};

ITransaction* TSchemeShard::CreateTxGetForcedCompaction(TEvForcedCompaction::TEvGetRequest::TPtr& ev) {
    return new TForcedCompaction::TTxGet(this, ev);
}

} // namespace NKikimr::NSchemeShard
