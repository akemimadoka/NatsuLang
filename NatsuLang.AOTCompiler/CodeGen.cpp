﻿#include "CodeGen.h"

#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/Support/Host.h>
#include <llvm/Support/TargetRegistry.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/LegacyPassManager.h>

using namespace NatsuLib;
using namespace NatsuLang;
using namespace Compiler;

AotCompiler::AotDiagIdMap::AotDiagIdMap(NatsuLib::natRefPointer<NatsuLib::TextReader<NatsuLib::StringType::Utf8>> const& reader)
{
	using DiagIDUnderlyingType = std::underlying_type_t<Diag::DiagnosticsEngine::DiagID>;

	std::unordered_map<nStrView, Diag::DiagnosticsEngine::DiagID> idNameMap;
	for (auto id = static_cast<DiagIDUnderlyingType>(Diag::DiagnosticsEngine::DiagID::Invalid) + 1;
		id < static_cast<DiagIDUnderlyingType>(Diag::DiagnosticsEngine::DiagID::EndOfDiagID); ++id)
	{
		if (auto idName = Diag::DiagnosticsEngine::GetDiagIDName(static_cast<Diag::DiagnosticsEngine::DiagID>(id)))
		{
			idNameMap.emplace(idName, static_cast<Diag::DiagnosticsEngine::DiagID>(id));
		}
	}

	nString diagIDName;
	while (true)
	{
		auto curLine = reader->ReadLine();

		if (diagIDName.IsEmpty())
		{
			if (curLine.IsEmpty())
			{
				break;
			}

			diagIDName = std::move(curLine);
		}
		else
		{
			const auto iter = idNameMap.find(diagIDName);
			if (iter == idNameMap.cend())
			{
				// TODO: 报告错误的 ID
				break;
			}

			if (!m_IdMap.try_emplace(iter->second, std::move(curLine)).second)
			{
				// TODO: 报告重复的 ID
			}

			diagIDName.Clear();
		}
	}
}

AotCompiler::AotDiagIdMap::~AotDiagIdMap()
{
}

nString AotCompiler::AotDiagIdMap::GetText(Diag::DiagnosticsEngine::DiagID id)
{
	const auto iter = m_IdMap.find(id);
	return iter == m_IdMap.cend() ? u8"(No available text)"_nv : iter->second.GetView();
}

AotCompiler::AotDiagConsumer::AotDiagConsumer(AotCompiler& compiler)
	: m_Compiler{ compiler }, m_Errored{ false }
{
}

AotCompiler::AotDiagConsumer::~AotDiagConsumer()
{
}

void AotCompiler::AotDiagConsumer::HandleDiagnostic(Diag::DiagnosticsEngine::Level level, Diag::DiagnosticsEngine::Diagnostic const& diag)
{
	m_Errored |= Diag::DiagnosticsEngine::IsUnrecoverableLevel(level);

	nuInt levelId;

	switch (level)
	{
	case Diag::DiagnosticsEngine::Level::Ignored:
	case Diag::DiagnosticsEngine::Level::Note:
	case Diag::DiagnosticsEngine::Level::Remark:
		levelId = natLog::Msg;
		break;
	case Diag::DiagnosticsEngine::Level::Warning:
		levelId = natLog::Warn;
		break;
	case Diag::DiagnosticsEngine::Level::Error:
	case Diag::DiagnosticsEngine::Level::Fatal:
	default:
		levelId = natLog::Err;
		break;
	}

	m_Compiler.m_Logger.Log(levelId, diag.GetDiagMessage());

	const auto loc = diag.GetSourceLocation();
	if (loc.GetFileID())
	{
		const auto [succeed, fileContent] = m_Compiler.m_SourceManager.GetFileContent(loc.GetFileID());
		if (const auto line = loc.GetLineInfo(); succeed && line)
		{
			size_t offset{};
			for (nuInt i = 1; i < line; ++i)
			{
				offset = fileContent.Find(Environment::GetNewLine(), static_cast<ptrdiff_t>(offset));
				if (offset == nStrView::npos)
				{
					// TODO: 无法定位到源文件
					return;
				}

				offset += Environment::GetNewLine().GetSize();
			}

			const auto nextNewLine = fileContent.Find(Environment::GetNewLine(), static_cast<ptrdiff_t>(offset));
			const auto column = loc.GetColumnInfo();
			offset += column ? column - 1 : 0;
			if (nextNewLine <= offset)
			{
				// TODO: 无法定位到源文件
				return;
			}

			m_Compiler.m_Logger.Log(levelId, fileContent.Slice(static_cast<ptrdiff_t>(offset), nextNewLine == nStrView::npos ? -1 : static_cast<ptrdiff_t>(nextNewLine)));
			m_Compiler.m_Logger.Log(levelId, u8"^"_nv);
		}
	}
}

AotCompiler::AotAstConsumer::AotAstConsumer(AotCompiler& compiler)
	: m_Compiler{ compiler }
{
}

AotCompiler::AotAstConsumer::~AotAstConsumer()
{
}

void AotCompiler::AotAstConsumer::Initialize(ASTContext& context)
{
}

void AotCompiler::AotAstConsumer::HandleTranslationUnit(ASTContext& context)
{
}

nBool AotCompiler::AotAstConsumer::HandleTopLevelDecl(Linq<Valued<Declaration::DeclPtr>> const& decls)
{
	for (auto decl : decls)
	{
		if (const auto funcDecl = static_cast<natRefPointer<Declaration::FunctionDecl>>(decl))
		{
			switch (funcDecl->GetStorageClass())
			{
			case Specifier::StorageClass::None:
			{
				AotStmtVisitor visitor{ m_Compiler, funcDecl };
				visitor.StartVisit();
				break;
			}
			case Specifier::StorageClass::Extern:
			{
				const auto functionType = m_Compiler.getCorrespondingType(funcDecl->GetValueType());
				const auto functionName = funcDecl->GetIdentifierInfo()->GetName();

				const auto funcValue = llvm::Function::Create(static_cast<llvm::FunctionType*>(functionType),
					// TODO: 修改链接性
					llvm::GlobalVariable::ExternalLinkage,
					std::string(functionName.cbegin(), functionName.cend()),
					m_Compiler.m_Module.get());

				auto argIter = funcValue->arg_begin();
				const auto argEnd = funcValue->arg_end();
				auto paramIter = funcDecl->GetParams().begin();
				const auto paramEnd = funcDecl->GetParams().end();

				for (; argIter != argEnd && paramIter != paramEnd; ++argIter, static_cast<void>(++paramIter))
				{
					const auto name = (*paramIter)->GetIdentifierInfo()->GetName();
					const std::string nameStr{ name.cbegin(), name.cend() };
					argIter->setName(nameStr);
				}

				m_Compiler.m_FunctionMap.emplace(std::move(funcDecl), funcValue);

				break;
			}
			default:
				break;
			}
		}
	}

	return true;
}

AotCompiler::AotStmtVisitor::AotStmtVisitor(AotCompiler& compiler, natRefPointer<Declaration::FunctionDecl> funcDecl)
	: m_Compiler{ compiler }, m_CurrentFunction{ std::move(funcDecl) },
	  m_LastVisitedValue{ nullptr }, m_RequiredModifiableValue{ false }
{
	const auto functionType = m_Compiler.getCorrespondingType(m_CurrentFunction->GetValueType());
	const auto functionName = m_CurrentFunction->GetIdentifierInfo()->GetName();

	m_CurrentFunctionValue = llvm::Function::Create(static_cast<llvm::FunctionType*>(functionType),
	                                                // TODO: 修改链接性
	                                                llvm::GlobalVariable::ExternalLinkage,
	                                                std::string(functionName.cbegin(), functionName.cend()),
	                                                m_Compiler.m_Module.get());

	auto argIter = m_CurrentFunctionValue->arg_begin();
	const auto argEnd = m_CurrentFunctionValue->arg_end();
	auto paramIter = m_CurrentFunction->GetParams().begin();
	const auto paramEnd = m_CurrentFunction->GetParams().end();

	const auto block = llvm::BasicBlock::Create(m_Compiler.m_LLVMContext, "Entry", m_CurrentFunctionValue);
	m_Compiler.m_IRBuilder.SetInsertPoint(block);

	for (; argIter != argEnd && paramIter != paramEnd; ++argIter, static_cast<void>(++paramIter))
	{
		const auto name = (*paramIter)->GetIdentifierInfo()->GetName();
		const std::string nameStr{ name.cbegin(), name.cend() };
		argIter->setName(nameStr);

		llvm::IRBuilder<> entryIRBuilder{ &m_CurrentFunctionValue->getEntryBlock(), m_CurrentFunctionValue->getEntryBlock().begin() };
		const auto arg = entryIRBuilder.CreateAlloca(argIter->getType(), nullptr, nameStr);
		m_Compiler.m_IRBuilder.CreateStore(&*argIter, arg);

		m_DeclMap.emplace(*paramIter, arg);
	}

	m_Compiler.m_FunctionMap.emplace(m_CurrentFunction, m_CurrentFunctionValue);
}

AotCompiler::AotStmtVisitor::~AotStmtVisitor()
{
}

void AotCompiler::AotStmtVisitor::VisitBreakStmt(natRefPointer<Statement::BreakStmt> const& stmt)
{
	nat_Throw(AotCompilerException, u8"此功能尚未实现"_nv);
}

void AotCompiler::AotStmtVisitor::VisitCatchStmt(natRefPointer<Statement::CatchStmt> const& stmt)
{
	nat_Throw(AotCompilerException, u8"此功能尚未实现"_nv);
}

void AotCompiler::AotStmtVisitor::VisitTryStmt(natRefPointer<Statement::TryStmt> const& stmt)
{
	nat_Throw(AotCompilerException, u8"此功能尚未实现"_nv);
}

void AotCompiler::AotStmtVisitor::VisitCompoundStmt(natRefPointer<Statement::CompoundStmt> const& stmt)
{
	for (auto&& item : stmt->GetChildrens())
	{
		Visit(item);
	}
}

void AotCompiler::AotStmtVisitor::VisitContinueStmt(natRefPointer<Statement::ContinueStmt> const& stmt)
{
	nat_Throw(AotCompilerException, u8"此功能尚未实现"_nv);
}

void AotCompiler::AotStmtVisitor::VisitDeclStmt(natRefPointer<Statement::DeclStmt> const& stmt)
{
	for (auto decl : stmt->GetDecls())
	{
		if (!decl)
		{
			nat_Throw(AotCompilerException, u8"错误的声明"_nv);
		}

		if (auto varDecl = static_cast<natRefPointer<Declaration::VarDecl>>(decl))
		{
			if (varDecl->IsFunction())
			{
				// 目前只能在顶层声明函数
				continue;
			}

			const auto type = varDecl->GetValueType();
			llvm::Value* arraySize = nullptr;
			if (const auto arrayType = static_cast<natRefPointer<Type::ArrayType>>(type))
			{
				arraySize = llvm::ConstantInt::get(llvm::Type::getInt32Ty(m_Compiler.m_LLVMContext), arrayType->GetSize());
			}
			const auto varName = varDecl->GetName();
			const auto valueType = m_Compiler.getCorrespondingType(type);

			if (varDecl->GetStorageClass() == Specifier::StorageClass::None)
			{
				const auto storage = m_Compiler.m_IRBuilder.CreateAlloca(valueType, arraySize, std::string(varName.cbegin(), varName.cend()));

				if (const auto initExpr = varDecl->GetInitializer())
				{
					Visit(initExpr);
					const auto initializer = m_LastVisitedValue;
					m_Compiler.m_IRBuilder.CreateStore(initializer, storage);
				}
				else
				{
					m_Compiler.m_IRBuilder.CreateStore(llvm::Constant::getNullValue(valueType), storage);
				}

				m_DeclMap.emplace(varDecl, storage);
			}
		}
	}
}

void AotCompiler::AotStmtVisitor::VisitDoStmt(natRefPointer<Statement::DoStmt> const& stmt)
{
	nat_Throw(AotCompilerException, u8"此功能尚未实现"_nv);
}

void AotCompiler::AotStmtVisitor::VisitExpr(natRefPointer<Expression::Expr> const& expr)
{
	nat_Throw(AotCompilerException, u8"此功能尚未实现"_nv);
}

void AotCompiler::AotStmtVisitor::VisitConditionalOperator(natRefPointer<Expression::ConditionalOperator> const& expr)
{
	nat_Throw(AotCompilerException, u8"此功能尚未实现"_nv);
}

void AotCompiler::AotStmtVisitor::VisitArraySubscriptExpr(natRefPointer<Expression::ArraySubscriptExpr> const& expr)
{
	nat_Throw(AotCompilerException, u8"此功能尚未实现"_nv);
}

void AotCompiler::AotStmtVisitor::VisitBinaryOperator(natRefPointer<Expression::BinaryOperator> const& expr)
{
	const auto builtinLeftOperandType = static_cast<natRefPointer<Type::BuiltinType>>(expr->GetLeftOperand()->GetExprType());
	Visit(expr->GetLeftOperand());
	const auto leftOperand = m_LastVisitedValue;

	const auto builtinRightOperandType = static_cast<natRefPointer<Type::BuiltinType>>(expr->GetRightOperand()->GetExprType());
	Visit(expr->GetRightOperand());
	const auto rightOperand = m_LastVisitedValue;

	const auto opCode = expr->GetOpcode();
	// 左右操作数类型应当相同
	const auto commonType = expr->GetLeftOperand()->GetExprType();
	const auto resultType = static_cast<natRefPointer<Type::BuiltinType>>(expr->GetExprType());

	m_LastVisitedValue = EmitBinOp(leftOperand, rightOperand, opCode, commonType, resultType);
}

void AotCompiler::AotStmtVisitor::VisitCompoundAssignOperator(natRefPointer<Expression::CompoundAssignOperator> const& expr)
{
	const auto resultType = expr->GetLeftOperand()->GetExprType();

	EvaluateAsModifiableValue(expr->GetLeftOperand());
	const auto leftOperand = m_LastVisitedValue;

	Visit(expr->GetRightOperand());
	const auto rightOperand = m_LastVisitedValue;

	const auto opCode = expr->GetOpcode();
	llvm::Value* value;

	switch (opCode)
	{
	case Expression::BinaryOperationType::Assign:
		value = rightOperand;
		break;
	case Expression::BinaryOperationType::MulAssign:
		value = EmitBinOp(m_Compiler.m_IRBuilder.CreateLoad(leftOperand), rightOperand, Expression::BinaryOperationType::Mul, resultType, resultType);
		break;
	case Expression::BinaryOperationType::DivAssign:
		value = EmitBinOp(m_Compiler.m_IRBuilder.CreateLoad(leftOperand), rightOperand, Expression::BinaryOperationType::Div, resultType, resultType);
		break;
	case Expression::BinaryOperationType::RemAssign:
		value = EmitBinOp(m_Compiler.m_IRBuilder.CreateLoad(leftOperand), rightOperand, Expression::BinaryOperationType::Mod, resultType, resultType);
		break;
	case Expression::BinaryOperationType::AddAssign:
		value = EmitBinOp(m_Compiler.m_IRBuilder.CreateLoad(leftOperand), rightOperand, Expression::BinaryOperationType::Add, resultType, resultType);
		break;
	case Expression::BinaryOperationType::SubAssign:
		value = EmitBinOp(m_Compiler.m_IRBuilder.CreateLoad(leftOperand), rightOperand, Expression::BinaryOperationType::Sub, resultType, resultType);
		break;
	case Expression::BinaryOperationType::ShlAssign:
		value = EmitBinOp(m_Compiler.m_IRBuilder.CreateLoad(leftOperand), rightOperand, Expression::BinaryOperationType::Shl, resultType, resultType);
		break;
	case Expression::BinaryOperationType::ShrAssign:
		value = EmitBinOp(m_Compiler.m_IRBuilder.CreateLoad(leftOperand), rightOperand, Expression::BinaryOperationType::Shr, resultType, resultType);
		break;
	case Expression::BinaryOperationType::AndAssign:
		value = EmitBinOp(m_Compiler.m_IRBuilder.CreateLoad(leftOperand), rightOperand, Expression::BinaryOperationType::And, resultType, resultType);
		break;
	case Expression::BinaryOperationType::XorAssign:
		value = EmitBinOp(m_Compiler.m_IRBuilder.CreateLoad(leftOperand), rightOperand, Expression::BinaryOperationType::Xor, resultType, resultType);
		break;
	case Expression::BinaryOperationType::OrAssign:
		value = EmitBinOp(m_Compiler.m_IRBuilder.CreateLoad(leftOperand), rightOperand, Expression::BinaryOperationType::Or, resultType, resultType);
		break;
	default:
		assert(!"Invalid Opcode");
		nat_Throw(AotCompilerException, u8"无效的 Opcode"_nv);
	}

	m_Compiler.m_IRBuilder.CreateStore(value, leftOperand);
	m_LastVisitedValue = leftOperand;
}

void AotCompiler::AotStmtVisitor::VisitBooleanLiteral(natRefPointer<Expression::BooleanLiteral> const& expr)
{
	m_LastVisitedValue = llvm::ConstantInt::get(llvm::Type::getInt1Ty(m_Compiler.m_LLVMContext), expr->GetValue());
}

void AotCompiler::AotStmtVisitor::VisitConstructExpr(natRefPointer<Expression::ConstructExpr> const& expr)
{
	nat_Throw(AotCompilerException, u8"此功能尚未实现"_nv);
}

void AotCompiler::AotStmtVisitor::VisitDeleteExpr(natRefPointer<Expression::DeleteExpr> const& expr)
{
	nat_Throw(AotCompilerException, u8"此功能尚未实现"_nv);
}

void AotCompiler::AotStmtVisitor::VisitNewExpr(natRefPointer<Expression::NewExpr> const& expr)
{
	nat_Throw(AotCompilerException, u8"此功能尚未实现"_nv);
}

void AotCompiler::AotStmtVisitor::VisitThisExpr(natRefPointer<Expression::ThisExpr> const& expr)
{
	nat_Throw(AotCompilerException, u8"此功能尚未实现"_nv);
}

void AotCompiler::AotStmtVisitor::VisitThrowExpr(natRefPointer<Expression::ThrowExpr> const& expr)
{
	nat_Throw(AotCompilerException, u8"此功能尚未实现"_nv);
}

void AotCompiler::AotStmtVisitor::VisitCallExpr(natRefPointer<Expression::CallExpr> const& expr)
{
	Visit(expr->GetCallee());
	const auto callee = llvm::dyn_cast<llvm::Function>(m_LastVisitedValue);
	assert(callee);

	if (callee->arg_size() != expr->GetArgCount())
	{
		nat_Throw(AotCompilerException, u8"参数数量不匹配，这可能是默认参数功能未实现导致的"_nv);
	}

	std::vector<llvm::Value*> args;
	args.reserve(expr->GetArgCount());

	// TODO: 实现默认参数
	for (auto&& arg : expr->GetArgs())
	{
		Visit(arg);
		assert(m_LastVisitedValue);
		args.emplace_back(m_LastVisitedValue);
	}

	m_LastVisitedValue = m_Compiler.m_IRBuilder.CreateCall(callee, args, "call");
}

void AotCompiler::AotStmtVisitor::VisitMemberCallExpr(natRefPointer<Expression::MemberCallExpr> const& expr)
{
	nat_Throw(AotCompilerException, u8"此功能尚未实现"_nv);
}

void AotCompiler::AotStmtVisitor::VisitCastExpr(natRefPointer<Expression::CastExpr> const& expr)
{
	const auto operand = expr->GetOperand();
	Visit(operand);
	m_LastVisitedValue = ConvertScalarTo(m_LastVisitedValue, operand->GetExprType(), expr->GetExprType());
}

void AotCompiler::AotStmtVisitor::VisitAsTypeExpr(natRefPointer<Expression::AsTypeExpr> const& expr)
{
	const auto operand = expr->GetOperand();
	Visit(operand);
	m_LastVisitedValue = ConvertScalarTo(m_LastVisitedValue, operand->GetExprType(), expr->GetExprType());
}

void AotCompiler::AotStmtVisitor::VisitImplicitCastExpr(natRefPointer<Expression::ImplicitCastExpr> const& expr)
{
	const auto operand = expr->GetOperand();
	Visit(operand);
	m_LastVisitedValue = ConvertScalarTo(m_LastVisitedValue, operand->GetExprType(), expr->GetExprType());
}

void AotCompiler::AotStmtVisitor::VisitCharacterLiteral(natRefPointer<Expression::CharacterLiteral> const& expr)
{
	m_LastVisitedValue = llvm::ConstantInt::get(m_Compiler.getCorrespondingType(expr->GetExprType()), expr->GetCodePoint());
}

void AotCompiler::AotStmtVisitor::VisitDeclRefExpr(natRefPointer<Expression::DeclRefExpr> const& expr)
{
	const auto decl = expr->GetDecl();

	const auto declIter = m_DeclMap.find(decl);
	if (declIter != m_DeclMap.end())
	{
		m_LastVisitedValue = m_RequiredModifiableValue ? declIter->second : m_Compiler.m_IRBuilder.CreateLoad(declIter->second);
		return;
	}

	if (!m_RequiredModifiableValue)
	{
		const auto funcIter = m_Compiler.m_FunctionMap.find(decl);
		if (funcIter != m_Compiler.m_FunctionMap.end())
		{
			m_LastVisitedValue = funcIter->second;
			return;
		}
	}

	nat_Throw(AotCompilerException, u8"定义引用表达式引用了不存在或不合法的定义"_nv);
}

void AotCompiler::AotStmtVisitor::VisitFloatingLiteral(natRefPointer<Expression::FloatingLiteral> const& expr)
{
	const auto floatType = static_cast<natRefPointer<Type::BuiltinType>>(expr->GetExprType());

	if (floatType->GetBuiltinClass() == Type::BuiltinType::Float)
	{
		m_LastVisitedValue = llvm::ConstantFP::get(m_Compiler.m_LLVMContext, llvm::APFloat{ static_cast<nFloat>(expr->GetValue()) });
	}
	else
	{
		m_LastVisitedValue = llvm::ConstantFP::get(m_Compiler.m_LLVMContext, llvm::APFloat{ expr->GetValue() });
	}
}

void AotCompiler::AotStmtVisitor::VisitIntegerLiteral(natRefPointer<Expression::IntegerLiteral> const& expr)
{
	const auto intType = static_cast<natRefPointer<Type::BuiltinType>>(expr->GetExprType());
	const auto typeInfo = m_Compiler.m_AstContext.GetTypeInfo(intType);

	m_LastVisitedValue = llvm::ConstantInt::get(m_Compiler.m_LLVMContext, llvm::APInt{static_cast<unsigned>(typeInfo.Size * 8), expr->GetValue(), intType->IsSigned() });
}

void AotCompiler::AotStmtVisitor::VisitMemberExpr(natRefPointer<Expression::MemberExpr> const& expr)
{
	nat_Throw(AotCompilerException, u8"此功能尚未实现"_nv);
}

void AotCompiler::AotStmtVisitor::VisitParenExpr(natRefPointer<Expression::ParenExpr> const& expr)
{
	Visit(expr->GetInnerExpr());
}

void AotCompiler::AotStmtVisitor::VisitStmtExpr(natRefPointer<Expression::StmtExpr> const& expr)
{
	nat_Throw(AotCompilerException, u8"此功能尚未实现"_nv);
}

void AotCompiler::AotStmtVisitor::VisitStringLiteral(natRefPointer<Expression::StringLiteral> const& expr)
{
	nat_Throw(AotCompilerException, u8"此功能尚未实现"_nv);
}

void AotCompiler::AotStmtVisitor::VisitUnaryExprOrTypeTraitExpr(natRefPointer<Expression::UnaryExprOrTypeTraitExpr> const& expr)
{
	nat_Throw(AotCompilerException, u8"此功能尚未实现"_nv);
}

void AotCompiler::AotStmtVisitor::VisitUnaryOperator(natRefPointer<Expression::UnaryOperator> const& expr)
{
	const auto operand = expr->GetOperand();
	const auto opType = operand->GetExprType();
	const auto opCode = expr->GetOpcode();

	switch (opCode)
	{
	case Expression::UnaryOperationType::PostInc:
		EvaluateAsModifiableValue(operand);
		m_LastVisitedValue = EmitIncDec(m_LastVisitedValue, opType, true, false);
		break;
	case Expression::UnaryOperationType::PreInc:
		EvaluateAsModifiableValue(operand);
		m_LastVisitedValue = EmitIncDec(m_LastVisitedValue, opType, true, true);
		break;
	case Expression::UnaryOperationType::PostDec:
		EvaluateAsModifiableValue(operand);
		m_LastVisitedValue = EmitIncDec(m_LastVisitedValue, opType, false, false);
		break;
	case Expression::UnaryOperationType::PreDec:
		EvaluateAsModifiableValue(operand);
		m_LastVisitedValue = EmitIncDec(m_LastVisitedValue, opType, false, true);
		break;
	case Expression::UnaryOperationType::Plus:
		Visit(operand);
		break;
	case Expression::UnaryOperationType::Minus:
		Visit(operand);
		m_LastVisitedValue = m_Compiler.m_IRBuilder.CreateNeg(m_LastVisitedValue);
		break;
	case Expression::UnaryOperationType::Not:
		Visit(operand);
		m_LastVisitedValue = m_Compiler.m_IRBuilder.CreateNot(m_LastVisitedValue);
		break;
	case Expression::UnaryOperationType::LNot:
		EvaluateAsBool(operand);
		m_LastVisitedValue = m_Compiler.m_IRBuilder.CreateIsNull(m_LastVisitedValue);
		break;
	default:
		assert(!"Invalid opCode");
		[[fallthrough]];
	case Expression::UnaryOperationType::Invalid:
		nat_Throw(AotCompilerException, u8"错误的操作码"_nv);
	}
}

void AotCompiler::AotStmtVisitor::VisitForStmt(natRefPointer<Statement::ForStmt> const& stmt)
{
	nat_Throw(AotCompilerException, u8"此功能尚未实现"_nv);
}

void AotCompiler::AotStmtVisitor::VisitGotoStmt(natRefPointer<Statement::GotoStmt> const& stmt)
{
	nat_Throw(AotCompilerException, u8"此功能尚未实现"_nv);
}

void AotCompiler::AotStmtVisitor::VisitIfStmt(natRefPointer<Statement::IfStmt> const& stmt)
{
	Visit(stmt->GetCond());
	const auto condExpr = m_LastVisitedValue;

	const auto thenStmt = stmt->GetThen();
	const auto elseStmt = stmt->GetElse();

	const auto trueBranch = llvm::BasicBlock::Create(m_Compiler.m_LLVMContext, "");
	const auto endBranch = llvm::BasicBlock::Create(m_Compiler.m_LLVMContext, "");
	const auto falseBranch = elseStmt ? llvm::BasicBlock::Create(m_Compiler.m_LLVMContext, "") : endBranch;

	m_Compiler.m_IRBuilder.CreateCondBr(condExpr, trueBranch, falseBranch);

	EmitBlock(trueBranch);
	Visit(thenStmt);
	EmitBranch(endBranch);

	if (elseStmt)
	{
		EmitBlock(falseBranch);
		Visit(elseStmt);
		EmitBranch(endBranch);
	}

	EmitBlock(endBranch, true);
}

void AotCompiler::AotStmtVisitor::VisitLabelStmt(natRefPointer<Statement::LabelStmt> const& stmt)
{
	// TODO: 添加标签
	Visit(stmt->GetSubStmt());
}

void AotCompiler::AotStmtVisitor::VisitNullStmt(natRefPointer<Statement::NullStmt> const& stmt)
{
}

void AotCompiler::AotStmtVisitor::VisitReturnStmt(natRefPointer<Statement::ReturnStmt> const& stmt)
{
	if (const auto retExpr = stmt->GetReturnExpr())
	{
		Visit(retExpr);
		m_Compiler.m_IRBuilder.CreateRet(m_LastVisitedValue);
	}
	else
	{
		m_Compiler.m_IRBuilder.CreateRetVoid();
	}
}

void AotCompiler::AotStmtVisitor::VisitSwitchCase(natRefPointer<Statement::SwitchCase> const& stmt)
{
	nat_Throw(AotCompilerException, u8"此功能尚未实现"_nv);
}

void AotCompiler::AotStmtVisitor::VisitCaseStmt(natRefPointer<Statement::CaseStmt> const& stmt)
{
	nat_Throw(AotCompilerException, u8"此功能尚未实现"_nv);
}

void AotCompiler::AotStmtVisitor::VisitDefaultStmt(natRefPointer<Statement::DefaultStmt> const& stmt)
{
	nat_Throw(AotCompilerException, u8"此功能尚未实现"_nv);
}

void AotCompiler::AotStmtVisitor::VisitSwitchStmt(natRefPointer<Statement::SwitchStmt> const& stmt)
{
	nat_Throw(AotCompilerException, u8"此功能尚未实现"_nv);
}

void AotCompiler::AotStmtVisitor::VisitWhileStmt(natRefPointer<Statement::WhileStmt> const& stmt)
{
	nat_Throw(AotCompilerException, u8"此功能尚未实现"_nv);
}

void AotCompiler::AotStmtVisitor::VisitStmt(natRefPointer<Statement::Stmt> const& stmt)
{
	nat_Throw(AotCompilerException, u8"此功能尚未实现"_nv);
}

void AotCompiler::AotStmtVisitor::StartVisit()
{
	Visit(m_CurrentFunction->GetBody());
}

llvm::Function* AotCompiler::AotStmtVisitor::GetFunction() const
{
	std::string verifyInfo;
	llvm::raw_string_ostream os{ verifyInfo };
	if (verifyFunction(*m_CurrentFunctionValue, &os))
	{
		nat_Throw(AotCompilerException, u8"函数验证错误，信息为 {0}"_nv, verifyInfo);
	}

	return m_CurrentFunctionValue;
}

void AotCompiler::AotStmtVisitor::EmitBranch(llvm::BasicBlock* target)
{
	const auto curBlock = m_Compiler.m_IRBuilder.GetInsertBlock();

	if (curBlock && !curBlock->getTerminator())
	{
		m_Compiler.m_IRBuilder.CreateBr(target);
	}

	m_Compiler.m_IRBuilder.ClearInsertionPoint();
}

void AotCompiler::AotStmtVisitor::EmitBlock(llvm::BasicBlock* block, nBool finished)
{
	const auto curBlock = m_Compiler.m_IRBuilder.GetInsertBlock();

	EmitBranch(block);

	if (finished && block->use_empty())
	{
		delete block;
		return;
	}

	if (curBlock && curBlock->getParent())
	{
		m_CurrentFunctionValue->getBasicBlockList().insertAfter(curBlock->getIterator(), block);
	}
	else
	{
		m_CurrentFunctionValue->getBasicBlockList().push_back(block);
	}

	m_Compiler.m_IRBuilder.SetInsertPoint(block);
}

llvm::Value* AotCompiler::AotStmtVisitor::EmitBinOp(llvm::Value* leftOperand, llvm::Value* rightOperand, Expression::BinaryOperationType opCode, natRefPointer<Type::BuiltinType> const& commonType, natRefPointer<Type::BuiltinType> const& resultType)
{
	switch (opCode)
	{
	case Expression::BinaryOperationType::Mul:
		return m_Compiler.m_IRBuilder.CreateMul(leftOperand, rightOperand, "mul");
	case Expression::BinaryOperationType::Div:
		if (commonType->IsSigned())
		{
			return m_Compiler.m_IRBuilder.CreateSDiv(leftOperand, rightOperand, "div");
		}

		return m_Compiler.m_IRBuilder.CreateUDiv(leftOperand, rightOperand, "div");
	case Expression::BinaryOperationType::Mod:
		if (commonType->IsSigned())
		{
			return m_Compiler.m_IRBuilder.CreateSRem(leftOperand, rightOperand, "mod");
		}

		return m_Compiler.m_IRBuilder.CreateURem(leftOperand, rightOperand, "mod");
	case Expression::BinaryOperationType::Add:
		return m_Compiler.m_IRBuilder.CreateAdd(leftOperand, rightOperand, "add");
	case Expression::BinaryOperationType::Sub:
		return m_Compiler.m_IRBuilder.CreateSub(leftOperand, rightOperand, "sub");
	case Expression::BinaryOperationType::Shl:
		return m_Compiler.m_IRBuilder.CreateShl(leftOperand, rightOperand, "shl");
	case Expression::BinaryOperationType::Shr:
		if (commonType->IsSigned())
		{
			return m_Compiler.m_IRBuilder.CreateAShr(leftOperand, rightOperand, "shr");
		}

		return m_Compiler.m_IRBuilder.CreateLShr(leftOperand, rightOperand, "shr");
	case Expression::BinaryOperationType::LT:
		if (commonType->IsFloatingType())
		{
			return m_Compiler.m_IRBuilder.CreateFCmpOLT(leftOperand, rightOperand, "cmp");
		}

		if (commonType->IsSigned())
		{
			return m_Compiler.m_IRBuilder.CreateICmpSLT(leftOperand, rightOperand, "cmp");
		}

		return m_Compiler.m_IRBuilder.CreateICmpULT(leftOperand, rightOperand, "cmp");
	case Expression::BinaryOperationType::GT:
		if (commonType->IsFloatingType())
		{
			return m_Compiler.m_IRBuilder.CreateFCmpOGT(leftOperand, rightOperand, "cmp");
		}

		if (commonType->IsSigned())
		{
			return m_Compiler.m_IRBuilder.CreateICmpSGT(leftOperand, rightOperand, "cmp");
		}

		return m_Compiler.m_IRBuilder.CreateICmpUGT(leftOperand, rightOperand, "cmp");
	case Expression::BinaryOperationType::LE:
		if (commonType->IsFloatingType())
		{
			return m_Compiler.m_IRBuilder.CreateFCmpOLE(leftOperand, rightOperand, "cmp");
		}

		if (commonType->IsSigned())
		{
			return m_Compiler.m_IRBuilder.CreateICmpSLE(leftOperand, rightOperand, "cmp");
		}

		return m_Compiler.m_IRBuilder.CreateICmpULE(leftOperand, rightOperand, "cmp");
	case Expression::BinaryOperationType::GE:
		if (commonType->IsFloatingType())
		{
			return m_Compiler.m_IRBuilder.CreateFCmpOGE(leftOperand, rightOperand, "cmp");
		}

		if (commonType->IsSigned())
		{
			return m_Compiler.m_IRBuilder.CreateICmpSGE(leftOperand, rightOperand, "cmp");
		}

		return m_Compiler.m_IRBuilder.CreateICmpUGE(leftOperand, rightOperand, "cmp");
	case Expression::BinaryOperationType::EQ:
		if (commonType->IsFloatingType())
		{
			return m_Compiler.m_IRBuilder.CreateFCmpOEQ(leftOperand, rightOperand, "cmp");
		}

		return m_Compiler.m_IRBuilder.CreateICmpEQ(leftOperand, rightOperand, "cmp");
	case Expression::BinaryOperationType::NE:
		if (commonType->IsFloatingType())
		{
			return m_Compiler.m_IRBuilder.CreateFCmpUNE(leftOperand, rightOperand, "cmp");
		}

		return m_Compiler.m_IRBuilder.CreateICmpNE(leftOperand, rightOperand, "cmp");
	case Expression::BinaryOperationType::And:
		return m_Compiler.m_IRBuilder.CreateAnd(leftOperand, rightOperand, "and");
	case Expression::BinaryOperationType::Xor:
		return m_Compiler.m_IRBuilder.CreateXor(leftOperand, rightOperand, "xor");
	case Expression::BinaryOperationType::Or:
		return m_Compiler.m_IRBuilder.CreateOr(leftOperand, rightOperand, "or");
	case Expression::BinaryOperationType::LAnd:
	{
		auto rhsBlock = llvm::BasicBlock::Create(m_Compiler.m_LLVMContext, "land.rhs");
		const auto endBlock = llvm::BasicBlock::Create(m_Compiler.m_LLVMContext, "land.end");

		m_Compiler.m_IRBuilder.CreateCondBr(leftOperand, rhsBlock, endBlock);

		const auto phiNode = llvm::PHINode::Create(llvm::Type::getInt1Ty(m_Compiler.m_LLVMContext), 2, "", endBlock);

		for (auto i = llvm::pred_begin(endBlock), end = llvm::pred_end(endBlock); i != end; ++i)
		{
			phiNode->addIncoming(llvm::ConstantInt::getFalse(m_Compiler.m_LLVMContext), *i);
		}

		EmitBlock(rhsBlock);
		const auto rhsCond = ConvertScalarToBool(rightOperand, m_Compiler.m_AstContext.GetBuiltinType(Type::BuiltinType::Bool));

		rhsBlock = m_Compiler.m_IRBuilder.GetInsertBlock();

		EmitBlock(endBlock);
		phiNode->addIncoming(rhsCond, rhsBlock);

		return m_Compiler.m_IRBuilder.CreateZExtOrBitCast(phiNode, m_Compiler.getCorrespondingType(resultType));
	}
	case Expression::BinaryOperationType::LOr:
	{
		auto rhsBlock = llvm::BasicBlock::Create(m_Compiler.m_LLVMContext, "lor.rhs");
		const auto endBlock = llvm::BasicBlock::Create(m_Compiler.m_LLVMContext, "lor.end");

		m_Compiler.m_IRBuilder.CreateCondBr(leftOperand, endBlock, rhsBlock);

		const auto phiNode = llvm::PHINode::Create(llvm::Type::getInt1Ty(m_Compiler.m_LLVMContext), 2, "", endBlock);

		for (auto i = llvm::pred_begin(endBlock), end = llvm::pred_end(endBlock); i != end; ++i)
		{
			phiNode->addIncoming(llvm::ConstantInt::getTrue(m_Compiler.m_LLVMContext), *i);
		}

		EmitBlock(rhsBlock);
		const auto rhsCond = ConvertScalarToBool(rightOperand, m_Compiler.m_AstContext.GetBuiltinType(Type::BuiltinType::Bool));

		rhsBlock = m_Compiler.m_IRBuilder.GetInsertBlock();

		EmitBlock(endBlock);
		phiNode->addIncoming(rhsCond, rhsBlock);

		return m_Compiler.m_IRBuilder.CreateZExtOrBitCast(phiNode, m_Compiler.getCorrespondingType(resultType));
	}
	default:
		assert(!"Invalid Opcode");
		nat_Throw(AotCompilerException, u8"无效的 Opcode"_nv);
	}
}

llvm::Value* AotCompiler::AotStmtVisitor::EmitIncDec(llvm::Value* operand, natRefPointer<Type::BuiltinType> const& opType, nBool isInc, nBool isPre)
{
	llvm::Value* value = m_Compiler.m_IRBuilder.CreateLoad(operand);

	if (opType->IsFloatingType())
	{
		value = m_Compiler.m_IRBuilder.CreateFAdd(value, llvm::ConstantFP::get(m_Compiler.getCorrespondingType(opType), isInc ? 1.0 : -1.0));
	}
	else
	{
		value = m_Compiler.m_IRBuilder.CreateAdd(value, llvm::ConstantInt::get(m_Compiler.getCorrespondingType(opType), isInc ? 1 : -1, opType->IsSigned()));
	}

	m_Compiler.m_IRBuilder.CreateStore(value, operand);
	return isPre ? operand : value;
}

void AotCompiler::AotStmtVisitor::EvaluateAsModifiableValue(Expression::ExprPtr const& expr)
{
	assert(expr);

	m_RequiredModifiableValue = true;
	const auto scope = make_scope([this]
	{
		m_RequiredModifiableValue = false;
	});

	Visit(expr);
}

void AotCompiler::AotStmtVisitor::EvaluateAsBool(Expression::ExprPtr const& expr)
{
	assert(expr);

	Visit(expr);

	m_LastVisitedValue = ConvertScalarToBool(m_LastVisitedValue, expr->GetExprType());
}

llvm::Value* AotCompiler::AotStmtVisitor::ConvertScalarTo(llvm::Value* from, Type::TypePtr fromType, Type::TypePtr toType)
{
	fromType = Type::Type::GetUnderlyingType(fromType);
	toType = Type::Type::GetUnderlyingType(toType);

	if (fromType == toType)
	{
		return from;
	}

	const auto builtinFromType = static_cast<natRefPointer<Type::BuiltinType>>(fromType), builtinToType = static_cast<natRefPointer<Type::BuiltinType>>(toType);

	if (!builtinFromType || !builtinToType)
	{
		// TODO: 报告错误
		return nullptr;
	}

	if (builtinToType->GetBuiltinClass() == Type::BuiltinType::Void)
	{
		return nullptr;
	}

	if (builtinToType->GetBuiltinClass() == Type::BuiltinType::Bool)
	{
		return ConvertScalarToBool(from, builtinFromType);
	}

	const auto llvmFromType = m_Compiler.getCorrespondingType(fromType), llvmToType = m_Compiler.getCorrespondingType(toType);

	if (llvmFromType == llvmToType)
	{
		return from;
	}

	if (builtinFromType->IsIntegerType())
	{
		const auto fromSigned = builtinFromType->IsSigned();

		if (builtinToType->IsIntegerType())
		{
			return m_Compiler.m_IRBuilder.CreateIntCast(from, llvmToType, fromSigned, "scalarconv");
		}
		
		if (fromSigned)
		{
			return m_Compiler.m_IRBuilder.CreateSIToFP(from, llvmToType, "scalarconv");
		}
		
		return m_Compiler.m_IRBuilder.CreateUIToFP(from, llvmToType, "scalarconv");
	}
	
	if (builtinToType->IsIntegerType())
	{
		if (builtinToType->IsSigned())
		{
			return m_Compiler.m_IRBuilder.CreateFPToSI(from, llvmToType, "scalarconv");
		}

		return m_Compiler.m_IRBuilder.CreateFPToUI(from, llvmToType, "scalarconv");
	}

	nInt result;
	const auto succeed = builtinFromType->CompareRankTo(builtinToType, result);
	assert(succeed);

	if (result > 0)
	{
		return m_Compiler.m_IRBuilder.CreateFPTrunc(from, llvmToType, "scalarconv");
	}

	return m_Compiler.m_IRBuilder.CreateFPExt(from, llvmToType, "scalarconv");
}

llvm::Value* AotCompiler::AotStmtVisitor::ConvertScalarToBool(llvm::Value* from, natRefPointer<Type::BuiltinType> const& fromType)
{
	assert(from && fromType);

	if (fromType->IsFloatingType())
	{
		const auto floatingZero = llvm::ConstantFP::getNullValue(from->getType());
		return m_Compiler.m_IRBuilder.CreateFCmpUNE(from, floatingZero, "floatingtobool");
	}

	assert(fromType->IsIntegerType());
	return m_Compiler.m_IRBuilder.CreateIsNotNull(from, "inttobool");
}

AotCompiler::AotCompiler(natRefPointer<TextReader<StringType::Utf8>> const& diagIdMapFile, natLog& logger)
	: m_DiagConsumer{ make_ref<AotDiagConsumer>(*this) },
	m_Diag{ make_ref<AotDiagIdMap>(diagIdMapFile), m_DiagConsumer },
	m_Logger{ logger },
	m_SourceManager{ m_Diag, m_FileManager },
	m_Preprocessor{ m_Diag, m_SourceManager },
	m_Consumer{ make_ref<AotAstConsumer>(*this) },
	m_Sema{ m_Preprocessor, m_AstContext, m_Consumer },
	m_Parser{ m_Preprocessor, m_Sema },
	m_IRBuilder{ m_LLVMContext }
{
	LLVMInitializeX86TargetInfo();
	LLVMInitializeX86Target();
	LLVMInitializeX86TargetMC();
	LLVMInitializeX86AsmParser();
	LLVMInitializeX86AsmPrinter();
}

AotCompiler::~AotCompiler()
{
}

void AotCompiler::Compile(Uri const& uri, llvm::raw_pwrite_stream& stream)
{
	const auto path = uri.GetPath();
	m_Module = std::make_unique<llvm::Module>(std::string(path.begin(), path.end()), m_LLVMContext);
	const auto targetTriple = llvm::sys::getDefaultTargetTriple();
	m_Module->setTargetTriple(targetTriple);
	std::string error;
	const auto target = llvm::TargetRegistry::lookupTarget(targetTriple, error);
	if (!target)
	{
		m_Logger.LogErr(u8"{0}"_nv, error);
		return;
	}

	const llvm::TargetOptions opt;
	const llvm::Optional<llvm::Reloc::Model> RM{};
	const auto machine = target->createTargetMachine(targetTriple, "generic", "", opt, RM, llvm::None, llvm::CodeGenOpt::Default);
	m_Module->setDataLayout(machine->createDataLayout());

	const auto fileId = m_SourceManager.GetFileID(uri);
	const auto [succeed, content] = m_SourceManager.GetFileContent(fileId);
	if (!succeed)
	{
		nat_Throw(AotCompilerException, u8"无法取得文件 \"{0}\" 的内容"_nv, uri.GetUnderlyingString());
	}

	auto lexer = make_ref<Lex::Lexer>(content, m_Preprocessor);
	lexer->SetFileID(fileId);
	m_Preprocessor.SetLexer(std::move(lexer));
	m_Parser.ConsumeToken();
	ParseAST(m_Parser);

	if (m_DiagConsumer->IsErrored())
	{
		m_Logger.LogErr(u8"编译文件 \"{0}\" 失败"_nv, uri.GetUnderlyingString());
		return;
	}

#if !defined(NDEBUG)
	std::string buffer;
	llvm::raw_string_ostream os{ buffer };
	m_Module->print(os, nullptr);
	m_Logger.LogMsg(u8"编译成功，生成的 IR:\n{0}"_nv, buffer);
#endif // NDEBUG

	llvm::legacy::PassManager passManager;
	machine->addPassesToEmitFile(passManager, stream, llvm::TargetMachine::CGFT_ObjectFile);
	passManager.run(*m_Module);
	stream.flush();

	m_Module.reset();
}

llvm::Type* AotCompiler::getCorrespondingType(Type::TypePtr const& type)
{
	const auto underlyingType = Type::Type::GetUnderlyingType(type);
	const auto typeClass = underlyingType->GetType();

	switch (typeClass)
	{
	case Type::Type::Builtin:
	{
		const auto builtinType = static_cast<natRefPointer<Type::BuiltinType>>(underlyingType);
		switch (builtinType->GetBuiltinClass())
		{
		case Type::BuiltinType::Void:
			return llvm::Type::getVoidTy(m_LLVMContext);
		case Type::BuiltinType::Bool:
			return llvm::Type::getInt1Ty(m_LLVMContext);
		case Type::BuiltinType::Char:
		case Type::BuiltinType::UShort:
		case Type::BuiltinType::UInt:
		case Type::BuiltinType::ULong:
		case Type::BuiltinType::ULongLong:
		case Type::BuiltinType::UInt128:
		case Type::BuiltinType::Short:
		case Type::BuiltinType::Int:
		case Type::BuiltinType::Long:
		case Type::BuiltinType::LongLong:
		case Type::BuiltinType::Int128:
			return llvm::IntegerType::get(m_LLVMContext, m_AstContext.GetTypeInfo(builtinType).Size * 8);
		case Type::BuiltinType::Float:
			return llvm::Type::getFloatTy(m_LLVMContext);
		case Type::BuiltinType::Double:
			return llvm::Type::getDoubleTy(m_LLVMContext);
		case Type::BuiltinType::LongDouble:
		case Type::BuiltinType::Float128:
			return llvm::Type::getFP128Ty(m_LLVMContext);
		case Type::BuiltinType::Overload:
		case Type::BuiltinType::BoundMember:
		case Type::BuiltinType::BuiltinFn:
		default:
			assert(!"Invalid BuiltinClass");
			[[fallthrough]];
		case Type::BuiltinType::Invalid:
			nat_Throw(AotCompilerException, u8"此功能尚未实现"_nv);
		}
	}
	case Type::Type::Array:
	{
		const auto arrayType = static_cast<natRefPointer<Type::ArrayType>>(underlyingType);
		return llvm::ArrayType::get(getCorrespondingType(arrayType->GetElementType()), static_cast<std::uint64_t>(arrayType->GetSize()));
	}
	case Type::Type::Function:
	{
		const auto functionType = static_cast<natRefPointer<Type::FunctionType>>(underlyingType);
		const auto args{ functionType->GetParameterTypes().select([this](Type::TypePtr const& argType)
		{
			return getCorrespondingType(argType);
		}).Cast<std::vector<llvm::Type*>>() };

		return llvm::FunctionType::get(getCorrespondingType(functionType->GetResultType()), args, false);
	}
	case Type::Type::Record:
		nat_Throw(AotCompilerException, u8"此功能尚未实现"_nv);
	case Type::Type::Enum:
		nat_Throw(AotCompilerException, u8"此功能尚未实现"_nv);
	default:
		assert(!"Invalid type");
		[[fallthrough]];
	case Type::Type::Paren:
	case Type::Type::TypeOf:
	case Type::Type::Auto:
		assert(!"Should never happen, check NatsuLang::Type::Type::GetUnderlyingType");
		nat_Throw(AotCompilerException, u8"错误的类型"_nv);
	}
}
