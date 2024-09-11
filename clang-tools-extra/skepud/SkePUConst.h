#pragma once

#include <string>
#include <sstream>
#include <fstream>
#include <functional>
#include <unordered_set>
#include <unordered_map>
#include <utility>

#include "clang/AST/AST.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTDiagnostic.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/ASTConsumers.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/CommandLine.h"

#include "clang/Lex/Lexer.h"
#include "clang/Basic/Diagnostic.h"
#include "skepu_data_structures.h"


extern llvm::cl::opt<bool> GenCUDA;
extern llvm::cl::opt<bool> GenOMP;
extern llvm::cl::opt<bool> GenCL;
extern llvm::cl::opt<bool> DoNotGenLineDirectives;

extern llvm::cl::opt<std::string> ResultName;
extern llvm::cl::opt<std::string> ResultDir;

extern llvm::cl::opt<bool> Verbose;

extern std::string inputFileName;

// User functions, name maps to AST entry and indexed indicator
extern std::unordered_map<const clang::FunctionDecl*, UserFunction*> UserFunctions;

// User functions, name maps to AST entry and indexed indicator
extern std::unordered_map<const clang::TypeDecl*, UserType*> UserTypes;

// User functions, name maps to AST entry and indexed indicator
extern std::unordered_map<const clang::VarDecl*, UserConstant*> UserConstants;

extern std::vector<SkeletonInvocation*> SkeletonInvocations;
extern std::vector<Skeleton*> Skeletons;

extern std::vector<SkepuContainerInvocation*> ContainerInvocations;
extern std::vector<SkepuContainer*> Containers;

extern const std::unordered_map<std::string, Skeleton> SkeletonsLookup;
extern std::unordered_map<Skeleton::Type, std::string> SkeletonDescriptions;

extern clang::Rewriter GlobalRewriter;

extern bool Debug;



//extern clang::SourceManager *SM;

const std::string SkePU_UF_Prefix {"skepu_userfunction_"};
extern size_t GlobalSkeletonIndex;

[[noreturn]] void SkePUAbort(std::string msg);
llvm::raw_ostream& SkePULog();

void replaceTextInString(std::string& text, const std::string &find, const std::string &replace);
std::string templateString(std::string templ, std::vector<std::pair<std::string, std::string>> replacements);
std::string transformToCXXIdentifier(std::string &in);
std::string getSourceAsString(clang::SourceRange range);


// Library markers
extern bool didFindBlas;
extern clang::SourceLocation blasBegin, blasEnd;

class SkepuMatcher
{
public:
    SkepuMatcher(llvm::StringRef File);    

    bool runMatcher();
    // User functions, name maps to AST entry and indexed indicator
    //std::unordered_map<const clang::FunctionDecl*, UserFunction*> _UserFunctions;

    // User functions, name maps to AST entry and indexed indicator
    //std::unordered_map<const clang::TypeDecl*, UserType*> _UserTypes;

    // User functions, name maps to AST entry and indexed indicator
    //std::unordered_map<const clang::VarDecl*, UserConstant*> _UserConstants;

     // Copy constructor

    std::vector<Skeleton*> _Skeletons;
    std::vector<UserFunction*> _Userfunctions;
    std::vector<UserType*> _UserTypes;
    std::vector<UserConstant*> _UserConstants;
    std::vector<SkepuContainer*> _Containers;
    std::vector<std::string> _SkeletonTypes;
    std::unordered_map<std::string, Skeleton> _SkeletonsStandard;

    std::unordered_map<std::string, std::string> _SkeletonDescriptions;

private:
    llvm::StringRef File;

public: 

    /*
    ~SkepuMatcher() {
        for (auto& skeleton : _Skeletons) {
            delete skeleton;
        }
        for (auto& userFunction : _Userfunctions) {
            delete userFunction;
        }
        for (auto& userType : _UserTypes) {
            delete userType;
        }
        for (auto& userConstant : _UserConstants) {
            delete userConstant;
        }
        for (auto& container : _Containers) {
            delete container;
        }
    }

    SkepuMatcher(const SkepuMatcher& other) 
        : _Skeletons(other._Skeletons),
          _Userfunctions(other._Userfunctions),
          _UserTypes(other._UserTypes),
          _UserConstants(other._UserConstants),
          _Containers(other._Containers),
          _SkeletonTypes(other._SkeletonTypes),
          _SkeletonDescriptions(other._SkeletonDescriptions),
          File(other.File) {}

    // Move constructor
    SkepuMatcher(SkepuMatcher&& other) noexcept
        : _Skeletons(std::move(other._Skeletons)),
          _Userfunctions(std::move(other._Userfunctions)),
          _UserTypes(std::move(other._UserTypes)),
          _UserConstants(std::move(other._UserConstants)),
          _Containers(std::move(other._Containers)),
          _SkeletonTypes(std::move(other._SkeletonTypes)),
          _SkeletonDescriptions(std::move(other._SkeletonDescriptions)),
          File(std::move(other.File)) {}

    // Copy assignment operator
    SkepuMatcher& operator=(const SkepuMatcher& other) {
        if (this != &other) {
            File = other.File;
            _Skeletons = other._Skeletons;
            _Userfunctions = other._Userfunctions;
            _UserTypes = other._UserTypes;
            _UserConstants = other._UserConstants;
            _Containers = other._Containers;
            _SkeletonTypes = other._SkeletonTypes;
            _SkeletonDescriptions = other._SkeletonDescriptions;
        }
        return *this;
    }

    // Move assignment operator
    SkepuMatcher& operator=(SkepuMatcher&& other) noexcept {
        if (this != &other) {
            File = std::move(other.File);
            _Skeletons = std::move(other._Skeletons);
            _Userfunctions = std::move(other._Userfunctions);
            _UserTypes = std::move(other._UserTypes);
            _UserConstants = std::move(other._UserConstants);
            _Containers = std::move(other._Containers);
            _SkeletonTypes = std::move(other._SkeletonTypes);
            _SkeletonDescriptions = std::move(other._SkeletonDescriptions);
        }
        return *this;
    }
    */
};