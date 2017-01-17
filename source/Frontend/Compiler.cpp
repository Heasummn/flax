// Compiler.cpp
// Copyright (c) 2014 - 2015, zhiayang@gmail.com
// Licensed under the Apache License Version 2.0.

#include <iostream>
#include <fstream>
#include <cassert>
#include <fstream>
#include <cstdlib>
#include <cinttypes>

#include <locale>
#include <utility>
#include <codecvt>

#include <sys/stat.h>
#include "errors.h"
#include "parser.h"
#include "codegen.h"
#include "compiler.h"
#include "dependency.h"

#define __STDC_LIMIT_MACROS
#define __STDC_CONSTANT_MACROS

#include "llvm/IR/Verifier.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/MemoryBuffer.h"

// dirty AF.
#ifdef _WIN32
#include <Windows.h>

static bool _fileExists(LPCTSTR szPath)
{
	DWORD dwAttrib = GetFileAttributes(szPath);
	return (dwAttrib != INVALID_FILE_ATTRIBUTES && !(dwAttrib & FILE_ATTRIBUTE_DIRECTORY));
}



static std::string _getFullPathName(const char* _path)
{
	std::string path = _path;
	std::replace(path.begin(), path.end(), '/', '\\');

	char* out = new char[1024];
	GetFullPathName((TCHAR*) path.c_str(), 1024, (TCHAR*) out, 0);

	// apparently windows is fucked up, and this function always returns non-zero.
	// call something else to verify the full path.

	bool ret = _fileExists((TCHAR*) out);

	if(ret == false)
	{
		// fprintf(stderr, "file %s does not exist\n", path.c_str());
		delete[] out;
		return "";
	}
	else
	{
		// fprintf(stderr, "getFullPath for %s -- %s (%d) // %d\n", path.c_str(), out, GetLastError(), ret);

		std::string ret = out;
		delete[] out;

		return ret;
	}

}
#else
static std::string _getFullPathName(const char* path)
{
	auto ret = realpath(path, 0);
	if(ret == 0)
	{
		return "";
	}
	else
	{
		std::string str = ret;
		free(ret);

		return str;
	}
}
#endif




using namespace Ast;

namespace Compiler
{
	static HighlightOptions prettyErrorImport(Import* imp, std::string fpath)
	{
		HighlightOptions ops;
		ops.caret = imp->pin;
		ops.caret.file = fpath;

		// fprintf(stderr, "ops.caret.file = %s // %s\n", ops.caret.file.c_str(), fpath.c_str());

		auto tmp = imp->pin;
		tmp.col += std::string("import ").length();
		tmp.len = imp->module.length();

		ops.underlines.push_back(tmp);

		return ops;
	}



	std::string resolveImport(Import* imp, std::string fullPath)
	{
		std::string curpath = getPathFromFile(fullPath);

		if(imp->module.find("*") != (size_t) -1)
		{
			parserError(imp->pin, "Wildcard imports are currently not supported (trying to import %s)", imp->module.c_str());
		}

		// first check the current directory.
		std::string modname = imp->module;
		for(size_t i = 0; i < modname.length(); i++)
		{
			if(modname[i] == '.')
				modname[i] = '/';
		}
		
		std::string name = curpath + "/" + modname + ".flx";
		std::string fname = _getFullPathName(name.c_str());

		// a file here
		if(!fname.empty())
		{
			return fname;
		}
		else
		{
			std::string builtinlib = getSysroot() + getPrefix() + modname + ".flx";

			struct stat buffer;
			if(stat(builtinlib.c_str(), &buffer) == 0)
			{
				return getFullPathOfFile(builtinlib);;
			}
			else
			{
				std::string msg = "No module or library with the name '" + modname + "' could be found (no such builtin library either)";

				va_list ap;

				__error_gen(prettyErrorImport(imp, fullPath), msg.c_str(), "Error", true, ap);
				abort();
			}
		}
	}





	static void cloneCGIInnards(Codegen::CodegenInstance* from, Codegen::CodegenInstance* to)
	{
		to->typeMap					= from->typeMap;
		to->customOperatorMap		= from->customOperatorMap;
		to->customOperatorMapRev	= from->customOperatorMapRev;
		to->globalConstructors		= from->globalConstructors;
	}

	static void copyRootInnards(Codegen::CodegenInstance* cgi, Root* from, Root* to, bool doClone)
	{
		using namespace Codegen;


		// todo: deprecate this shit
		for(auto v : from->typeList)
		{
			bool skip = false;
			for(auto k : to->typeList)
			{
				if(std::get<0>(k) == std::get<0>(v))
				{
					skip = true;
					break;
				}
			}

			if(skip)
				continue;

			to->typeList.push_back(v);
		}


		if(doClone)
		{
			// cgi->cloneFunctionTree(from->rootFuncStack, to->rootFuncStack, false);
		}
	}

	static Codegen::CodegenInstance* _compileFile(std::string fpath, Codegen::CodegenInstance* rcgi, Root* dummyRoot)
	{
		auto p = prof::Profile("compileFile");

		using namespace Codegen;
		using namespace Parser;

		CodegenInstance* cgi = new CodegenInstance();
		cloneCGIInnards(rcgi, cgi);

		ParserState pstate(cgi);

		cgi->customOperatorMap = rcgi->customOperatorMap;
		cgi->customOperatorMapRev = rcgi->customOperatorMapRev;

		std::string curpath = Compiler::getPathFromFile(fpath);

		// parse
		// printf("*** start module %s\n", Compiler::getFilenameFromPath(fpath).c_str());
		Root* root = Parser::Parse(pstate, fpath);
		cgi->rootNode = root;


		// add the previous stuff to our own root
		copyRootInnards(cgi, dummyRoot, root, true);

		cgi->module = new fir::Module(Parser::getModuleName(fpath));
		cgi->importOtherCgi(rcgi);

		auto q = prof::Profile("codegen");
		Codegen::doCodegen(fpath, root, cgi);
		q.finish();

		// add the new stuff to the main root
		// todo: check for duplicates
		// copyRootInnards(rcgi, root, dummyRoot, true);

		rcgi->customOperatorMap = cgi->customOperatorMap;
		rcgi->customOperatorMapRev = cgi->customOperatorMapRev;

		return cgi;
	}


	static void _resolveImportGraph(Codegen::DependencyGraph* g, std::unordered_map<std::string, bool>& visited, std::string currentMod,
		std::string curpath)
	{
		using namespace Parser;

		// NOTE: make sure resolveImport **DOES NOT** use codegeninstance, cuz it's 0.
		ParserState fakeps(0);


		fakeps.currentPos.file = currentMod;


		fakeps.currentPos.line = 1;
		fakeps.currentPos.col = 1;
		fakeps.currentPos.len = 1;

		{
			auto p = prof::Profile("getFileTokens");
			fakeps.tokens = Compiler::getFileTokens(currentMod);
		}

		auto p = prof::Profile("find imports");
		while(fakeps.tokens.size() > 0)
		{
			Token t = fakeps.front();
			fakeps.pop();

			if(t.type == TType::Import)
			{
				// hack: parseImport expects front token to be "import"
				fakeps.tokens.push_front(t);

				Import* imp = parseImport(fakeps);

				std::string file = Compiler::getFullPathOfFile(Compiler::resolveImport(imp, Compiler::getFullPathOfFile(currentMod)));

				g->addModuleDependency(currentMod, file, imp);

				if(!visited[file])
				{
					visited[file] = true;
					_resolveImportGraph(g, visited, file, curpath);
				}
			}
		}
		p.finish();
	}

	static Codegen::DependencyGraph* resolveImportGraph(std::string baseFullPath, std::string curpath)
	{
		using namespace Codegen;
		DependencyGraph* g = new DependencyGraph();

		std::unordered_map<std::string, bool> visited;
		_resolveImportGraph(g, visited, baseFullPath, curpath);

		return g;
	}










	using namespace Codegen;
	std::vector<std::vector<DepNode*>> checkCyclicDependencies(std::string filename)
	{
		filename = getFullPathOfFile(filename);
		std::string curpath = getPathFromFile(filename);

		DependencyGraph* g = resolveImportGraph(filename, curpath);

		// size_t acc = 0;
		// for(auto e : g->edgesFrom)
		// 	acc += e.second.size();

		// printf("%zu edges in graph\n", acc);

		std::vector<std::vector<DepNode*>> groups = g->findCyclicDependencies();

		for(auto gr : groups)
		{
			if(gr.size() > 1)
			{
				std::string modlist;
				std::vector<Expr*> imps;

				for(auto m : gr)
				{
					std::string fn = getFilenameFromPath(m->name);
					fn = fn.substr(0, fn.find_last_of('.'));

					modlist += "    " + fn + "\n";
				}

				info("Cyclic import dependencies between these modules:\n%s", modlist.c_str());
				info("Offending import statements:");

				for(auto m : gr)
				{
					for(auto u : m->users)
					{
						va_list ap;

						__error_gen(prettyErrorImport(dynamic_cast<Import*>(u.second), u.first->name), "here", "Note", false, ap);
					}
				}

				error("Cyclic dependencies found, cannot continue");
			}
		}

		return groups;
	}



	CompiledData compileFile(std::string filename, std::vector<std::vector<DepNode*>> groups, std::map<Ast::ArithmeticOp,
		std::pair<std::string, int>> foundOps, std::map<std::string, Ast::ArithmeticOp> foundOpsRev)
	{
		filename = getFullPathOfFile(filename);

		std::unordered_map<std::string, Root*> rootmap;
		std::vector<std::pair<std::string, fir::Module*>> modulelist;


		Root* dummyRoot = new Root();
		CodegenInstance* rcgi = new CodegenInstance();
		rcgi->rootNode = dummyRoot;
		rcgi->module = new fir::Module("dummy");

		rcgi->customOperatorMap = foundOps;
		rcgi->customOperatorMapRev = foundOpsRev;

		// fprintf(stderr, "%zu groups (%zu)\n", groups.size(), g->nodes.size());

		if(groups.size() == 0)
		{
			DepNode* dn = new DepNode();
			dn->name = filename;
			groups.insert(groups.begin(), { dn });
		}

		for(auto gr : groups)
		{
			iceAssert(gr.size() == 1);
			std::string name = Compiler::getFullPathOfFile(gr.front()->name);

			auto cgi = _compileFile(name, rcgi, dummyRoot);
			rcgi->importOtherCgi(cgi);

			modulelist.push_back({ name, cgi->module });
			rootmap[name] = cgi->rootNode;

			delete cgi;
		}

		CompiledData ret;

		ret.rootNode = rootmap[Compiler::getFullPathOfFile(filename)];
		ret.rootMap = rootmap;
		ret.moduleList = modulelist;

		return ret;
	}


















































	std::string getPathFromFile(std::string path)
	{
		std::string ret;

		size_t sep = path.find_last_of("\\/");
		if(sep != std::string::npos)
			ret = path.substr(0, sep);

		return ret;
	}

	std::string getFilenameFromPath(std::string path)
	{
		std::string ret;

		size_t sep = path.find_last_of("\\/");
		if(sep != std::string::npos)
			ret = path.substr(sep + 1);

		return ret;
	}

	std::string getFullPathOfFile(std::string partial)
	{
		std::string fullpath = _getFullPathName(partial.c_str());
		if(fullpath.empty())
			error("Nonexistent file %s", partial.c_str());

		return fullpath;
	}
}





















