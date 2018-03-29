﻿#pragma once
#include <natRefObj.h>

namespace NatsuLang
{
	namespace Statement
	{
		class Stmt;
		using StmtPtr = NatsuLib::natRefPointer<Stmt>;

#define STMT(StmtType, Base) class StmtType;
#define EXPR(ExprType, Base)
#include "Basic/StmtDef.h"
	}

	namespace Expression
	{
#define STMT(StmtType, Base)
#define EXPR(ExprType, Base) class ExprType;
#include "Basic/StmtDef.h"
	}

	struct StmtVisitor
		: NatsuLib::natRefObj
	{
		virtual ~StmtVisitor() = 0;

		virtual void Visit(Statement::StmtPtr const& stmt);

#define STMT(Type, Base) virtual void Visit##Type(NatsuLib::natRefPointer<Statement::Type> const& stmt);
#define EXPR(Type, Base) virtual void Visit##Type(NatsuLib::natRefPointer<Expression::Type> const& expr);
#include "Basic/StmtDef.h"

		virtual void VisitStmt(Statement::StmtPtr const& stmt);
	};
}
