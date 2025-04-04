#pragma once

#include <util/generic/fwd.h>
#include <util/generic/list.h>
#include <util/generic/hash.h>

TString GetYdbEndpoint();
TString GetYdbDatabase();

TString RunYdb(const TList<TString>& args1, const TList<TString>& args2, bool checkExitCode = true, bool autoAddEndpointAndDatabase = true, const THashMap<TString, TString>& env = {}, int expectedExitCode = 0);

ui64 GetFullTimeValue(const TString& output);
THashSet<TString> GetCodecsList(const TString& output);

ui64 GetCommitTimeValue(const TString& output);

void EnsureStatisticsColumns(const TList<TString>& args,
                             const TVector<TString>& columns1,
                             const TVector<TString>& columns2);

void UnitAssertColumnsOrder(TString line,
                            const TVector<TString>& columns);
