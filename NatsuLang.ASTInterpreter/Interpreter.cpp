﻿#include "Interpreter.h"

using namespace NatsuLib;
using namespace NatsuLang;
using namespace NatsuLang::Detail;

Interpreter::Interpreter(natRefPointer<TextReader<StringType::Utf8>> const& diagIdMapFile, natLog& logger)
	: m_DiagConsumer{ make_ref<InterpreterDiagConsumer>(*this) },
	  m_Diag{ make_ref<InterpreterDiagIdMap>(diagIdMapFile), m_DiagConsumer },
	  m_Logger{ logger },
	  m_SourceManager{ m_Diag, m_FileManager },
	  m_Preprocessor{ m_Diag, m_SourceManager },
	  m_AstContext{ TargetInfo{ Environment::GetEndianness(), sizeof(void*), alignof(void*) } },
	  m_Consumer{ make_ref<InterpreterASTConsumer>(*this) },
	  m_Sema{ m_Preprocessor, m_AstContext, m_Consumer },
	  m_Parser{ m_Preprocessor, m_Sema },
	  m_Visitor{ *this }, m_DeclStorage{ *this }
{
	m_AstContext.UseDefaultClassLayoutBuilder();
}

Interpreter::~Interpreter()
{
}

void Interpreter::Run(Uri const& uri)
{
	const auto fileID = m_SourceManager.GetFileID(uri);
	m_Preprocessor.SetLexer(make_ref<Lex::Lexer>(fileID, m_SourceManager.GetFileContent(fileID).second, m_Preprocessor));
	m_Parser.ConsumeToken();
	m_Sema.SetCurrentPhase(Semantic::Sema::Phase::Phase1);
	ParseAST(m_Parser);
	EndParsingAST(m_Parser);
	m_DeclStorage.GarbageCollect();
}

void Interpreter::Run(nStrView content)
{
	// 解释器的 REPL 模式将视为第2阶段
	m_Sema.SetCurrentPhase(Semantic::Sema::Phase::Phase2);

	m_Preprocessor.SetLexer(make_ref<Lex::Lexer>(0, content, m_Preprocessor));
	m_Parser.ConsumeToken();
	const auto stmt = m_Parser.ParseStatement(Declaration::Context::Block, true);
	if (!stmt || m_DiagConsumer->HasError())
	{
		m_DiagConsumer->Reset();
		// 玄学问题，加上 u8 前缀会乱码
		nat_Throw(InterpreterException, "编译语句 \"{0}\" 失败"_nv, content);
	}

	m_Visitor.Visit(stmt);
	m_Visitor.ResetReturnedExpr();
	m_DeclStorage.GarbageCollect();
}

Interpreter::InterpreterDeclStorage& Interpreter::GetDeclStorage() noexcept
{
	return m_DeclStorage;
}

void Interpreter::RegisterFunction(nStrView name, Type::TypePtr resultType, std::initializer_list<Type::TypePtr> argTypes, Function const& func)
{
	Lex::Token dummyToken;
	const auto id = m_Parser.GetPreprocessor().FindIdentifierInfo(name, dummyToken);

	auto scope = m_Sema.GetCurrentScope();
	for (; scope->GetParent(); scope = scope->GetParent()) {}

	const auto dc = scope->GetEntity();

	auto funcType = make_ref<Type::FunctionType>(from(argTypes), resultType);
	auto funcDecl = make_ref<Declaration::FunctionDecl>(Declaration::Decl::Function, dc,
		SourceLocation{}, SourceLocation{}, id, funcType, Specifier::StorageClass::Extern);

	Declaration::DeclContext* const funcDc = funcDecl.Get();

	funcDecl->SetParams(from(argTypes).select([funcDc](Type::TypePtr const& argType)
	{
		return make_ref<Declaration::ParmVarDecl>(Declaration::Decl::ParmVar, funcDc,
			SourceLocation{}, SourceLocation{}, nullptr, argType, Specifier::StorageClass::None, nullptr);
	}));

	m_Sema.PushOnScopeChains(funcDecl, scope);
	m_FunctionMap.emplace(std::move(funcDecl), func);
}

ASTContext& Interpreter::GetASTContext() noexcept
{
	return m_AstContext;
}
