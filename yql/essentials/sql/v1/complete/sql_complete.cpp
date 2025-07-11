#include "sql_complete.h"

#include <yql/essentials/sql/v1/complete/syntax/grammar.h>
#include <yql/essentials/sql/v1/complete/text/word.h>
#include <yql/essentials/sql/v1/complete/name/object/simple/static/schema.h>
#include <yql/essentials/sql/v1/complete/name/service/ranking/dummy.h>
#include <yql/essentials/sql/v1/complete/name/service/binding/name_service.h>
#include <yql/essentials/sql/v1/complete/name/service/column/name_service.h>
#include <yql/essentials/sql/v1/complete/name/service/schema/name_service.h>
#include <yql/essentials/sql/v1/complete/name/service/static/name_service.h>
#include <yql/essentials/sql/v1/complete/name/service/union/name_service.h>
#include <yql/essentials/sql/v1/complete/syntax/format.h>
#include <yql/essentials/sql/v1/complete/analysis/global/global.h>
#include <yql/essentials/sql/v1/complete/analysis/local/local.h>

#include <util/generic/algorithm.h>
#include <util/charset/utf8.h>

namespace NSQLComplete {

    class TSqlCompletionEngine: public ISqlCompletionEngine {
    public:
        TSqlCompletionEngine(
            TLexerSupplier lexer,
            INameService::TPtr names,
            ISqlCompletionEngine::TConfiguration configuration)
            : Configuration_(std::move(configuration))
            , SyntaxAnalysis_(MakeLocalSyntaxAnalysis(
                  lexer,
                  Configuration_.IgnoredRules_,
                  Configuration_.DisabledPreviousByToken_,
                  Configuration_.ForcedPreviousByToken_))
            , GlobalAnalysis_(MakeGlobalAnalysis())
            , Names_(std::move(names))
        {
        }

        TCompletion
        Complete(TCompletionInput input, TEnvironment env = {}) override {
            return CompleteAsync(input, env).ExtractValueSync();
        }

        NThreading::TFuture<TCompletion> CompleteAsync(TCompletionInput input, TEnvironment env) override {
            if (
                input.CursorPosition < input.Text.length() &&
                    IsUTF8ContinuationByte(input.Text.at(input.CursorPosition)) ||
                input.Text.length() < input.CursorPosition) {
                ythrow yexception()
                    << "invalid cursor position " << input.CursorPosition
                    << " for input size " << input.Text.size();
            }

            TLocalSyntaxContext context = SyntaxAnalysis_->Analyze(input);
            auto keywords = context.Keywords;

            TGlobalContext global = GlobalAnalysis_->Analyze(input, std::move(env));

            TNameRequest request = NameRequestFrom(input, context, global);
            if (request.IsEmpty()) {
                return NThreading::MakeFuture<TCompletion>({
                    .CompletedToken = GetCompletedToken(input, context.EditRange),
                    .Candidates = {},
                });
            }

            TVector<INameService::TPtr> children;

            children.emplace_back(MakeBindingNameService(std::move(global.Names)));

            if (!context.Binding && global.Column) {
                children.emplace_back(MakeColumnNameService(std::move(global.Column->Columns)));
            }

            if (!context.Binding) {
                children.emplace_back(Names_);
            }

            return MakeUnionNameService(std::move(children), MakeDummyRanking())
                ->Lookup(std::move(request))
                .Apply([this, input, context = std::move(context)](auto f) {
                    return ToCompletion(input, std::move(context), f.ExtractValue());
                });
        }

    private:
        TCompletedToken GetCompletedToken(TCompletionInput input, TEditRange editRange) const {
            return {
                .Content = input.Text.SubStr(editRange.Begin, editRange.Length),
                .SourcePosition = editRange.Begin,
            };
        }

        TNameRequest NameRequestFrom(
            TCompletionInput input,
            const TLocalSyntaxContext& context, // TODO(YQL-19747): rename to `local`
            const TGlobalContext& global) const {
            TNameRequest request = {
                .Prefix = TString(GetCompletedToken(input, context.EditRange).Content),
                .Limit = Configuration_.Limit,
            };

            for (const auto& [first, _] : context.Keywords) {
                request.Keywords.emplace_back(first);
            }

            if (context.Pragma) {
                TPragmaName::TConstraints constraints;
                constraints.Namespace = context.Pragma->Namespace;
                request.Constraints.Pragma = std::move(constraints);
            }

            if (context.Type) {
                request.Constraints.Type = TTypeName::TConstraints();
            }

            if (context.Function) {
                TFunctionName::TConstraints constraints;
                constraints.Namespace = context.Function->Namespace;
                request.Constraints.Function = std::move(constraints);
            }

            if (context.Hint) {
                THintName::TConstraints constraints;
                constraints.Statement = context.Hint->StatementKind;
                request.Constraints.Hint = std::move(constraints);
            }

            if (context.Object) {
                request.Constraints.Object = TObjectNameConstraints();
                request.Constraints.Object->Kinds = context.Object->Kinds;
                request.Prefix = context.Object->Path;
            }

            if (context.Object && global.Use) {
                request.Constraints.Object->Provider = global.Use->Provider;
                request.Constraints.Object->Cluster = global.Use->Cluster;
            }

            if (context.Object && context.Object->HasCluster()) {
                request.Constraints.Object->Provider = context.Object->Provider;
                request.Constraints.Object->Cluster = context.Object->Cluster;
            }

            if (context.Cluster) {
                TClusterName::TConstraints constraints;
                constraints.Namespace = ""; // TODO(YQL-19747): filter by provider
                request.Constraints.Cluster = std::move(constraints);
            }

            if (auto name = global.EnclosingFunction.Transform(NormalizeName);
                name && name == "concat") {
                auto& object = request.Constraints.Object;
                object = object.Defined() ? object : TObjectNameConstraints();
                object->Kinds.emplace(EObjectKind::Folder);
                object->Kinds.emplace(EObjectKind::Table);
            }

            if (context.Column && global.Column) {
                TMaybe<TStringBuf> table = context.Column->Table;
                table = !table->empty() ? table : Nothing();

                request.Constraints.Column = TColumnName::TConstraints();
                request.Constraints.Column->Tables =
                    TColumnContext(*global.Column).ExtractAliased(table).Tables;
            }

            return request;
        }

        TCompletion ToCompletion(
            TCompletionInput input,
            TLocalSyntaxContext context,
            TNameResponse response) const {
            TCompletion completion = {
                .CompletedToken = GetCompletedToken(input, context.EditRange),
                .Candidates = Convert(std::move(response.RankedNames), std::move(context)),
            };

            if (response.NameHintLength) {
                const auto length = *response.NameHintLength;
                TEditRange editRange = {
                    .Begin = input.CursorPosition - length,
                    .Length = length,
                };
                completion.CompletedToken = GetCompletedToken(input, editRange);
            }

            return completion;
        }

        static TVector<TCandidate> Convert(TVector<TGenericName> names, TLocalSyntaxContext context) {
            TVector<TCandidate> candidates;
            candidates.reserve(names.size());
            for (auto& name : names) {
                candidates.emplace_back(Convert(std::move(name), context));
            }
            return candidates;
        }

        // TODO(YQL-19747): extract to a separate file
        static TCandidate Convert(TGenericName name, TLocalSyntaxContext& context) {
            return std::visit([&](auto&& name) -> TCandidate {
                using T = std::decay_t<decltype(name)>;

                if constexpr (std::is_base_of_v<TKeyword, T>) {
                    TVector<TString>& seq = context.Keywords[name.Content];
                    seq.insert(std::begin(seq), name.Content);

                    TCandidate candidate = {
                        .Kind = ECandidateKind::Keyword,
                        .Content = FormatKeywords(seq),
                    };

                    if (candidate.Content.EndsWith('(')) {
                        candidate.Content += ')';
                        candidate.CursorShift = 1;
                    }

                    return candidate;
                }

                if constexpr (std::is_base_of_v<TPragmaName, T>) {
                    return {ECandidateKind::PragmaName, std::move(name.Indentifier)};
                }

                if constexpr (std::is_base_of_v<TTypeName, T>) {
                    TCandidate candidate = {
                        .Kind = ECandidateKind::TypeName,
                        .Content = std::move(name.Indentifier),
                    };

                    switch (name.Kind) {
                        case TTypeName::EKind::Simple: {
                        } break;
                        case TTypeName::EKind::Container: {
                            candidate.Content += "<>";
                            candidate.CursorShift = 1;
                        } break;
                        case TTypeName::EKind::Parameterized: {
                            candidate.Content += "()";
                            candidate.CursorShift = 1;
                        } break;
                    }

                    return candidate;
                }

                if constexpr (std::is_base_of_v<TFunctionName, T>) {
                    TCandidate candidate = {
                        .Kind = ECandidateKind::FunctionName,
                        .Content = std::move(name.Indentifier),
                    };

                    candidate.Content += "()";
                    candidate.CursorShift = 1;

                    return candidate;
                }

                if constexpr (std::is_base_of_v<THintName, T>) {
                    return {ECandidateKind::HintName, std::move(name.Indentifier)};
                }

                if constexpr (std::is_base_of_v<TFolderName, T>) {
                    TCandidate candidate = {
                        .Kind = ECandidateKind::FolderName,
                        .Content = std::move(name.Indentifier),
                    };

                    if (!context.IsQuoted.AtLhs) {
                        candidate.Content.prepend('`');
                    }

                    candidate.Content.append('/');

                    if (!context.IsQuoted.AtRhs) {
                        candidate.Content.append('`');
                        candidate.CursorShift = 1;
                    }

                    return candidate;
                }

                if constexpr (std::is_base_of_v<TTableName, T>) {
                    if (!context.IsQuoted.AtLhs) {
                        name.Indentifier.prepend('`');
                    }
                    if (!context.IsQuoted.AtRhs) {
                        name.Indentifier.append('`');
                    }
                    return {ECandidateKind::TableName, std::move(name.Indentifier)};
                }

                if constexpr (std::is_base_of_v<TClusterName, T>) {
                    return {ECandidateKind::ClusterName, std::move(name.Indentifier)};
                }

                if constexpr (std::is_base_of_v<TColumnName, T>) {
                    if (context.Column->Table.empty() && !name.TableAlias.empty()) {
                        name.Indentifier.prepend('.');
                        name.Indentifier.prepend(name.TableAlias);
                    }

                    return {ECandidateKind::ColumnName, std::move(name.Indentifier)};
                }

                if constexpr (std::is_base_of_v<TBindingName, T>) {
                    if (!context.Binding) {
                        name.Indentifier.prepend('$');
                    }
                    return {ECandidateKind::BindingName, std::move(name.Indentifier)};
                }

                if constexpr (std::is_base_of_v<TUnkownName, T>) {
                    return {ECandidateKind::UnknownName, std::move(name.Content)};
                }
            }, std::move(name));
        }

        TConfiguration Configuration_;
        ILocalSyntaxAnalysis::TPtr SyntaxAnalysis_;
        IGlobalAnalysis::TPtr GlobalAnalysis_;
        INameService::TPtr Names_;
    };

    ISqlCompletionEngine::TConfiguration MakeConfiguration(THashSet<TString> allowedStmts) {
        allowedStmts.emplace("sql_stmt");

        ISqlCompletionEngine::TConfiguration config;
        for (const std::string& name : GetSqlGrammar().GetAllRules()) {
            if (name.ends_with("_stmt") && !allowedStmts.contains(name)) {
                config.IgnoredRules_.emplace(name);
            }
        }
        return config;
    }

    ISqlCompletionEngine::TConfiguration MakeYDBConfiguration() {
        ISqlCompletionEngine::TConfiguration config;
        config.IgnoredRules_ = {
            "use_stmt",
            "import_stmt",
            "export_stmt",
        };
        return config;
    }

    ISqlCompletionEngine::TConfiguration MakeYQLConfiguration() {
        auto config = MakeConfiguration(/* allowedStmts = */ {
            "lambda_stmt",
            "pragma_stmt",
            "select_stmt",
            "named_nodes_stmt",
            "drop_table_stmt",
            "use_stmt",
            "into_table_stmt",
            "commit_stmt",
            "declare_stmt",
            "import_stmt",
            "export_stmt",
            "do_stmt",
            "define_action_or_subquery_stmt",
            "if_stmt",
            "for_stmt",
            "values_stmt",
        });

        config.DisabledPreviousByToken_ = {};

        config.ForcedPreviousByToken_ = {
            {"PARALLEL", {}},
            {"TABLESTORE", {}},
            {"FOR", {"EVALUATE"}},
            {"IF", {"EVALUATE"}},
            {"EXTERNAL", {"USING"}},
        };

        return config;
    }

    ISqlCompletionEngine::TPtr MakeSqlCompletionEngine(
        TLexerSupplier lexer,
        INameService::TPtr names,
        ISqlCompletionEngine::TConfiguration configuration) {
        return MakeHolder<TSqlCompletionEngine>(
            lexer, std::move(names), std::move(configuration));
    }

} // namespace NSQLComplete

template <>
void Out<NSQLComplete::ECandidateKind>(IOutputStream& out, NSQLComplete::ECandidateKind value) {
    switch (value) {
        case NSQLComplete::ECandidateKind::Keyword:
            out << "Keyword";
            break;
        case NSQLComplete::ECandidateKind::PragmaName:
            out << "PragmaName";
            break;
        case NSQLComplete::ECandidateKind::TypeName:
            out << "TypeName";
            break;
        case NSQLComplete::ECandidateKind::FunctionName:
            out << "FunctionName";
            break;
        case NSQLComplete::ECandidateKind::HintName:
            out << "HintName";
            break;
        case NSQLComplete::ECandidateKind::FolderName:
            out << "FolderName";
            break;
        case NSQLComplete::ECandidateKind::TableName:
            out << "TableName";
            break;
        case NSQLComplete::ECandidateKind::ClusterName:
            out << "ClusterName";
            break;
        case NSQLComplete::ECandidateKind::BindingName:
            out << "BindingName";
            break;
        case NSQLComplete::ECandidateKind::ColumnName:
            out << "ColumnName";
            break;
        case NSQLComplete::ECandidateKind::UnknownName:
            out << "UnknownName";
            break;
    }
}

template <>
void Out<NSQLComplete::TCandidate>(IOutputStream& out, const NSQLComplete::TCandidate& value) {
    out << "{" << value.Kind << ", \"" << value.Content << "\"";
    if (value.CursorShift != 0) {
        out << ", " << value.CursorShift;
    }
    out << "}";
}
