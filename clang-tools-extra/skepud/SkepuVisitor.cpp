#include "SkepuVisitor.h"

#include "SkePUConst.h"
#include "skepu_data_structures.h"

std::unordered_set<std::string> SkeletonInstancesString;

[[noreturn]] void SkePUAbort(std::string msg)
{
	llvm::errs() << "[SKEPU] INTERNAL FATAL ERROR: " << msg << "\n";
	exit(1);
}

std::unordered_set<std::string>& getSkeletonStrings(){
    return SkeletonInstancesString;
}

// ------------------------------
// AST visitor
// ------------------------------

UserFunction *HandleUserFunction(FunctionDecl *f)
{
	//clangd::log("Userfunction: {0}, {1}", UserFunctions.find(f) != UserFunctions.end(),
	//f->getNameAsString());
	// Check so that this userfunction has not been treated already
	if (UserFunctions.find(f) != UserFunctions.end())
	{
		return UserFunctions[f];
	}

	UserFunction *UF = new UserFunction(f);
	UserFunctions[f] = UF;

	return UF;
}


UserFunction *HandleFunctionPointerArg(Expr *ArgExpr)
{
	// Check that the argument is a declaration reference
	if (!(DeclRefExpr::classof(ArgExpr)))
		SkePUAbort("Userfunction argument not a DeclRefExpr");

	// Get the referee delcaration of the argument declaration reference
	ValueDecl *ValDecl = dyn_cast<DeclRefExpr>(ArgExpr)->getDecl();

	// Check that the argument refers to a function declaration
	if (!FunctionDecl::classof(ValDecl))
		SkePUAbort("Userfunction argument not referencing a function declaration");

	// Get the function declaration and function name
	FunctionDecl *UserFunc = dyn_cast<FunctionDecl>(ValDecl);
	std::string FuncName = UserFunc->getNameInfo().getName().getAsString();

	return HandleUserFunction(UserFunc);
}

UserFunction *HandleLambdaArg(Expr *ArgExpr, VarDecl *d)
{
	CXXConstructExpr *ConstrExpr = dyn_cast<CXXConstructExpr>(ArgExpr);
	if (!ConstrExpr)
		SkePUAbort("User function (assumed lambda) argument not a construct expr");

	Expr *Arg = ConstrExpr->getArg(0);

	if (MaterializeTemporaryExpr *MatTempExpr = dyn_cast<MaterializeTemporaryExpr>(Arg))
		Arg = MatTempExpr->getSubExpr(); // changed from GetTemporaryExpr

	LambdaExpr *Lambda = dyn_cast<LambdaExpr>(Arg);
	if (!Lambda)
		SkePUAbort("User function (assumed lambda) argument not a lambda");

	if (Lambda->capture_size() > 0)
		SkePUAbort("User function lambda argument has non-empty capture list");

	CXXMethodDecl *CallOperator = Lambda->getCallOperator();

	UserFunction *UF = new UserFunction(CallOperator, d);
	UserFunctions[CallOperator] = UF;

	return UF;
}


const Skeleton::Type* DeclIsValidSkeleton(VarDecl *d)
{
	if (isa<ParmVarDecl>(d))
		return nullptr;

	if (d->isThisDeclarationADefinition() != VarDecl::DefinitionKind::Definition)
		return nullptr;

	Expr *InitExpr = d->getInit();
	if (!InitExpr)
		return nullptr;

	if (auto *CleanUpExpr = dyn_cast<ExprWithCleanups>(InitExpr))
		InitExpr = CleanUpExpr->getSubExpr();

	auto *ConstructExpr = dyn_cast<CXXConstructExpr>(InitExpr);
	if (!ConstructExpr || ConstructExpr->getConstructionKind() != CXXConstructExpr::ConstructionKind::CK_Complete)
		return nullptr;

	if (ConstructExpr->getNumArgs() == 0)
		return nullptr;

	auto *TempExpr = ConstructExpr->getArgs()[0];

	if (auto *MatTempExpr = dyn_cast<MaterializeTemporaryExpr>(TempExpr))
		TempExpr = MatTempExpr->getSubExpr(); // changed from GetTemporaryExpr

	if (auto *BindTempExpr = dyn_cast<CXXBindTemporaryExpr>(TempExpr))
		TempExpr = BindTempExpr->getSubExpr();

	CallExpr *CExpr = dyn_cast<CallExpr>(TempExpr);
	if (!CExpr)
		return nullptr;

	const FunctionDecl *Callee = CExpr->getDirectCallee();
	const Type *RetType = Callee->getReturnType().getTypePtr();

	if (isa<DecltypeType>(RetType))
		RetType = dyn_cast<DecltypeType>(RetType)->getUnderlyingType().getTypePtr();

	if (auto *ElabType = dyn_cast<ElaboratedType>(RetType))
		RetType = ElabType->getNamedType().getTypePtr();

	if (!isa<TemplateSpecializationType>(RetType))
		return nullptr;

	const TemplateDecl *Template = RetType->getAs<TemplateSpecializationType>()->getTemplateName().getAsTemplateDecl();
	std::string TypeName = Template->getNameAsString();

	if (SkeletonsLookup.find(TypeName) == SkeletonsLookup.end())
		return nullptr;

	return &SkeletonsLookup.at(TypeName).type;
}

QualType findReturnType(const QualType &QT) {
    if (const auto *FunctionType = QT->getAs<FunctionProtoType>()) {
        // If it's a function type, return its return type
        return FunctionType->getReturnType();
    } else if (const auto *DeclType = QT->getAs<DecltypeType>()) {
        // If it's a decltype, recursively check its underlying expression
        return findReturnType(DeclType->getUnderlyingType());
    } else {
        // In other cases, return an invalid type
        return QualType();
    }
}

Skeleton* HandleSkeletonInstance(VarDecl *d)
{
	if (d->isThisDeclarationADefinition() != VarDecl::DefinitionKind::Definition)
		SkePUAbort("Not a definition");

	Expr *InitExpr = d->getInit();

	if (isa<ExprWithCleanups>(InitExpr))
		InitExpr = dyn_cast<ExprWithCleanups>(InitExpr)->getSubExpr();

	CXXConstructExpr *ConstructExpr = dyn_cast<CXXConstructExpr>(InitExpr);
	if (!ConstructExpr || ConstructExpr->getConstructionKind() != CXXConstructExpr::ConstructionKind::CK_Complete)
		SkePUAbort("Not a complete constructor");

	Expr *TempExpr = dyn_cast<MaterializeTemporaryExpr>(ConstructExpr->getArgs()[0])->getSubExpr(); // changed from GetTemporaryExpr

	 if (isa<CXXBindTemporaryExpr>(TempExpr))
		TempExpr = dyn_cast<CXXBindTemporaryExpr>(TempExpr)->getSubExpr();

	CallExpr *CExpr = dyn_cast<CallExpr>(TempExpr);
	if (!CExpr)
		SkePUAbort("Not a call expression");


	const FunctionDecl *Callee = CExpr->getDirectCallee();
	const Type *RetType = Callee->getReturnType().getTypePtr();

	if (isa<DecltypeType>(RetType))
		RetType = dyn_cast<DecltypeType>(RetType)->getUnderlyingType().getTypePtr();

	const TemplateSpecializationType *Template = RetType->getAs<TemplateSpecializationType>();
	std::string TypeName = Template->getTemplateName().getAsTemplateDecl()->getNameAsString();
	Skeleton::Type skeletonType = SkeletonsLookup.at(TypeName).type;
	
	std::string InstanceName = d->getNameAsString();
	SkeletonInstancesString.insert(InstanceName);

	std::vector<size_t> arity = { 0, 2 };
	switch (skeletonType)
	{
	case Skeleton::Type::Map:
	case Skeleton::Type::MapReduce:
		assert(Template->getNumArgs() > 0);
		arity[0] = Template->template_arguments()[0].getAsExpr()->EvaluateKnownConstInt(d->getASTContext()).getExtValue();
		break;
	case Skeleton::Type::MapPairs:
	case Skeleton::Type::MapPairsReduce:
		assert(Template->getNumArgs() > 1);
		arity[0] = Template->template_arguments()[0].getAsExpr()->EvaluateKnownConstInt(d->getASTContext()).getExtValue();
		arity[1] = Template->template_arguments()[1].getAsExpr()->EvaluateKnownConstInt(d->getASTContext()).getExtValue();
		break;
	case Skeleton::Type::MapOverlap1D:
		arity[0] = 1; break;
	case Skeleton::Type::MapOverlap2D:
		arity[0] = 1; break;
	case Skeleton::Type::MapOverlap3D:
		arity[0] = 1; break;
	case Skeleton::Type::MapOverlap4D:
		arity[0] = 1; break;
	default:
		break;
	}
	
	std::vector<UserFunction*> FuncArgs;
	size_t i = 0;
	for (Expr *expr : CExpr->arguments())
	{
		UserFunction *UF;
		
		// The argument may be an implcit cast, get the underlying expression
		if (ImplicitCastExpr *ImplExpr = dyn_cast<ImplicitCastExpr>(expr))
			UF = HandleFunctionPointerArg(ImplExpr->IgnoreImpCasts());
		
		// It can also be an explicit cast, get the underlying expression
		else if (UnaryOperator *UnaryCastExpr = dyn_cast<UnaryOperator>(expr))
			UF = HandleFunctionPointerArg(UnaryCastExpr->getSubExpr());
		
		// The user function is probably defined as a lambda
		else
			UF = HandleLambdaArg(expr, d);
		
		FuncArgs.push_back(UF);
		
		if (skeletonType == Skeleton::Type::MapPairs || skeletonType == Skeleton::Type::MapPairsReduce)
			UF->updateArgLists(arity[0], arity[1]);
		else
			UF->updateArgLists(arity[i++]);
	}


	Skeleton *skeleton = new Skeleton{};
	skeleton->type = SkeletonsLookup.at(TypeName).type;
	skeleton->name = SkeletonsLookup.at(TypeName).name;
	skeleton->userfunctionArgAmount = SkeletonsLookup.at(TypeName).userfunctionArgAmount;
	skeleton->deviceKernelAmount = SkeletonsLookup.at(TypeName).deviceKernelAmount;

	skeleton->variableName = d->getNameAsString();
	skeleton->parameters = FuncArgs;
	skeleton->retType = Callee->getReturnType();

	SourceManager &SM = Callee->getASTContext().getSourceManager();
	SourceRange range = d->getSourceRange();

 	clangd::Range clangdRange;
	clangdRange.start.line = SM.getSpellingLineNumber(range.getBegin()) - 1;
	clangdRange.start.character = SM.getSpellingColumnNumber(range.getBegin());
	clangdRange.end.line = SM.getSpellingLineNumber(range.getEnd()) - 1;
	clangdRange.end.character = SM.getSpellingColumnNumber(range.getEnd());

	skeleton->selectionRange = clangdRange;
	clangdRange.start.character = 0;
	clangdRange.end.character = SM.getSpellingColumnNumber(range.getEnd()) + 1;
	skeleton->range = clangdRange;

	skeleton->location = d->getLocation();
	skeleton->locationString = d->getLocation().printToString(d->getASTContext().getSourceManager());


	skeleton->constructLayout();
	
    return skeleton;
}

bool DeclIsValidContainer(VarDecl *d)
{
	if (isa<ParmVarDecl>(d))
		return false;

	Expr *InitExpr = d->getInit();
	if (!InitExpr)
		return false;

	QualType varType = d->getType();

	auto *type = varType.getTypePtr();

	if (auto *innertype = dyn_cast<ElaboratedType>(type))
		type = innertype->getNamedType().getTypePtr();


	if(const auto *templateType = dyn_cast< TemplateSpecializationType>(type))
	{
		std::string templateName = templateType->getTemplateName().getAsTemplateDecl()->getNameAsString();

		SourceManager &sourceManager = d->getASTContext().getSourceManager();
		SourceLocation loc = d->getLocation();
		FileID mainFileID = sourceManager.getMainFileID();
		FileID varFileID = sourceManager.getFileID(loc);
		
		if(mainFileID != varFileID)
		{
			return false;
		}

		const CXXRecordDecl *recordDecl = type->getAsCXXRecordDecl();
        if (recordDecl) {
             for (const DeclContext *dc = recordDecl->getEnclosingNamespaceContext();
                 dc && dc->isNamespace(); dc = dc->getParent()) {
                const NamespaceDecl *nsDecl = cast<NamespaceDecl>(dc);
                if (nsDecl->getName() == "skepu") {
					SkepuContainer* sc = new SkepuContainer{};
					sc->type = templateName;
					sc->typenameType = templateType->template_arguments()[0].getAsType().getAsString();
					sc->variableName = d->getNameAsString();	

					SourceManager &SM = d->getASTContext().getSourceManager();
					SourceRange range = d->getSourceRange();

					clangd::Range clangdRange;
					clangdRange.start.line = SM.getSpellingLineNumber(range.getBegin()) - 1;
					clangdRange.start.character = SM.getSpellingColumnNumber(range.getBegin());
					clangdRange.end.line = SM.getSpellingLineNumber(range.getEnd()) - 1;
					clangdRange.end.character = SM.getSpellingColumnNumber(range.getEnd());

					sc->selectionRange = clangdRange;
					clangdRange.start.character = 0;
					clangdRange.end.character = SM.getSpellingColumnNumber(range.getEnd()) + 1;
					sc->range = clangdRange;

					sc->locationString = d->getLocation().printToString(d->getASTContext().getSourceManager());
					
					Containers.push_back(sc);
                    return true;
                }
            }
		
        }
	}
	
	return false;
}




// Returns nullptr if the user type can be ignored
UserType *HandleUserType(const CXXRecordDecl *t)
{
	if (!t)
		return nullptr;

	std::string name = t->getNameAsString();

	// These types are handled separately
	if (name == "Index1D" || name == "Index2D" || name == "Index3D" || name == "Index4D"
		|| name == "Region1D" || name == "Region2D" || name == "Region3D" || name == "Region4D"
		|| name == "Vec" || name == "Mat" || name == "Ten3" || name == "Ten4" || name == "MatRow" || name == "MatCol"
		|| name == "complex")
		return nullptr;

	// Check if already handled, otherwise construct and add
	if (UserTypes.find(t) == UserTypes.end())
		UserTypes[t] = new UserType(t);

	return UserTypes[t];
}


SkePUASTVisitor::SkePUASTVisitor(ASTContext *ctx, std::unordered_set<clang::VarDecl *> &instanceSet)
: SkeletonInstances(instanceSet), Context(ctx)
{}

bool SkePUASTVisitor::VisitVarDecl(VarDecl *d)
{	
	/*
	std::string typeName = d->getType().getAsString();
	if (typeName.find("skepu::PrecompilerMarker") != std::string::npos)
	{
		std::string varName = d->getNameAsString();
		if (varName == "startOfBlasHPP")
		{
			blasBegin = d->getSourceRange().getEnd();
			didFindBlas = true;
		}
		else if (varName == "endOfBlasHPP")
			blasEnd = d->getSourceRange().getEnd();
	}
	*/


	if (DeclIsValidSkeleton(d))
	{
		SkeletonInstances.insert(d);
	}
	else if(DeclIsValidContainer(d))
	{
		//clangd::log("########################## found skepu container");
	}
	else if (d->hasAttr<SkepuUserConstantAttr>())
	{
		UserConstants[d] = new UserConstant(d);
	}

	return RecursiveASTVisitor<SkePUASTVisitor>::VisitVarDecl(d);
}


bool SkePUASTVisitor::VisitCallExpr(clang::CallExpr *ce){
	SourceLocation startLoc = ce->getBeginLoc();
	SourceManager &sourceManager = Context->getSourceManager();
	if (sourceManager.isMacroBodyExpansion(startLoc)) {
		// If it's a macro expansion, get the spelling location
		startLoc = sourceManager.getSpellingLoc(startLoc);
	}

	if (FunctionDecl *FD = ce->getDirectCallee()) {
        // Get variable that was called
		if (ce->getNumArgs() <= 0) {
			return true;
		}
		Expr *argExpr = ce->getArg(0);
		if (DeclRefExpr *DRE = dyn_cast<DeclRefExpr>(argExpr)) {
			if (VarDecl *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
				if(DeclIsValidSkeleton(VD))
				{					
					SkeletonInvocation *skeletonInvocation = new SkeletonInvocation{};

					skeletonInvocation->retType = VD->getType();
					skeletonInvocation->variableName = VD->getNameAsString();

					skeletonInvocation->invloc = FD->getPointOfInstantiation();
					skeletonInvocation->defloc = VD->getLocation();

					skeletonInvocation->invLocationString = FD->getPointOfInstantiation().printToString(Context->getSourceManager()) ;
					skeletonInvocation->defLocationString = VD->getLocation().printToString(Context->getSourceManager());
					
					SkeletonInvocations.push_back(skeletonInvocation);
				}
			}
		}	
	}
	return true;
}

bool SkePUASTVisitor::VisitCXXOperatorCallExpr(CXXOperatorCallExpr *c)
{
	return RecursiveASTVisitor<SkePUASTVisitor>::VisitCallExpr(c);
}


// Implementation of the ASTConsumer interface for reading an AST produced by the Clang parser.
SkePUASTConsumer::SkePUASTConsumer(ASTContext *ctx, std::unordered_set<clang::VarDecl *> &instanceSet)
: Visitor(ctx, instanceSet)
{}

// Override the method that gets called for each parsed top-level
// declaration.
bool SkePUASTConsumer::HandleTopLevelDecl(DeclGroupRef DR)
{
	for (DeclGroupRef::iterator b = DR.begin(), e = DR.end(); b != e; ++b)
	{
		Visitor.TraverseDecl(*b);
	}
	return true;
}
