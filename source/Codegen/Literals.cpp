// LiteralCodegen.cpp
// Copyright (c) 2014 - 2015, zhiayang@gmail.com
// Licensed under the Apache License Version 2.0.

#include "ast.h"
#include "codegen.h"

using namespace Ast;
using namespace Codegen;

Result_t Number::codegen(CodegenInstance* cgi, fir::Value* extra)
{
	// check builtin type
	if(!this->decimal)
	{
		if(this->needUnsigned)
			return Result_t(fir::ConstantInt::getUint64((uint64_t) this->ival), 0);

		else
			return Result_t(fir::ConstantInt::getInt64(this->ival), 0);
	}
	else
	{
		return Result_t(fir::ConstantFP::getFloat64(this->dval), 0);
	}
}

Result_t BoolVal::codegen(CodegenInstance* cgi, fir::Value* extra)
{
	return Result_t(fir::ConstantInt::getBool(this->val), 0);
}

Result_t NullVal::codegen(CodegenInstance* cgi, fir::Value* extra)
{
	return Result_t(fir::ConstantValue::getNull(), 0);
}

Result_t StringLiteral::codegen(CodegenInstance* cgi, fir::Value* extra)
{
	auto pair = cgi->getTypeByString("String");
	if(pair && !this->isRaw)
	{
		fir::ClassType* stringType = dynamic_cast<fir::ClassType*>(pair->first);

		fir::Value* alloca = cgi->getStackAlloc(stringType);

		// String layout:
		// var data: Int8*
		// var allocated: Uint64


		fir::Value* stringPtr = cgi->builder.CreateStructGEP(alloca, 0);
		fir::Value* allocdPtr = cgi->builder.CreateStructGEP(alloca, 1);

		fir::Value* stringVal = cgi->module->createGlobalString(this->str);
		stringVal = cgi->builder.CreateConstGEP2(stringVal, 0, 0);

		cgi->builder.CreateStore(stringVal, stringPtr);
		cgi->builder.CreateStore(fir::ConstantInt::getUint64(0, cgi->getContext()), allocdPtr);

		fir::Value* val = cgi->builder.CreateLoad(alloca);
		alloca->makeImmutable();

		return Result_t(val, alloca);
	}
	else
	{
		// todo: dirty.
		static bool didWarn = false;

		if(!this->isRaw && !didWarn)
		{
			warn(this, "String type not available, using Int8* for string literal (will not warn again)");
			didWarn = true;
		}

		// good old Int8*
		fir::Value* stringVal = cgi->module->createGlobalString(this->str);
		stringVal = cgi->builder.CreateConstGEP2(stringVal, 0, 0);

		return Result_t(stringVal, 0);
	}
}


Result_t ArrayLiteral::codegen(CodegenInstance* cgi, fir::Value* extra)
{
	fir::Type* tp = 0;
	std::vector<fir::ConstantValue*> vals;

	if(this->values.size() == 0)
	{
		if(!extra)
		{
			error(this, "Unable to infer type for empty array");
		}

		tp = extra->getType()->getPointerElementType();
	}
	else
	{
		tp = cgi->getExprType(this->values.front());

		for(Expr* e : this->values)
		{
			fir::Value* v = e->codegen(cgi).result.first;
			if(dynamic_cast<fir::ConstantValue*>(v))
			{
				fir::ConstantValue* c = dynamic_cast<fir::ConstantValue*>(v);

				vals.push_back(c);
				if(vals.back()->getType() != tp)
				{
					error(e, "Array members must have the same type, got %s and %s",
						cgi->getReadableType(tp).c_str(), cgi->getReadableType(vals.back()->getType()).c_str());
				}
			}
			else
			{
				error(e, "Array literal members must be constant");
			}
		}
	}

	fir::ArrayType* atype = fir::ArrayType::get(tp, this->values.size());
	fir::Value* alloc = cgi->builder.CreateStackAlloc(atype);
	fir::Value* val = fir::ConstantArray::get(atype, vals);

	cgi->builder.CreateStore(val, alloc);
	return Result_t(val, alloc);
}














