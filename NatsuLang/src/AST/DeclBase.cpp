#include "AST/DeclBase.h"

using namespace NatsuLib;
using namespace NatsuLang::Declaration;

namespace
{
	constexpr const char* getTypeName(Decl::DeclType type) noexcept
	{
		switch (type)
		{
#define DECL(Derived, Base) case Decl::Derived: return #Derived;
#define ABSTRACT_DECL(Decl)
#include "Basic/DeclDef.h"
		default:
			assert(!"Invalid type.");
			return "";
		}
	}
}

DeclContext* Decl::castToDeclContext(const Decl* decl)
{
	const auto type = decl->GetType();
	switch (type)
	{
#define DECL(Name, Base)
#define DECL_CONTEXT(Name) case Decl::Name: return static_cast<Name##Decl*>(const_cast<Decl*>(decl));
#define DECL_CONTEXT_BASE(Name)
#include "Basic/DeclDef.h"
	default:
#define DECL(Name, Base)
#define DECL_CONTEXT_BASE(Name) if (type >= First##Name && type <= Last##Name) return static_cast<Name##Decl*>(const_cast<Decl*>(decl));
#include "Basic/DeclDef.h"

























































































































		assert(!"Invalid type.");
		return nullptr;
	}
}

Decl* Decl::castFromDeclContext(const DeclContext* declContext)
{
	const auto type = declContext->GetType();
	switch (type)
	{
#define DECL(Name, Base)
#define DECL_CONTEXT(Name) case Decl::Name: return static_cast<Name##Decl*>(const_cast<DeclContext*>(declContext));
#define DECL_CONTEXT_BASE(Name)
#include "Basic/DeclDef.h"
	default:
#define DECL(Name, BASE)
#define DECL_CONTEXT_BASE(Name) if (type >= First##Name && type <= Last##Name) return static_cast<Name##Decl*>(const_cast<DeclContext*>(declContext));
#include "Basic/DeclDef.h"

























































































































		assert(!"Invalid type.");
		return nullptr;
	}
}

const char* Decl::GetTypeName() const noexcept
{
	return getTypeName(m_Type);
}

void Decl::SetNextDeclInContext(natRefPointer<Decl> value) noexcept
{
	m_NextDeclInContext = value;
}

const char* DeclContext::GetTypeName() const noexcept
{
	return getTypeName(m_Type);
}

Linq<DeclPtr> DeclContext::GetDecls() const
{
	return from(DeclIterator{ m_FirstDecl }, DeclIterator{});
}

void DeclContext::AddDecl(natRefPointer<Decl> decl)
{
	if (m_FirstDecl)
	{
		m_LastDecl->SetNextDeclInContext(decl);
		m_LastDecl = decl;
	}
	else
	{
		m_FirstDecl = m_LastDecl = decl;
	}

	OnNewDeclAdded(std::move(decl));
}

void DeclContext::RemoveDecl(natRefPointer<Decl> const& decl)
{
	// TODO
}

nBool DeclContext::ContainsDecl(natRefPointer<Decl> const& decl)
{
	return decl->GetContext() == this && (decl->GetNextDeclInContext() || decl == m_LastDecl);
}

DeclContext::DeclIterator::DeclIterator(natRefPointer<Decl> firstDecl)
	: m_Current{ std::move(firstDecl) }
{
}

natRefPointer<Decl> DeclContext::DeclIterator::operator*() const noexcept
{
	return m_Current;
}

DeclContext::DeclIterator& DeclContext::DeclIterator::operator++() & noexcept
{
	if (m_Current)
	{
		m_Current = m_Current->GetNextDeclInContext();
	}

	return *this;
}

nBool DeclContext::DeclIterator::operator==(DeclIterator const& other) const noexcept
{
	return m_Current == other.m_Current;
}

nBool DeclContext::DeclIterator::operator!=(DeclIterator const& other) const noexcept
{
	return m_Current != other.m_Current;
}

void DeclContext::OnNewDeclAdded(natRefPointer<Decl> /*decl*/)
{
}
