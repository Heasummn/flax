// ControlFlowCodegen.cpp
// Copyright (c) 2014 - The Foreseeable Future, zhiayang@gmail.com
// Licensed under the Apache License Version 2.0.


#include "../include/ast.h"
#include "../include/codegen.h"
#include "../include/llvm_all.h"

using namespace Ast;
using namespace Codegen;



void codeGenRecursiveIf(llvm::Function* func, std::deque<std::pair<Expr*, Closure*>> pairs, llvm::BasicBlock* merge, llvm::PHINode* phi)
{
	if(pairs.size() == 0)
		return;

	llvm::BasicBlock* t = llvm::BasicBlock::Create(getContext(), "trueCaseR", func);
	llvm::BasicBlock* f = llvm::BasicBlock::Create(getContext(), "falseCaseR");

	llvm::Value* cond = pairs.front().first->codeGen();


	VarType apprType = determineVarType(pairs.front().first);
	if(apprType != VarType::Bool)
		cond = mainBuilder.CreateICmpNE(cond, llvm::ConstantInt::get(getContext(), llvm::APInt(pow(2, (int) apprType % 4) * 8, 0, apprType > VarType::Int64)), "ifCond");

	else
		cond = mainBuilder.CreateICmpNE(cond, llvm::ConstantInt::get(getContext(), llvm::APInt(1, false, true)));



	mainBuilder.CreateCondBr(cond, t, f);
	mainBuilder.SetInsertPoint(t);

	llvm::Value* val = nullptr;
	{
		pushScope();
		val = pairs.front().second->codeGen();
		popScope();
	}

	if(phi)
		phi->addIncoming(val, t);

	mainBuilder.CreateBr(merge);


	// now the false case...
	// set the insert point to the false case, then go again.
	mainBuilder.SetInsertPoint(f);

	// recursively call ourselves
	pairs.pop_front();
	codeGenRecursiveIf(func, pairs, merge, phi);

	// once that's done, we can add the false-case block to the func
	func->getBasicBlockList().push_back(f);
}

llvm::Value* If::codeGen()
{
	assert(this->cases.size() > 0);
	llvm::Value* firstCond = this->cases[0].first->codeGen();
	VarType apprType = determineVarType(this->cases[0].first);

	if(apprType != VarType::Bool)
		firstCond = mainBuilder.CreateICmpNE(firstCond, llvm::ConstantInt::get(getContext(), llvm::APInt(pow(2, (int) apprType % 4) * 8, 0, apprType > VarType::Int64)), "ifCond");

	else
		firstCond = mainBuilder.CreateICmpNE(firstCond, llvm::ConstantInt::get(getContext(), llvm::APInt(1, false, true)));


	llvm::Function* func = mainBuilder.GetInsertBlock()->getParent();
	llvm::BasicBlock* trueb = llvm::BasicBlock::Create(getContext(), "trueCase", func);
	llvm::BasicBlock* falseb = llvm::BasicBlock::Create(getContext(), "falseCase");
	llvm::BasicBlock* merge = llvm::BasicBlock::Create(getContext(), "merge");

	// create the first conditional
	mainBuilder.CreateCondBr(firstCond, trueb, falseb);



	// emit code for the first block
	llvm::Value* truev = nullptr;
	{
		mainBuilder.SetInsertPoint(trueb);

		// push a new symtab
		pushScope();
		truev = this->cases[0].second->codeGen();
		popScope();

		mainBuilder.CreateBr(merge);
	}



	// now for the clusterfuck.
	// to support if-elseif-elseif-elseif-...-else, we need to essentially compound/cascade conditionals in the 'else' block
	// of the if statement.

	mainBuilder.SetInsertPoint(falseb);

	auto c1 = this->cases.front();
	this->cases.pop_front();

	llvm::BasicBlock* curblk = mainBuilder.GetInsertBlock();
	mainBuilder.SetInsertPoint(merge);

	// llvm::PHINode* phi = mainBuilder.CreatePHI(llvm::Type::getVoidTy(getContext()), this->cases.size() + (this->final ? 1 : 0));

	llvm::PHINode* phi = nullptr;

	if(phi)
		phi->addIncoming(truev, trueb);

	mainBuilder.SetInsertPoint(curblk);
	codeGenRecursiveIf(func, std::deque<std::pair<Expr*, Closure*>>(this->cases), merge, phi);

	func->getBasicBlockList().push_back(falseb);

	// if we have an 'else' case
	if(this->final)
	{
		pushScope();
		llvm::Value* v = this->final->codeGen();
		popScope();

		if(phi)
			phi->addIncoming(v, falseb);
	}

	mainBuilder.CreateBr(merge);

	func->getBasicBlockList().push_back(merge);
	mainBuilder.SetInsertPoint(merge);

	return llvm::ConstantInt::get(getContext(), llvm::APInt(1, 0, true));
}
