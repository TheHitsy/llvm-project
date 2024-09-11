#pragma once

#define SKEPUD_GLOBALS

#include "SkePUConst.h"
#include "skepu_data_structures.h"

#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/CommandLine.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

#include "support/Logger.h"
#include "Protocol.h"


using namespace clang;
using namespace clang::ast_matchers;
using namespace clang::tooling;

// ------------------------------
// AST visitor
// ------------------------------

UserFunction *HandleUserFunction(clang::FunctionDecl *f);
UserType *HandleUserType(const clang::CXXRecordDecl *t);
Skeleton* HandleSkeletonInstance(clang::VarDecl *d);
std::unordered_set<std::string>& getSkeletonStrings();


class SkePUASTVisitor : public clang::RecursiveASTVisitor<SkePUASTVisitor>
{
public:

	SkePUASTVisitor(clang::ASTContext *ctx, std::unordered_set<clang::VarDecl *> &instanceSet);

	bool VisitVarDecl(clang::VarDecl *d);
	bool VisitCXXOperatorCallExpr(clang::CXXOperatorCallExpr *c);
	bool VisitCallExpr(clang::CallExpr *ce);

	std::unordered_set<clang::VarDecl *> &SkeletonInstances;

private:
	clang::ASTContext *Context;
};

// Implementation of the ASTConsumer interface for reading an AST produced by the Clang parser.
class SkePUASTConsumer : public clang::ASTConsumer
{
public:

	SkePUASTConsumer(clang::ASTContext *ctx, std::unordered_set<clang::VarDecl *> &instanceSet);

	// Override the method that gets called for each parsed top-level declaration.
	bool HandleTopLevelDecl(clang::DeclGroupRef DR) override;

private:
	SkePUASTVisitor Visitor;

};
