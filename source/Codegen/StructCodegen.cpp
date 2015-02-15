// AggrTypeCodegen.cpp
// Copyright (c) 2014 - The Foreseeable Future, zhiayang@gmail.com
// Licensed under the Apache License Version 2.0.


#include "../include/ast.h"
#include "../include/codegen.h"
#include "../include/llvm_all.h"

using namespace Ast;
using namespace Codegen;


Result_t Struct::codegen(CodegenInstance* cgi, llvm::Value* lhsPtr)
{
	assert(this->didCreateType);
	TypePair_t* _type = cgi->getType(this->mangledName);
	if(!_type)
		GenError::unknownSymbol(this, this->name, SymbolType::Type);


	llvm::StructType* str = llvm::cast<llvm::StructType>(_type->first);
	llvm::Function* defaultInitFunc = 0;


	// generate initialiser
	{
		defaultInitFunc = llvm::Function::Create(llvm::FunctionType::get(llvm::Type::getVoidTy(llvm::getGlobalContext()), llvm::PointerType::get(str, 0), false), llvm::Function::ExternalLinkage, "__automatic_init#" + this->mangledName, cgi->mainModule);

		cgi->addFunctionToScope(defaultInitFunc->getName(), FuncPair_t(defaultInitFunc, 0));
		llvm::BasicBlock* iblock = llvm::BasicBlock::Create(llvm::getGlobalContext(), "initialiser", defaultInitFunc);
		cgi->mainBuilder.SetInsertPoint(iblock);

		// create the local instance of reference to self
		llvm::Value* self = &defaultInitFunc->getArgumentList().front();

		for(VarDecl* var : this->members)
		{
			int i = this->nameMap[var->name];
			assert(i >= 0);

			llvm::Value* ptr = cgi->mainBuilder.CreateStructGEP(self, i, "memberPtr_" + var->name);

			auto r = var->initVal ? var->initVal->codegen(cgi).result : ValPtr_t(0, 0);
			var->doInitialValue(cgi, cgi->getType(var->type), r.first, r.second, ptr);
		}


		// issue here is that functions aren't codegened (ie. don't have the llvm::Function*)
		// before their bodies are codegened, so this makes functions in structs order-dependent.

		// pass 1
		for(Func* f : this->funcs)
		{
			llvm::BasicBlock* ob = cgi->mainBuilder.GetInsertBlock();

			std::string oname = f->decl->name;
			bool isOpOverload = f->decl->name.find("operator#") == 0;
			if(isOpOverload)
				f->decl->name = f->decl->name.substr(strlen("operator#"));

			llvm::Value* val = nullptr;



			// hack:
			// 1. append 'E' to the end of the function's basename, as per C++ ABI
			// 2. remove the first varDecl of its parameters (self)
			// 3. mangle the name
			// 4. restore the parameter
			// this makes sure that we don't get ridiculous mangled names for member functions


			val = f->decl->codegen(cgi).result.first;

			if(f->decl->name == "init")
				this->initFuncs.push_back(llvm::cast<llvm::Function>(val));


			cgi->mainBuilder.SetInsertPoint(ob);
			this->lfuncs.push_back(llvm::cast<llvm::Function>(val));

			if(isOpOverload)
			{
				ArithmeticOp ao = cgi->determineArithmeticOp(f->decl->name);
				this->lOpOverloads.push_back(std::make_pair(ao, llvm::cast<llvm::Function>(val)));
			}

			// make the functions public as well
			cgi->rootNode->publicFuncs.push_back(std::pair<FuncDecl*, llvm::Function*>(f->decl, llvm::cast<llvm::Function>(val)));
		}




		// pass 2
		for(Func* f : this->funcs)
		{
			llvm::BasicBlock* ob = cgi->mainBuilder.GetInsertBlock();

			if(f->decl->name == "init")
			{
				std::deque<Expr*> todeque;

				VarRef* svr = new VarRef(this->posinfo, "self");
				todeque.push_back(svr);

				// add the call to auto init
				FuncCall* fc = new FuncCall(this->posinfo, "__automatic_init#" + this->mangledName, todeque);
				f->block->statements.push_front(fc);
			}

			f->codegen(cgi);
			cgi->mainBuilder.SetInsertPoint(ob);
		}




















		cgi->mainBuilder.CreateRetVoid();

		llvm::verifyFunction(*defaultInitFunc);
	}

	if(this->initFuncs.size() == 0)
		this->initFuncs.push_back(defaultInitFunc);


	// if we have an init function, then the __automatic_init will only be called from the local module
	// and only the normal init() needs to be exposed.
	cgi->rootNode->publicFuncs.push_back(std::pair<FuncDecl*, llvm::Function*>(0, defaultInitFunc));

	return Result_t(nullptr, nullptr);
}

void Struct::createType(CodegenInstance* cgi)
{
	if(cgi->isDuplicateType(this->name))
		GenError::duplicateSymbol(this, this->name, SymbolType::Type);

	llvm::Type** types = new llvm::Type*[this->funcs.size() + this->members.size()];

	// create a bodyless struct so we can use it
	this->mangledName = cgi->mangleWithNamespace(this->name);
	llvm::StructType* str = llvm::StructType::create(llvm::getGlobalContext(), this->mangledName);
	cgi->addNewType(str, this, ExprType::Struct);



	if(!this->didCreateType)
	{
		// because we can't (and don't want to) mangle names in the parser,
		// we could only build an incomplete name -> index map
		// finish it here.

		for(auto p : this->opOverloads)
			p->codegen(cgi);

		for(Func* func : this->funcs)
		{
			func->decl->parentStruct = this;

			std::string mangled = cgi->mangleName(func->decl->name, func->decl->params);
			if(this->nameMap.find(mangled) != this->nameMap.end())
				error(this, "Duplicate member '%s'", func->decl->name.c_str());
		}

		for(VarDecl* var : this->members)
		{
			llvm::Type* type = cgi->getLlvmType(var);
			if(type == str)
				error(this, "Cannot have non-pointer member of type self");

			types[this->nameMap[var->name]] = cgi->getLlvmType(var);
		}
	}


	std::vector<llvm::Type*> vec(types, types + this->members.size());
	str->setBody(vec, this->packed);

	this->didCreateType = true;
	cgi->getRootAST()->publicTypes.push_back(std::pair<Struct*, llvm::Type*>(this, str));
	this->scope = cgi->namespaceStack;

	delete types;
}










Result_t OpOverload::codegen(CodegenInstance* cgi, llvm::Value* lhsPtr)
{
	// this is never really called for actual codegen. operators are handled as functions,
	// so we just put them into the structs' funcs.
	// BinOp will do a lookup on the opMap, but never call codegen for this.

	// however, this will get called, because we need to know if the parameters for
	// the operator overload are legit. people ignore our return value.


	if(this->op == ArithmeticOp::Assign)
	{
		// make sure the first and only parameter is an 'other' pointer.
		FuncDecl* decl = this->func->decl;

		if(decl->params.size() != 1)
		{
			error("Operator overload for '=' can only have one argument (have %d)", decl->params.size());
		}

		// we can't actually do much, because they can assign to anything
	}
	else if(this->op == ArithmeticOp::CmpEq)
	{
		FuncDecl* decl = this->func->decl;
		if(decl->params.size() != 1)
			error("Operator overload for '==' can only have one argument");

		if(decl->type != "Bool")
			error("Operator overload for '==' must return a boolean value");

		// llvm::Type* ptype = cgi->getLlvmType(decl->params.front());
		// assert(ptype);

		// llvm::Type* stype = cgi->getType(this->str->name)->first;

		// if(ptype->getPointerTo() == stype->getPointerTo())
		// 	error(this, "Argument of overload operators (usually 'other') must be a pointer.");

		// if(ptype != stype->getPointerTo())
		// 	error(this, "Type mismatch [%s, %s]", cgi->getReadableType(ptype).c_str(), cgi->getReadableType(stype).c_str());
	}
	else
	{
		error("(%s:%d) -> Internal check failed: invalid operator", __FILE__, __LINE__);
	}

	return Result_t(0, 0);
}














