#include "schemeshard_impl.h"

namespace NKikimr::NSchemeShard {

using namespace NTabletFlatExecutor;

struct TSchemeShard::TForcedCompaction::TTxForget: public TTransactionBase<TSchemeShard> {
    explicit TTxForget(TSelf* self, TEvForcedCompaction::TEvForgetRequest::TPtr& ev)
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
    TEvForcedCompaction::TEvForgetRequest::TPtr Request;
};

ITransaction* TSchemeShard::CreateTxForgetForcedCompaction(TEvForcedCompaction::TEvForgetRequest::TPtr& ev) {
    return new TForcedCompaction::TTxForget(this, ev);
}

} // namespace NKikimr::NSchemeShard
