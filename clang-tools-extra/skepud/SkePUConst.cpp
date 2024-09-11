#include "SkePUConst.h"
#include "support/Logger.h"
#include "Protocol.h"
#include <filesystem>
#include <stdexcept>
#include "SkepuVisitor.h"
#include "clang/AST/Type.h"

llvm::cl::OptionCategory SkePUCategory("SkePU precompiler options");

llvm::cl::opt<std::string> ResultDir("dir", llvm::cl::desc("Directory of output files"), llvm::cl::cat(SkePUCategory));
llvm::cl::opt<std::string> ResultName("name", llvm::cl::desc("File name of main output file (without extension, e.g., .cpp or .cu)"), llvm::cl::cat(SkePUCategory));

llvm::cl::opt<bool> GenCUDA("cuda",  llvm::cl::desc("Generate CUDA backend"),   llvm::cl::cat(SkePUCategory));
llvm::cl::opt<bool> GenOMP("openmp", llvm::cl::desc("Generate OpenMP backend"), llvm::cl::cat(SkePUCategory));
llvm::cl::opt<bool> GenCL("opencl",  llvm::cl::desc("Generate OpenCL backend"), llvm::cl::cat(SkePUCategory));
llvm::cl::opt<bool> GenStarPUMPI("starpu-mpi",  llvm::cl::desc("Generate StarPU-MPI backend"), llvm::cl::cat(SkePUCategory));

llvm::cl::opt<bool> Verbose("verbose",  llvm::cl::desc("Verbose logging printout"), llvm::cl::cat(SkePUCategory));
llvm::cl::opt<bool> Silent("silent",  llvm::cl::desc("Disable normal printouts"), llvm::cl::cat(SkePUCategory));
llvm::cl::opt<bool> NoAddExtension("override-extension",  llvm::cl::desc("Do not automatically add file extension to output file (good for headers)"), llvm::cl::cat(SkePUCategory));

llvm::cl::opt<bool> DoNotGenLineDirectives("no-preserve-lines", llvm::cl::desc("Do not try to preserve line numbers from source file"),   llvm::cl::cat(SkePUCategory));

llvm::cl::opt<std::string> AllowedFuncNames("fnames", llvm::cl::desc("Function names which are allowed to be called from user functions (separated by space, e.g. -fnames \"conj csqrt\")"), llvm::cl::cat(SkePUCategory));


// Derived
static std::string mainFileName;
std::string inputFileName;


// User functions, name maps to AST entry and indexed indicator
std::unordered_map<const FunctionDecl*, UserFunction*> UserFunctions;

// User functions, name maps to AST entry and indexed indicator
std::unordered_map<const TypeDecl*, UserType*> UserTypes;

// User functions, name maps to AST entry and indexed indicator
std::unordered_map<const VarDecl*, UserConstant*> UserConstants;

std::vector<Skeleton*> Skeletons;
std::vector<SkeletonInvocation*> SkeletonInvocations;

std::vector<SkepuContainerInvocation*> ContainerInvocations;
std::vector<SkepuContainer*> Containers;


bool Debug = false;



void clearGlobals()
{
  if(!UserFunctions.empty())
  {
    for (auto& pair : UserFunctions) {
        delete pair.second;
    }
    UserFunctions.clear();
  }

  if(!UserTypes.empty())
  {
    for (auto& pair : UserTypes) {
        delete pair.second;
    }
    UserTypes.clear();
  }

  if(!UserConstants.empty())
  {
    for (auto& pair : UserConstants) {
        delete pair.second;
    }
    UserConstants.clear();
  }

  if(!SkeletonInvocations.empty())
  {
    for (auto& pair : SkeletonInvocations) {
        delete pair;
    }
    SkeletonInvocations.clear();
  }

  if(!Skeletons.empty())
  {
    for (auto& pair : Skeletons) {
        delete pair;
    }
    Skeletons.clear();
  }

  if(!ContainerInvocations.empty())
  {
    for (auto& pair : ContainerInvocations) {
        delete pair;
    }
    ContainerInvocations.clear();
  }
  
  if(!Containers.empty())
  {
    for (auto& pair : Containers) {
        delete pair;
    }
    Containers.clear();
  }

}

// Explicitly allowed functions to call from user functions TAKEN FROM SOURCE 
std::unordered_set<std::string> AllowedFunctionNamesCalledInUFs
{
	"exp", "exp2", "exp2f",
	"sqrt",
	"abs", "fabs",
	"max", "fmax",
	"pow",
	"log", "log2", "log10",
	"sin", "sinh", "asin", "asinh",
	"cos", "cosh", "acos", "acosh",
	"tan", "tanh", "atan", "atanh",
	"round", "ceil", "floor",
	"erf",
	"printf",
};

// Skeleton types lookup from internal SkePU class template name TAKEN FROM SOURCE
const std::unordered_map<std::string, Skeleton> SkeletonsLookup =
{
	{"MapImpl",              {"Map",                Skeleton::Type::Map,                1, 1}},
	{"Reduce1D",             {"Reduce1D",           Skeleton::Type::Reduce1D,           1, 1}},
	{"Reduce2D",             {"Reduce2D",           Skeleton::Type::Reduce2D,           2, 2}},
	{"MapReduceImpl",        {"MapReduce",          Skeleton::Type::MapReduce,          2, 2}},
	{"ScanImpl",             {"Scan",               Skeleton::Type::Scan,               1, 3}},
	{"MapOverlap1D",         {"MapOverlap1D",       Skeleton::Type::MapOverlap1D,       1, 4}},
	{"MapOverlap2D",         {"MapOverlap2D",       Skeleton::Type::MapOverlap2D,       1, 1}},
	{"MapOverlap3D",         {"MapOverlap3D",       Skeleton::Type::MapOverlap3D,       1, 1}},
	{"MapOverlap4D",         {"MapOverlap4D",       Skeleton::Type::MapOverlap4D,       1, 1}},
	{"MapPairsImpl",         {"MapPairs",           Skeleton::Type::MapPairs,           1, 1}},
	{"MapPairsReduceImpl",   {"MapPairsReduce",     Skeleton::Type::MapPairsReduce,     2, 1}},
	{"CallImpl",             {"Call",               Skeleton::Type::Call,               1, 1}},
};

std::unordered_map<Skeleton::Type, std::string> SkeletonDescriptions =
{
	{Skeleton::Type::Map, 
  "Data-parallel element-wise application of a function with arbitrary arity."},
	{Skeleton::Type::Reduce1D, 
  "Reduction with 1D and 2D variations."},
	{Skeleton::Type::Reduce2D, 
  "Reduction with 1D and 2D variations."},
	{Skeleton::Type::MapReduce, 
  "Efficient chaining of Map and Reduce."},
	{Skeleton::Type::Scan, 
  "Generalized prefix sum."},
	{Skeleton::Type::MapOverlap1D, 
  "Stencil operation in 1D, 2D, 3D, and 4D."},
	{Skeleton::Type::MapOverlap2D, 
  "Stencil operation in 1D, 2D, 3D, and 4D."},
	{Skeleton::Type::MapOverlap3D, 
  "Stencil operation in 1D, 2D, 3D, and 4D."},
	{Skeleton::Type::MapOverlap4D, 
  "Stencil operation in 1D, 2D, 3D, and 4D."},
	{Skeleton::Type::MapPairs, 
  "Cartesian product-style computation."},
	{Skeleton::Type::MapPairsReduce, 
  "Efficient chaining of MapPairs and Reduce."},
	{Skeleton::Type::Call, 
  "Generic multi-variant component."},
};



Rewriter GlobalRewriter;
size_t GlobalSkeletonIndex = 0;


// Library markers
bool didFindBlas = false;
clang::SourceLocation blasBegin, blasEnd;

// For each source file provided to the tool, a new FrontendAction is created.
class SkePUFrontendAction : public ASTFrontendAction
{
public:

	bool BeginSourceFileAction(CompilerInstance &CI) override
	{
		inputFileName = this->getCurrentFile().str();
    if(Debug)
      clangd::log("BeginSourceFileAction for {0}", inputFileName);
		return true;
	}

  void EndSourceFileAction() override
	{		
    if(Debug)
		  clangd::log("######## SkePUFrontendAction:EndSourceFileAction ");
          
    for (VarDecl *d : this->SkeletonInstances){
			Skeleton *skeleton = HandleSkeletonInstance(d);
      if(skeleton != nullptr)
      {
        Skeletons.push_back(skeleton);
      }
		}
    for(SkeletonInvocation *Invocation: SkeletonInvocations)
    {
      for(Skeleton *skeleton: Skeletons)
      {
        if(skeleton == nullptr || Invocation == nullptr)
        {
          continue;
        }

        // TODO: Check if some more data points need to be compared such as special names and such
        if(Invocation->retType.isNull())
        {
          //clangd::log("Invocation ret is null");
        }

        if(skeleton->retType.isNull())
        {
          //clangd::log("Skeleton ret is null");
        }
      
        if(skeleton->variableName == Invocation->variableName && skeleton->retType == Invocation->retType)
        {
          skeleton->invocations.push_back(Invocation);
          skeleton->printVariables();
        }
      }
    }
	}


	std::unique_ptr<ASTConsumer> CreateASTConsumer(CompilerInstance &CI, StringRef file) override
	{
    if(Debug)
      clangd::log("Creating AST consumer for {0}", file.str());
    
		GlobalRewriter.setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
		return std::make_unique<SkePUASTConsumer>(&CI.getASTContext(), this->SkeletonInstances);
	}

  const std::unordered_set<clang::VarDecl *> &getResults() const{
    return SkeletonInstances;
  }

private:
	std::unordered_set<clang::VarDecl *> SkeletonInstances;
};

SkepuMatcher::SkepuMatcher(llvm::StringRef File)
: File(File)
{
  if(Debug)
    clangd::log("SkepuMatcher Initilized");
}

bool SkepuMatcher::runMatcher()
{
   std::filesystem::path p(File.str());
  std::string fName = p.filename();

  // Temp for now since skepu lies outside of examples folder
  std::string tmpPathSkepuEx = "/home/marcus/git_folder/skepu/examples";
 	std::string headerPath = tmpPathSkepuEx + "/../llvm/clang/lib/Headers";
  std::string skepuSrcPath = tmpPathSkepuEx + "/../skepu-headers/src";
  /*
  std::string headerPath = p.parent_path().string() + "../llvm/clang/lib/Headers";
  std::string skepuSrcPath = p.parent_path().string() + "../skepu-headers/src";
  */

  std::string filePath = p.parent_path().string() +"/"+ fName;
  std::string seperator = "--";
  std::string cXX = "-std=c++11";
  std::string expansion = "-Wno-expansion-to-defined";
  std::string include = "-I ";
  std::string argIncludeSkepu = "-include" + skepuSrcPath + "/skepu";
  std::string argSkepu ="-I/home/marcus/git_folder/skepu/skepu-headers/src/";

  const char* argv[] = {"",
  filePath.c_str(),
  seperator.c_str(),
  cXX.c_str(),
  expansion.c_str(),
  include.c_str(),
  headerPath.c_str(),
  argIncludeSkepu.c_str(),
  argSkepu.c_str(),
  nullptr};

  int argc = sizeof(argv) / sizeof(argv[0]) -1;

  if(Debug)
  {
    for (int i = 1; i < argc; ++i) {
          clangd::log("Argument {0}, : {1}", i, argv[i]);
    }
  }
  
  auto ExpectedParser = CommonOptionsParser::create(argc, argv, SkePUCategory);
  if (!ExpectedParser) {
    // Fail gracefully for unsupported options.
    if(Debug)
    {
      clangd::log("SkepuMatcherInit Error: The CommonOptionsParser has failed");
    }
  //  llvm::errs() << ExpectedParser.takeError();
    return false;
  }

  CommonOptionsParser& OptionsParser = ExpectedParser.get();

  ClangTool Tool(OptionsParser.getCompilations(),
                 OptionsParser.getSourcePathList());



  clearGlobals();


  Tool.run(tooling::newFrontendActionFactory<SkePUFrontendAction>().get());

  this->_Skeletons = std::move(Skeletons);
  this->_Containers = std::move(Containers);

  /*
  this->_SkeletonsStandard =
  {
    {"Map", {"Map",                Skeleton::Type::Map,                1, 1}},
    {"Reduce1D", {"Reduce1D",           Skeleton::Type::Reduce1D,           1, 1}},
    {"Reduce2D", {"Reduce2D",           Skeleton::Type::Reduce2D,           2, 2}},
    {"MapReduce", {"MapReduce",          Skeleton::Type::MapReduce,          2, 2}},
    {"Scan", {"Scan",               Skeleton::Type::Scan,               1, 3}},
    {"MapOverlap1D", {"MapOverlap1D",       Skeleton::Type::MapOverlap1D,       1, 4}},
    {"MapOverlap2D", {"MapOverlap2D",       Skeleton::Type::MapOverlap2D,       1, 1}},
    {"MapOverlap3D", {"MapOverlap3D",       Skeleton::Type::MapOverlap3D,       1, 1}},
    {"MapOverlap4D", {"MapOverlap4D",       Skeleton::Type::MapOverlap4D,       1, 1}},
    {"MapPairs", {"MapPairs",           Skeleton::Type::MapPairs,           1, 1}},
    {"MapPairsReduce", {"MapPairsReduce",     Skeleton::Type::MapPairsReduce,     2, 1}},
    {"Call", {"Call",               Skeleton::Type::Call,               1, 1}}
  };
  */
  
  for (const auto& pair : SkeletonsLookup) {
      // Assuming Skeleton has a proper copy constructor
      this->_SkeletonsStandard.insert(std::make_pair(pair.first, pair.second));
  }

  for(auto&& p: SkeletonsLookup )
  {
    this->_SkeletonDescriptions[p.second.name] = SkeletonDescriptions[p.second.type];
    this->_SkeletonTypes.push_back(p.second.name);
  }

  for(auto& skeleton : this->_Skeletons)
  {
    skeleton->description = "Skeleton of " + skeleton->name + ": " + SkeletonDescriptions.find(skeleton->type)->second;
    //skeleton->printVariables();
  }

  for(auto par : UserFunctions)
  {
    _Userfunctions.push_back(std::move(par.second));
  }

  for(auto par : UserConstants)
  {
    _UserConstants.push_back(std::move(par.second));
  }

   for(auto par : UserTypes)
  {
    _UserTypes.push_back(std::move(par.second));
  }

  if(Debug)
    clangd::log("End of SkepuMatcher tool");
  return true;
}
