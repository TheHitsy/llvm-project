
#include <sstream>
#include <unordered_set>
#include <unordered_map>

#include "clang/Sema/Sema.h"
#include "clang/AST/AST.h"
#include "clang/AST/ASTDiagnostic.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Lex/Lexer.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Rewrite/Core/Rewriter.h"

#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"

#include "SkePUConst.h"
#include "skepu_data_structures.h"
#include "SkepuVisitor.h"
#include "skepu_code_gen_cl.h"

using namespace clang;
using namespace clang::ast_matchers;

// Defined in skepu.cpp
extern std::unordered_set<std::string> AllowedFunctionNamesCalledInUFs;

// This visitor traverses a userfunction AST node and finds references to other userfunctions and usertypes.
class UserFunctionVisitor : public RecursiveASTVisitor<UserFunctionVisitor>
{
public:

	bool VisitCallExpr(CallExpr *c)
	{
		if (isa<CXXOperatorCallExpr>(c))
			return true;
		
		// The visitor may sometimes visit the same expression twice (unclear why)
		for (auto &ref : this->UFReferences)
		{
			// Ignore repeated visits
			if (ref.first == c)
			{
				return true;
			}
		}
		
		auto callee = c->getCallee();
		
		if (ImplicitCastExpr *ImplExpr = dyn_cast<ImplicitCastExpr>(callee))
			callee = ImplExpr->IgnoreImpCasts();
		

		if (auto *UnresolvedLookup = dyn_cast<UnresolvedLookupExpr>(callee))
		{
			std::string name = UnresolvedLookup->getName().getAsString();

			bool allowed = std::find(AllowedFunctionNamesCalledInUFs.begin(), AllowedFunctionNamesCalledInUFs.end(), name)
				!= AllowedFunctionNamesCalledInUFs.end();
			if (!allowed)
				//GlobalRewriter.getSourceMgr().getDiagnostics().Report(c->getBeginLoc(), diag::err_skepu_userfunction_call) << name;

			return allowed;
		}

		FunctionDecl *Func = c->getDirectCallee();
		std::string name = Func->getName().str();
		

		if (name == "")
		{
			// CXXConversionDecl?
			if(Debug)
			{
				clangd::log("Ignored reference to function without known name.");
			}
			
		}
		else if (name == "ret")
		{
			if(Debug)
			{
				clangd::log("Ignored reference to special function: '{0}'", name );
			}
			ReferencedRets.insert(c);
		}
		else if (name == "get" || name == "getNormalized")
		{
			if(Debug)
			{
				clangd::log("Ignored reference to special function: '{0}'");
			}
			ReferencedGets.insert(dyn_cast<CXXMemberCallExpr>(c));
		}
		else if (std::find(AllowedFunctionNamesCalledInUFs.begin(), AllowedFunctionNamesCalledInUFs.end(), name) != AllowedFunctionNamesCalledInUFs.end())
		{
			// Called function is explicitly allowed
			if(Debug)
			{
				clangd::log("Ignored reference to whitelisted function: '{0}'" , name );
			}
		}
		else
		{
			if(Debug)
			{
				clangd::log("Found reference to other userfunction: '{0}'", name );
			}
			UserFunction *UF = HandleUserFunction(Func);
			UF->updateArgLists(0);
			UFReferences.emplace_back(c, UF);
			ReferencedUFs.insert(UF);
		}

		return true;
	}

	bool VisitCXXOperatorCallExpr(CXXOperatorCallExpr *c)
	{
		auto ooc = c->getOperator();
		if (ooc == OO_Subscript)
			this->containerSubscripts.push_back(c);
		if (ooc== OO_Call /* further check the type? */)
			this->containerCalls.push_back(c);
		else if (ooc == OO_Plus || ooc == OO_Minus || ooc == OO_Star || ooc == OO_Slash
			|| ooc == OO_EqualEqual || ooc == OO_ExclaimEqual
			|| ooc == OO_PlusEqual || ooc == OO_MinusEqual || ooc == OO_StarEqual || ooc == OO_SlashEqual
		)
		{
			// We start handling overloaded complex type operators here
			this->operatorOverloads.push_back(c);
		}
		return true;
	}

	bool VisitVarDecl(VarDecl *d)
	{
		if (auto *userType = HandleUserType(d->getType().getTypePtr()->getAsCXXRecordDecl()))
			ReferencedUTs.insert(userType);

		return true;
	}


	std::vector<std::pair<const CallExpr*, UserFunction*>> UFReferences{};
	std::set<UserFunction*> ReferencedUFs{};
	
	std::set<const CallExpr*> ReferencedRets{};
	std::set<CXXMemberCallExpr*> ReferencedGets{};

	std::vector<std::pair<const TypeSourceInfo*, UserType*>> UTReferences{};
	std::set<UserType*> ReferencedUTs{};

	std::vector<CXXOperatorCallExpr*> containerSubscripts{}, containerCalls{};
	std::vector<CXXOperatorCallExpr*> operatorOverloads{};
};



UserConstant::UserConstant(const VarDecl *v)
: astDeclNode(v)
{
	this->name = v->getNameAsString();
	this->typeName = v->getType().getAsString();
	
	SourceRange sr = v->getInit()->IgnoreImpCasts()->getSourceRange();
	SourceManager &SM = v->getASTContext().getSourceManager();
	clangd::Range clangdRange;
	clangdRange.start.line = SM.getSpellingLineNumber(sr.getBegin());
	clangdRange.start.character = SM.getSpellingColumnNumber(sr.getBegin());
	clangdRange.end.line = SM.getSpellingLineNumber(sr.getEnd());
	clangdRange.end.character = SM.getSpellingColumnNumber(sr.getEnd());

	if(clangdRange.start.line > 0)
	{
		clangdRange.start.line--;
		clangdRange.end.line--;
	}
	
	this->selectionRange = clangdRange;	


	SourceRange SRInit = v->getInit()->IgnoreImpCasts()->getSourceRange();
	this->definition = Lexer::getSourceText(CharSourceRange::getTokenRange(SRInit), GlobalRewriter.getSourceMgr(), LangOptions(), 0);
}


UserType::UserType(const CXXRecordDecl *t)
: astDeclNode(t), name(t->getNameAsString()), requiresDoublePrecision(false)
{
	this->type = t->getTypeForDecl();
	
	// Same as Param
	std::string rawTypeName = this->type->getCanonicalTypeInternal().getAsString();
	
	std::string resolvedTypeName = rawTypeName;
	
	// Remove 'struct'
	replaceTextInString(resolvedTypeName, "struct ", "");

	//this->typeNameOpenCL = transformToCXXIdentifier(resolvedTypeName);
	// End same as Param

	SourceRange sr = t->getSourceRange();
	SourceManager &SM = t->getASTContext().getSourceManager();
	clangd::Range clangdRange;
	clangdRange.start.line = SM.getSpellingLineNumber(sr.getBegin());
	clangdRange.start.character = SM.getSpellingColumnNumber(sr.getBegin());
	clangdRange.end.line = SM.getSpellingLineNumber(sr.getEnd());
	clangdRange.end.character = SM.getSpellingColumnNumber(sr.getEnd());

	if(clangdRange.start.line > 0)
	{
		clangdRange.start.line--;
		clangdRange.end.line--;
	}
	
	this->selectionRange = clangdRange;	
	
	
	if (const RecordDecl *r = dyn_cast<RecordDecl>(t))
	{
		for (const FieldDecl *f : r->fields())
		{
			std::string fieldName = f->getNameAsString();
			std::string typeName = f->getType().getAsString();

			if (typeName == "double")
				this->requiresDoublePrecision = true;
		}
	}
	/*
	static const std::string RunTimeTypeNameFunc = R"~~~(
	namespace skepu { template<> std::string getDataTypeCL<{{TYPE_NAME}}>() { return "struct {{TYPE_NAME}}"; } }
	namespace skepu { template<> std::string getDataTypeDefCL<{{TYPE_NAME}}>() { return {{TYPE_DEF}}; } }
	)~~~";

	if (GenCL)
	{
		std::string typeNameFunc = RunTimeTypeNameFunc;
		replaceTextInString(typeNameFunc, "{{TYPE_NAME}}", this->name);
		replaceTextInString(typeNameFunc, "{{TYPE_DEF}}", "R\"~~~(" + generateUserTypeCode_CL(*this) + ")~~~\"");
		GlobalRewriter.InsertText(t->getEndLoc().getLocWithOffset(2), typeNameFunc);
	}
	*/
}

std::string random_string(size_t length)
{
	auto randchar = []() -> char
	{
		const char charset[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
		const size_t max_index = sizeof(charset) - 1;
		return charset[ rand() % max_index ];
	};
	std::string str(length, 0);
	std::generate_n(str.begin(), length, randchar);
	return str;
}

UserFunction::Param::Param(const clang::ParmVarDecl *p)
: astDeclNode(p)
{
	this->name = p->getNameAsString();

	if (this->name == "")
		this->name = "unused_" + random_string(10);

	this->type = p->getOriginalType().getTypePtr();
	
	if (auto *referenceType = dyn_cast<ReferenceType>(type))
	{
		this->isReferenceType = true;
		if (isa<RValueReferenceType>(type)) this->isRValueReference = true;
		if (isa<LValueReferenceType>(type)) this->isLValueReference = true;
		type = referenceType->getPointeeType().getTypePtr();
	}
	
//	p->dump();
//	this->type->dump();
	
	this->rawTypeName = p->getOriginalType().getCanonicalType().getAsString();
	
	if (this->rawTypeName == "_Bool")
		this->rawTypeName = "bool";
	
	this->resolvedTypeName = this->rawTypeName;
	
	// Remove 'struct'
	replaceTextInString(this->resolvedTypeName, "struct ", "");

	this->escapedTypeName = transformToCXXIdentifier(this->resolvedTypeName);
	
	// TODO: link with UserType
	if (auto *userType = p->getOriginalType().getTypePtr()->getAs<clang::RecordType>())
		this->rawTypeName = userType->getDecl()->getNameAsString();
	
	if(Debug)
	{
		clangd::log("#================#");
		clangd::log("Param [Elwise]: {0} of type {1} resolving to {2}", this->name,  this->rawTypeName , this->resolvedTypeName);
		clangd::log("Escaped type name: {0}", this->escapedTypeName );
		clangd::log("Type name OpenCL: {0}", this->typeNameOpenCL());
		clangd::log("#================#");
	}
}

bool UserFunction::Param::constructibleFrom(const clang::ParmVarDecl *p)
{
	return !UserFunction::RandomParam::constructibleFrom(p) && !UserFunction::RegionParam::constructibleFrom(p) && !UserFunction::RandomAccessParam::constructibleFrom(p);
}

size_t UserFunction::Param::numKernelArgsCL() const
{
	return 1;
}

std::string UserFunction::Param::templateInstantiationType() const
{
	auto *type = this->astDeclNode->getOriginalType().getTypePtr();
	if (auto *innertype = dyn_cast<ElaboratedType>(type))
		type = innertype->getNamedType().getTypePtr();
	const auto *templateType = dyn_cast<TemplateSpecializationType>(type);
	return templateType->template_arguments()[0].getAsType().getAsString();
}


UserFunction::RandomAccessParam::RandomAccessParam(const ParmVarDecl *p)
: Param(p)
{
	this->fullTypeName = this->resolvedTypeName;
	this->unqualifiedFullTypeName = p->getOriginalType().getUnqualifiedType().getCanonicalType().getAsString();
	QualType underlying = p->getOriginalType();
	std::string qualifier;

	if (underlying.isConstQualified())
	{
		underlying = underlying.getUnqualifiedType();
		qualifier = "const";
		if (p->hasAttr<SkepuOutAttr>())
		{
			//GlobalRewriter.getSourceMgr().getDiagnostics().Report(p->getAttr<SkepuOutAttr>()->getRange().getBegin(), diag::err_skepu_invalid_out_attribute) << this->name;
		}

		this->accessMode = AccessMode::Read;
		if(Debug)
		{
			clangd::log("Read only access mode");
		}
	}
	else if (p->hasAttr<SkepuOutAttr>())
	{
		this->accessMode = AccessMode::Write;
		if(Debug)
		{
			clangd::log("write only access mode");
		}
	}
	else
	{
		this->accessMode = AccessMode::ReadWrite;
		if(Debug)
		{
			clangd::log("readwrite only access mode");
		}
	}

	auto *type = underlying.getTypePtr();

	if (auto *innertype = dyn_cast<ElaboratedType>(type))
		type = innertype->getNamedType().getTypePtr();

	const auto *templateType = dyn_cast<TemplateSpecializationType>(type);
	const clang::TemplateArgument containedTypeArg = templateType->template_arguments()[0];

	std::string templateName = templateType->getTemplateName().getAsTemplateDecl()->getNameAsString();
	this->containedType = containedTypeArg.getAsType().getTypePtr();
	
	
//	this->containedType->dump();
	
	if (auto *typedeft = dyn_cast<TypedefType>(this->containedType))
		this->containedType = typedeft->desugar().getTypePtr();
		
	if (auto *innertype = dyn_cast<ElaboratedType>(this->containedType))
		this->containedType = innertype->getNamedType().getTypePtr();
		
//	this->containedType->dump();
	this->resolvedTypeName = this->containedType->getCanonicalTypeInternal().getAsString();
	if(Debug)
	{
		clangd::log("Candidate type name: {0}", this->resolvedTypeName);
	}
	replaceTextInString(this->resolvedTypeName, "struct ", "");
	this->rawTypeName = this->resolvedTypeName;
	
	this->escapedTypeName = transformToCXXIdentifier(this->resolvedTypeName);
	
	if (templateName == "SparseMat")
	{
		this->containerType = ContainerType::SparseMatrix;
		this->accessMode = AccessMode::Read; // Override for sparse matrices
		
		if(Debug)
		{
			clangd::log("Sparse Matrix of {0} ", this->resolvedTypeName);
		}
	}
	else if (templateName == "Mat")
	{
		this->containerType = ContainerType::Matrix;
		if(Debug)
		{
			clangd::log("Matrix of {0}",this->resolvedTypeName);
		}
	}
	else if (templateName == "MatRow")
	{
		this->containerType = ContainerType::MatRow;
		if(Debug)
		{
			clangd::log("Matrix row of {0}",this->resolvedTypeName);
		}
	}
	else if (templateName == "MatCol")
	{
		this->containerType = ContainerType::MatCol;
		if(Debug)
		{
			clangd::log("Matrix column of {0}",this->resolvedTypeName);
		}
	}
	else if (templateName == "Vec")
	{
		this->containerType = ContainerType::Vector;
		if(Debug)
		{
			clangd::log("vector of {0}",this->resolvedTypeName);
		}
	}
	else if (templateName == "Ten3")
	{
		this->containerType = ContainerType::Tensor3;
		if(Debug)
		{
			clangd::log("tensor3 of {0}",this->resolvedTypeName);
		}
	}
	else if (templateName == "Ten4")
	{
		this->containerType = ContainerType::Tensor4;
		if(Debug)
		{
			clangd::log("tensor4 of {0}",this->resolvedTypeName);
		}
	}
	else if (templateName == "Region1D")
	{
		this->containerType = ContainerType::Region1D;
		if(Debug)
		{
			clangd::log("region1d of {0}",this->resolvedTypeName);
		}
	}
	else if (templateName == "Region2D")
	{
		this->containerType = ContainerType::Region2D;
		if(Debug)
		{
			clangd::log("region2d of {0}",this->resolvedTypeName);
		}
	}
	else if (templateName == "Region3D")
	{
		this->containerType = ContainerType::Region3D;
		if(Debug)
		{
			clangd::log("region3d of {0}",this->resolvedTypeName);
		}
	}
	else if (templateName == "Region4D")
	{
		this->containerType = ContainerType::Region4D;
		if(Debug)
		{
			clangd::log("region4d of {0}",this->resolvedTypeName);
		}
	}
	else
		SkePUAbort("Unhandled proxy type");

	if(Debug)
	{
		clangd::log("#-------------");
		clangd::log("Param [RandomAccess]: {0} of type {1} resolving to {2} (or fully {3})",this->name, this->rawTypeName , this->resolvedTypeName, this->fullTypeName);
		clangd::log("Escaped type name: {0} ", this->escapedTypeName);
		clangd::log("Type name OpenCL: {0}",this->typeNameOpenCL());
		clangd::log("#-------------");
	}
}

bool UserFunction::RandomAccessParam::constructibleFrom(const clang::ParmVarDecl *p)
{
	auto *type = p->getOriginalType().getTypePtr();
	
	if (auto *referenceType = dyn_cast<ReferenceType>(type))
		type = referenceType->getPointeeType().getTypePtr();
	
	if (auto *innertype = dyn_cast<ElaboratedType>(type))
		type = innertype->getNamedType().getTypePtr();

	const auto *templateType = dyn_cast<TemplateSpecializationType>(type);
	if (!templateType) return false;

	std::string templateName = templateType->getTemplateName().getAsTemplateDecl()->getNameAsString();
	return (templateName == "SparseMat") || (templateName == "Mat") || (templateName == "Vec")
		|| (templateName == "Ten3")  || (templateName == "Ten4") || (templateName == "MatRow") || (templateName == "MatCol");
}

UserFunction::RegionParam::RegionParam(const ParmVarDecl *p)
: RandomAccessParam(p)
{}

bool UserFunction::RegionParam::constructibleFrom(const clang::ParmVarDecl *p)
{
	auto *type = p->getOriginalType().getTypePtr();
	
	if (auto *referenceType = dyn_cast<ReferenceType>(type))
		type = referenceType->getPointeeType().getTypePtr();
	
	if (auto *innertype = dyn_cast<ElaboratedType>(type))
		type = innertype->getNamedType().getTypePtr();

	const auto *templateType = dyn_cast<TemplateSpecializationType>(type);
	if (!templateType) return false;

	std::string templateName = templateType->getTemplateName().getAsTemplateDecl()->getNameAsString();
	if(Debug)
	{
		clangd::log(templateName.c_str());
	}

	return (templateName == "Region1D") || (templateName == "Region2D") || (templateName == "Region3D")
		|| (templateName == "Region4D");
	
	if(Debug)
	{
		clangd::log("#++++++++++++++++#");
		clangd::log("Param [Region]");
		clangd::log("#++++++++++++++++#");
	}
}

UserFunction::RandomParam::RandomParam(const ParmVarDecl *p)
: Param(p)
{
	if (!this->isReferenceType)
		SkePUAbort("Random parameter must be of reference type.");
	
	auto *type = p->getOriginalType().getTypePtr();
	
	if (auto *referenceType = dyn_cast<ReferenceType>(type))
		type = referenceType->getPointeeType().getTypePtr();
	
	if (auto *innertype = dyn_cast<ElaboratedType>(type))
		type = innertype->getNamedType().getTypePtr();
	
	const auto *templateType = dyn_cast<TemplateSpecializationType>(type);
	if (templateType->template_arguments().size() > 0)
		this->randomCount = templateType->template_arguments()[0].getAsExpr()->EvaluateKnownConstInt(p->getASTContext()).getExtValue();
	else
		this->randomCount = 0;
}

bool UserFunction::RandomParam::constructibleFrom(const clang::ParmVarDecl *p)
{
	auto *type = p->getOriginalType().getTypePtr();
	
	if (auto *referenceType = dyn_cast<ReferenceType>(type))
		type = referenceType->getPointeeType().getTypePtr();
	
	if (auto *innertype = dyn_cast<ElaboratedType>(type))
		type = innertype->getNamedType().getTypePtr();
		
	const auto *templateType = dyn_cast<TemplateSpecializationType>(type);
	if (!templateType) return false;

	std::string templateName = templateType->getTemplateName().getAsTemplateDecl()->getNameAsString();
	if(Debug)
	{
		clangd::log(templateName.c_str());
	}
	return templateName == "Random";
}

std::string UserFunction::RandomAccessParam::TypeNameOpenCL() const
{
	switch (this->containerType)
	{
		case ContainerType::Vector:
			return "skepu_vec_proxy_" + this->innerTypeNameOpenCL();
		case ContainerType::Matrix:
			return "skepu_mat_proxy_" + this->innerTypeNameOpenCL();
		case ContainerType::MatRow:
			return "skepu_matrow_proxy_" + this->innerTypeNameOpenCL();
		case ContainerType::MatCol:
				return "skepu_matcol_proxy_" + this->innerTypeNameOpenCL();
		case ContainerType::Tensor3:
			return "skepu_ten3_proxy_" + this->innerTypeNameOpenCL();
		case ContainerType::Tensor4:
			return "skepu_ten4_proxy_" + this->innerTypeNameOpenCL();
		case ContainerType::SparseMatrix:
			return "skepu_sparse_mat_proxy_" + this->innerTypeNameOpenCL();
		default:
			SkePUAbort("ERROR: TypeNameOpenCL: Invalid switch value");
			return "";
	}
}

std::string UserFunction::Param::typeNameOpenCL() const
{
	if (this->resolvedTypeName.find("skepu::complex") != std::string::npos)
	{
		return "skepu_complex_float"; // todo fix templated type
	}
	
	return this->escapedTypeName;
}

std::string UserFunction::RandomAccessParam::innerTypeNameOpenCL() const
{
	if (this->resolvedTypeName.find("skepu::complex") != std::string::npos)
	{
		return "skepu_complex_float"; // todo fix templated type
	}
	
	return this->escapedTypeName;
}




std::string UserFunction::RandomAccessParam::TypeNameHost() const
{
	switch (this->containerType)
	{
		case ContainerType::Vector:
			return "std::tuple<skepu::Vector<" + this->resolvedTypeName + "> *, skepu::backend::DeviceMemPointer_CL<" + this->resolvedTypeName + "> *>";
		case ContainerType::Matrix:
		case ContainerType::MatRow:
		case ContainerType::MatCol:
			return "std::tuple<skepu::Matrix<" + this->resolvedTypeName + "> *, skepu::backend::DeviceMemPointer_CL<" + this->resolvedTypeName + "> *>";
		case ContainerType::Tensor3:
			return "std::tuple<skepu::Tensor3<" + this->resolvedTypeName + "> *, skepu::backend::DeviceMemPointer_CL<" + this->resolvedTypeName + "> *>";
		case ContainerType::Tensor4:
			return "std::tuple<skepu::Tensor4<" + this->resolvedTypeName + "> *, skepu::backend::DeviceMemPointer_CL<" + this->resolvedTypeName + "> *>";
		case ContainerType::SparseMatrix:
			return "std::tuple<skepu::SparseMatrix<" + this->resolvedTypeName + "> *, skepu::backend::DeviceMemPointer_CL<" + this->resolvedTypeName + "> *, "
				+ "skepu::backend::DeviceMemPointer_CL<size_t> *, skepu::backend::DeviceMemPointer_CL<size_t> *>";
		default:
			SkePUAbort("ERROR: TypeNameHost: Invalid switch value");
			return "";
	}
}

size_t UserFunction::RandomAccessParam::numKernelArgsCL() const
{
	switch (this->containerType)
	{
		case ContainerType::Vector:
		case ContainerType::MatRow:
		case ContainerType::MatCol:
			return 2;
		case ContainerType::Matrix:
			return 3;
		case ContainerType::Tensor3:
			return 4;
		case ContainerType::Tensor4:
			return 5;
		case ContainerType::SparseMatrix:
			return 4;
		default:
			SkePUAbort("ERROR: numKernelArgsCL: Invalid switch value");
			return 0;
	}
}




UserFunction::TemplateArgument::TemplateArgument(std::string name, std::string rawType, std::string resolvedType)
: paramName(name), rawTypeName(rawType), resolvedTypeName(resolvedType)
{}


UserFunction::UserFunction(CXXMethodDecl *f, VarDecl *d)
: UserFunction(f)
{
	this->rawName = this->uniqueName = "lambda_uf_" + random_string(10);

	this->codeLocation = d->getSourceRange().getBegin();
	if (const FunctionDecl *DeclCtx = dyn_cast<FunctionDecl>(d->getDeclContext()))
		this->codeLocation = DeclCtx->getSourceRange().getBegin();
}

UserFunction::UserFunction(clang::FunctionDecl *f)
: astDeclNode(f), requiresDoublePrecision(false)
{
	// Function name
	this->rawName = f->getNameInfo().getName().getAsString();

	// Code location
	this->codeLocation = f->getSourceRange().getEnd().getLocWithOffset(1);

	SourceManager &SM = f->getASTContext().getSourceManager();

	FullSourceLoc startLoc(f->getSourceRange().getBegin(), SM);
	FullSourceLoc endLoc(f->getSourceRange().getEnd(), SM);

	clangd::Range clangdRange;
	clangdRange.start.line = startLoc.getLineNumber();
	clangdRange.start.character =startLoc.getColumnNumber();
	clangdRange.end.line = endLoc.getLineNumber();
	clangdRange.end.character = endLoc.getColumnNumber();

	this->range = clangdRange;

	clangdRange.start.line = SM.getSpellingLineNumber(f->getNameInfo().getBeginLoc());
	clangdRange.start.character = SM.getSpellingColumnNumber(f->getNameInfo().getBeginLoc());
	clangdRange.end.line = SM.getSpellingLineNumber(f->getNameInfo().getEndLoc());
	clangdRange.end.character = SM.getSpellingColumnNumber(f->getNameInfo().getEndLoc());
	
	this->selectionRange = clangdRange;	

	std::stringstream SSUniqueName;
	SSUniqueName << this->rawName;

	// Handle userfunction templates
	if (f->getTemplatedKind() == FunctionDecl::TK_FunctionTemplateSpecialization)
	{
		this->fromTemplate = true;
		const FunctionTemplateDecl *t = f->getPrimaryTemplate();
		const TemplateParameterList *TPList = t->getTemplateParameters();
		const TemplateArgumentList *TAList = f->getTemplateSpecializationArgs();

		for (unsigned i = 0; i < TPList->size(); ++i)
		{
			if (TAList->get(i).getKind() != clang::TemplateArgument::ArgKind::Type)
			{
				if(Debug)
				{
					clangd::log("Note: Ignored non-type template parameter");
				}
		//		TPList->getParam(i)->dump();
				continue;
			}
			
			std::string paramName = TPList->getParam(i)->getNameAsString();
			std::string rawArgName = TAList->get(i).getAsType().getAsString();
			std::string resolvedArgName = rawArgName;
			if (auto *userType = TAList->get(i).getAsType().getTypePtr()->getAs<clang::RecordType>())
				rawArgName = userType->getDecl()->getNameAsString();
			
			// Remove 'struct'
			replaceTextInString(resolvedArgName, "struct ", "");
			
			this->templateArguments.emplace_back(paramName, rawArgName, resolvedArgName);
			SSUniqueName << "_" << transformToCXXIdentifier(rawArgName);
			if(Debug)
			{
				clangd::log("Template param: {0} = {1} resolving to {2}", paramName, rawArgName , resolvedArgName );
			}
		}
	}

	this->uniqueName = SSUniqueName.str();
	if(Debug)
	{
		clangd::log("### [UF] Created UserFunction object with unique name {0}", this->uniqueName);
	}

	if (!f->doesThisDeclarationHaveABody())
		SkePUAbort("Fatal error: Did not find a body for function '" + this->rawName + "' called inside user function. If this is a common library function, you can override SkePU transformation check by using the -fnames argument.");
	//	GlobalRewriter.getSourceMgr().getDiagnostics().Report(f->getSourceRange().getEnd(), diag::err_skepu_no_userfunction_body) << this->rawName; // Segfault here

	// Type name as string
	this->rawReturnTypeName = f->getReturnType().getCanonicalType().getAsString();
	
	if (this->rawReturnTypeName == "_Bool")
		this->rawReturnTypeName = "bool";
	
	
	
	// Look for multiple return values (skepu::multiple)
	if (this->rawReturnTypeName.find("skepu::multiple") != std::string::npos || this->rawReturnTypeName.find("::tuple<") != std::string::npos)
	{
		if(Debug)
		{
			clangd::log("  [UF {0}] Identified multi-valued return!", this->uniqueName);
		}
		
	//	if (GenCUDA || GenCL)
	//		SkePUAbort("Multi-valued return is not enabled for GPU backends.");
		
		const auto *templateType = f->getReturnType().getTypePtr()->getAs<clang::TemplateSpecializationType>();
		for (const clang::TemplateArgument &arg : templateType->template_arguments())
		{
			std::string argType = arg.getAsType().getAsString();
		//	clangd::log("    [UF {0}] Multi-return type: {1}" , this->uniqueName, argType );
			this->multipleReturnTypes.push_back(argType);
		}
		
		std::string resolvedCompoundTypeName = "std::tuple<";
		for (size_t i = 0; i < this->multipleReturnTypes.size(); ++i)
		{
			resolvedCompoundTypeName += this->multipleReturnTypes[i];
			if (i != multipleReturnTypes.size() - 1) resolvedCompoundTypeName += ",";
		}
		resolvedCompoundTypeName += ">";
		this->resolvedReturnTypeName = resolvedCompoundTypeName;
	}
	else
	{
		this->resolvedReturnTypeName = this->rawReturnTypeName;
		
		if (auto *userType = f->getReturnType().getTypePtr()->getAs<clang::RecordType>())
			this->rawReturnTypeName = userType->getDecl()->getNameAsString();
		
		// Returning a templated type, resolve it
		for (UserFunction::TemplateArgument &arg : this->templateArguments)
			if (this->rawReturnTypeName == arg.paramName)
				this->resolvedReturnTypeName = arg.resolvedTypeName;
	}
	
	// remove 'struct'
	replaceTextInString(this->resolvedReturnTypeName, "struct ", "");
	if(Debug)
	{
		clangd::log("  [UF {0}] Return type: {1} resolving to: {2}", this->uniqueName , this->rawReturnTypeName , this->resolvedReturnTypeName );
	}

	// Argument lists
	auto it = f->param_begin();

	if (f->param_size() > 0)
	{
		std::string name = (*it)->getOriginalType().getAsString();
		this->indexed1D = (name == "skepu::Index1D" || name == "struct skepu::Index1D");
		this->indexed2D = (name == "skepu::Index2D" || name == "struct skepu::Index2D");
		this->indexed3D = (name == "skepu::Index3D" || name == "struct skepu::Index3D");
		this->indexed4D = (name == "skepu::Index4D" || name == "struct skepu::Index4D");
	}

	if (this->indexed1D || this->indexed2D || this->indexed3D || this->indexed4D)
		this->indexParam = new UserFunction::Param(*it++);

	// Referenced functions and types
	UserFunctionVisitor UFVisitor;
	UFVisitor.TraverseDecl(this->astDeclNode);

	this->ReferencedUFs = UFVisitor.ReferencedUFs;
	this->UFReferences = UFVisitor.UFReferences;

	this->ReferencedUTs = UFVisitor.ReferencedUTs;
	this->UTReferences = UFVisitor.UTReferences;
	
	this->ReferencedRets = UFVisitor.ReferencedRets;
	this->ReferencedGets = UFVisitor.ReferencedGets;

	this->containerSubscripts = UFVisitor.containerSubscripts;
	this->containerCalls = UFVisitor.containerCalls;
	
	this->operatorOverloads = UFVisitor.operatorOverloads;
	
	if(Debug)
	{
		clangd::log("| Traversal analysis summary for UF {0} ", this->uniqueName );
		clangd::log("| {0} total UF references" , this->UFReferences.size());
		clangd::log("| {0} unique referenced UTs" , this->ReferencedUTs.size());
		clangd::log("| {0} total UT references" , this->UTReferences.size());
		clangd::log("| {0} ret() expressions" , this->ReferencedRets.size());
		clangd::log("| {0}  get() expressions (PRNG)" , this->ReferencedGets.size());
		clangd::log("| {0} container indexing calls" , this->containerCalls.size());
	}

	// Set requires double precision (TODO: more cases like parameters etc...)
	if (this->resolvedReturnTypeName == "double")
		this->requiresDoublePrecision = true;

	for (UserType *UT : this->ReferencedUTs)
		if (UT->requiresDoublePrecision)
			this->requiresDoublePrecision = true;
}

std::string UserFunction::funcNameCUDA()
{
	return SkePU_UF_Prefix + this->instanceName + "_" + this->uniqueName + "::CU";
}

std::string UserFunction::returnTypeNameOpenCL()
{
	if (this->resolvedReturnTypeName.find("skepu::complex") != std::string::npos)
	{
		return "skepu_complex_float"; // todo fix templated type
	}
	
	return this->rawReturnTypeName;
}

std::string UserFunction::multiReturnTypeNameGPU()
{
	std::string multiReturnType = "skepu_multiple";
		for (std::string &type : this->multipleReturnTypes)
			multiReturnType += "_" + transformToCXXIdentifier(type);
	return multiReturnType;
}


size_t UserFunction::numKernelArgsCL()
{
	size_t count = std::max<size_t>(1, this->multipleReturnTypes.size());
	
	if (this->randomParam)
		count += 1;
	if (this->regionParam)
		count += 1;

	for (Param &p : this->elwiseParams)
		count += p.numKernelArgsCL();
	for (Param &p : this->anyContainerParams)
		count += p.numKernelArgsCL();
	for (Param &p : this->anyScalarParams)
		count += p.numKernelArgsCL();

	return count;
}

bool UserFunction::refersTo(UserFunction &other)
{
	// True if this is the other user function
	if (this == &other)
		return true;

	// True any of this's directly refered userfunctions refers to other
	for (auto *uf : this->ReferencedUFs)
		if (uf->refersTo(other))
			return true;

	return false;
}

void UserFunction::updateArgLists(size_t arity, size_t Harity)
{
	if(Debug)
	{
		clangd::log("Trying with arity: {0} ", arity );
	}
	
	this->Varity = arity;
	this->Harity = Harity;

	this->randomParam = nullptr;
	this->regionParam = nullptr;
	this->elwiseParams.clear();
	this->anyContainerParams.clear();
	this->anyScalarParams.clear();

	auto it = this->astDeclNode->param_begin();
	const auto end = this->astDeclNode->param_end();

	if (this->indexed1D || this->indexed2D || this->indexed3D || this->indexed4D)
		it++;
	
	if (it != end && UserFunction::RandomParam::constructibleFrom(*it))
	{
		this->randomParam = new UserFunction::RandomParam(*it++);
		this->randomCount = this->randomParam->randomCount;
	}
	
	auto elwise_end = it + arity + Harity;
	
	if (it != end && UserFunction::RegionParam::constructibleFrom(*it))
		this->regionParam = new UserFunction::RegionParam(*it++);
	
	while (it != end && it != elwise_end && UserFunction::Param::constructibleFrom(*it))
		this->elwiseParams.emplace_back(*it++);

	while (it != end && UserFunction::RandomAccessParam::constructibleFrom(*it))
		this->anyContainerParams.emplace_back(*it++);

	while (it != end)
		this->anyScalarParams.emplace_back(*it++);

	// Find references to user types
	auto scanForType = [&] (const Type *p)
	{
		if (auto *userType = HandleUserType(p->getAsCXXRecordDecl()))
			ReferencedUTs.insert(userType);
	};

	for (auto &param : this->elwiseParams)
		scanForType(param.type);

	for (auto &param : this->anyContainerParams)
		scanForType(param.containedType);

	for (auto &param : this->anyScalarParams)
		scanForType(param.type);

	if(Debug)
	{
		clangd::log("Deduced Indexed: {0}",(this->indexParam ? "yes" : "no") );
		clangd::log("Found random parameter: {0}",(this->randomParam ? "yes" : "no") );
		clangd::log("Found region parameter: {0}",(this->regionParam ? "yes" : "no") );
		clangd::log("Deduced elementwise arity: {0}",(this->elwiseParams.size() ? "yes" : "no") );
		clangd::log("Deduced random access arity: {0}",(this->anyContainerParams.size() ? "yes" : "no") );
		clangd::log("Deduced scalar arity: {0}",(this->anyScalarParams.size() ? "yes" : "no") );
	}
}

void Skeleton::printVariables()	
{
	clangd::log("The Skeleton of {0} has {1} arguments, {2} type, variableName: {3}, at location: {4}, retType: {5}, variableName: {6}, it has {7} invocations ",
	this->name, this->userfunctionArgAmount, 
	(int)this->type, this->variableName, 
	this->locationString, this->retType.getAsString(),
	this->variableName, this->invocations.size()
	);

	for(auto invocation : this->invocations)
	{
		invocation->printVars();
	}
}

std::string UserFunction::getName()
{
	// TODO: kontrollera om det är flera return typer samt argument
	std::ostringstream oss;
	oss << this->rawName << "(";
	//<< this->elwiseParams.size() << " "<< this->anyContainerParams.size() << " " << this->anyScalarParams.size()
	
	for(const UserFunction::Param& param : this->elwiseParams)
	{
		if (&param != &this->elwiseParams.front()) // To avoid adding comma before the first element
            oss << ", ";
        oss << param.rawTypeName;
	}

	oss << ")";
	return oss.str();
}

std::string Skeleton::getDetails() const
{
	std::ostringstream oss;
	oss << this->name << "(";
	
	for(UserFunction* param : this->parameters)
	{
		if (param != this->parameters.front())
            oss << ", ";
        oss << param->resolvedReturnTypeName;
	}

	oss << ")";
	return oss.str();
}

void Skeleton::constructLayout()
{
	std::string IndexND = "";
	if(this->parameters.back()->indexed1D)
	{
		IndexND = "Vector";
	}
	else if(this->parameters.back()->indexed2D)
	{
		IndexND = "Matrix";
	}
	else if(this->parameters.back()->indexed3D)
	{
		IndexND = "Tensor3";
	}else if(this->parameters.back()->indexed4D)
	{
		IndexND = "Tensor4";
	}

	std::string retType = "";
	if(!this->parameters.empty())
	{
		retType = this->parameters.back()->resolvedReturnTypeName;
	}

	std::string tmpParam = retType;
	std::vector<std::string> retTypes;
	if (tmpParam.find("tuple") != std::string::npos) {
		// Multiret
		std::regex pattern("<([^<>]+)>");
		// Iterator for matching
		std::sregex_iterator iter(tmpParam.begin(), tmpParam.end(), pattern);
		std::sregex_iterator end;

		if (iter != end) {
			std::smatch match = *iter;
			std::string keywords = match.str(1); // Extracting the captured group

			// Splitting the keywords based on comma (',') delimiter
			std::regex comma_pattern("\\s*,\\s*");
			std::sregex_token_iterator key_iter(keywords.begin(), keywords.end(), comma_pattern, -1);
			std::sregex_token_iterator key_end;

			// Storing each keyword in a vector and printing
			std::vector<std::string> keyword_vector;
			while (key_iter != key_end) {
				std::string keyword = *key_iter;
				retTypes.push_back(keyword);
				++key_iter;
			}
		}
	}
	else
	{
		retTypes.push_back(tmpParam);
	}

	std::string tmp{};

	if(this->name == "Map" || this->name == "MapPairs" 
	|| this->name == "MapPairsReduce" || this->name == "MapOverlap1D"
	|| this->name == "MapOverlap2D" || this->name == "MapOverlap3D"
	|| this->name == "MapOverlap4D")
	{			
		int counter{0};
		for(auto const& elem : retTypes)
		{
			std::string resvParam{};
			if(IndexND != "")
			{
				resvParam = "Return: skepu::" + IndexND + "<" +  elem + ">& Return" + std::to_string(counter);
			}
			else 
			{
				resvParam = "Return: " + elem + "& Return" + std::to_string(counter) + " | (Can be skepu container)";
			}
			this->resolvedParams.push_back(resvParam) ;
			++counter;
		}
		this->resolvedReturnType = "void";
	}
	else 
	{
		if(!retTypes.empty())
		{
			this->resolvedReturnType = retTypes[0];
		}
		else 
		{
			this->resolvedReturnType = "void";
		}
	}

	if(!this->parameters.empty())
	{
		for(auto const& elem : this->parameters[0]->elwiseParams)
		{
			std::string resvParam{};
			if(IndexND != "")
			{
				resvParam = "skepu::" + IndexND + "<" + elem.resolvedTypeName + "> " + elem.name;
			}
			else 
			{
				resvParam = elem.resolvedTypeName + " "+ elem.name + " | (Can be skepu container of " + elem.resolvedTypeName + " type)";
			}
			this->resolvedParams.push_back(resvParam) ;
			//std::string elemStr = "skepu::" + IndexND + "<" + elem.resolvedTypeName + "> " + elem.name;
			//this->resolvedParams.push_back(elemStr);
		}

		for(auto const& elem : this->parameters[0]->anyContainerParams)
		{
			std::string elemStr ="skepu::"+ this->parameters[0]->containerTypeToString(elem.containerType)+"<"+elem.resolvedTypeName + "> " + elem.name;
			this->resolvedParams.push_back(elemStr);
		}

		for(auto const& elem : this->parameters[0]->anyScalarParams)
		{
			std::string elemStr = elem.resolvedTypeName + " " + elem.name;
			this->resolvedParams.push_back(elemStr);
		}
	}
}

const std::string Skeleton::getReturn() const
{
	std::string tmp{};

	
	switch(this->type)
	{
		case Skeleton::Type::Map:
			tmp = this->parameters.back()->resolvedReturnTypeName;
			break;
		case Skeleton::Type::Reduce1D:
			break;
		case Skeleton::Type::Reduce2D:
			break;
		case Skeleton::Type::MapReduce:
			tmp = this->parameters.back()->resolvedReturnTypeName;
			break;
		case Skeleton::Type::MapPairs:
			break;
		case Skeleton::Type::MapPairsReduce:
			break;
		case Skeleton::Type::Scan:
			break;
		case Skeleton::Type::MapOverlap1D:
			break;
		case Skeleton::Type::MapOverlap2D:
			break;
		case Skeleton::Type::MapOverlap3D:
			break;
		case Skeleton::Type::MapOverlap4D:
			break;
		case Skeleton::Type::Call:
			break;
		default:
			break;
	}

	return tmp;
}

void SkepuContainer::print()
{
	clangd::log("skepu container: type: {0}, typenameType: {1}, variableName: {2}, location: {3}",
	this->type, this->typenameType, this->variableName, this->locationString);
}


const std::string Skeleton::getDescription() const
{
	return description;
	/*
	std::string tmp{};
	tmp += this->name + " is a skeleton of type " + this->name;S
	tmp += " which requires " + std::to_string(this->userfunctionArgAmount) + " argument(s): \n";

	if(!arguments.empty())
	{
		tmp += "Takes the function templates:";
	}

	
	for(auto& arg : arguments)
	{	
		if(clang::FunctionDecl *FD = arg->astDeclNode)
		{
			tmp += FD->getReturnType().getAsString() + " " + FD->getNameAsString() + "<";
			for (const ParmVarDecl *param : FD->parameters()) {
				tmp += param->getType().getAsString();

				if(param != FD->parameters().back())
				{
					tmp += ", ";
				}
			}

			tmp += ">\n";
		}
	}


	tmp += " and returns " + this->retType.getAsStri	ng();
	this->InvDesc = tmp;
	*/
}


void SkeletonInvocation::printVars() const
{
	
	clangd::log("SkeletonInvocation: name: {0}, retType: {1}",
	this->variableName, this->retType.getAsString()
	);
	
}

