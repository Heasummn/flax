// OperatorOverloads.cpp
// Copyright (c) 2014 - The Foreseeable Future, zhiayang@gmail.com
// Licensed under the Apache License Version 2.0.


#include "ast.h"
#include "codegen.h"

using namespace Ast;
using namespace Codegen;

Result_t OpOverload::codegen(CodegenInstance* cgi, llvm::Value* lhsPtr, llvm::Value* rhs)
{
	// this is never really called for actual codegen. operators are handled as functions,
	// so we just put them into the structs' funcs.
	// BinOp will do a lookup on the opMap, but never call codegen for this.

	// however, this will get called, because we need to know if the parameters for
	// the operator overload are legit. people ignore our return value.

	FuncDecl* decl = this->func->decl;
	if(this->op == ArithmeticOp::Assign)
	{
		if(decl->params.size() != 1)
		{
			error(cgi, this, "Operator overload for '=' can only have one argument (have %zu)", decl->params.size());
		}

		// we can't actually do much, because they can assign to anything
	}
	else if(this->op == ArithmeticOp::CmpEq)
	{
		if(decl->params.size() != 1)
			error(cgi, this, "Operator overload for '==' can only have one argument");

		if(decl->type.strType != "Bool")
			error(cgi, this, "Operator overload for '==' must return a boolean value");
	}
	else if(this->op == ArithmeticOp::Add || this->op == ArithmeticOp::Subtract || this->op == ArithmeticOp::Multiply
		|| this->op == ArithmeticOp::Divide || this->op == ArithmeticOp::PlusEquals || this->op == ArithmeticOp::MinusEquals
		|| this->op == ArithmeticOp::MultiplyEquals || this->op == ArithmeticOp::DivideEquals)
	{
		if(decl->params.size() != 1)
			error(cgi, this, "Operator overload can only have one argument");
	}


	// custom operator: can't check shit.

	// else
	// {
	// 	error(cgi, this, "(%s:%d) -> Internal check failed: invalid operator", __FILE__, __LINE__);
	// }

	return Result_t(0, 0);
}




























